//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

#include <atomic>
#include <chrono>
#include <functional>

#include "Bluetooth.h"
#include "AppleCP.h"

namespace Core::AirPods {

//
// Structures
//

namespace Details {

struct BasicState {
    Battery battery;
    bool isCharging{false};

    bool operator==(const BasicState &rhs) const = default;
};
} // namespace Details

struct PodState : Details::BasicState {
    bool isInEar{false};

    bool operator==(const PodState &rhs) const = default;
};

struct CaseState : Details::BasicState {
    bool isBothPodsInCase{false};
    bool isLidOpened{false};

    bool operator==(const CaseState &rhs) const = default;
};

struct PodsState {
    PodState left, right;

    bool operator==(const PodsState &rhs) const = default;
};

struct State {
    Model model{Model::Unknown};
    PodsState pods;
    CaseState caseBox;
    QString displayName;

    bool operator==(const State &rhs) const = default;

    // Whether this side of the advertisement carries any meaningful pod data.
    // AirPods 4 firmware may stop filling in the legacy battery nibbles (0xF)
    // while the in-ear/charging bits stay valid.
    //
    bool LeftHasInfo() const
    {
        return pods.left.battery.Available() || pods.left.isInEar || pods.left.isCharging;
    }

    bool RightHasInfo() const
    {
        return pods.right.battery.Available() || pods.right.isInEar || pods.right.isCharging;
    }
};

//
// Classes
//

namespace Details {

class Advertisement
{
public:
    using AddressType = decltype(Bluetooth::AdvertisementWatcher::ReceivedData::address);

    struct AdvState : AirPods::State {
        Side side;

        // Raw per-advertisement in-ear bits (pre-gating): the broadcasting pod's
        // own ear ("current") and the other pod's ear ("another"). See
        // StateManager::UpdateState, which only trusts "another" once the pod
        // that owns it has stopped broadcasting.
        //
        bool rawCurrInEar{false};
        bool rawAnotInEar{false};

        // Raw per-advertisement charging bits: the broadcasting pod's own
        // charging status ("current") and the other pod's ("another").
        // Firmware often sets anotCharging when the other pod is in the case,
        // so only trust currCharging from the actively broadcasting pod.
        //
        bool rawCurrCharging{false};
        bool rawAnotCharging{false};
    };

    static bool IsDesiredAdv(const Bluetooth::AdvertisementWatcher::ReceivedData &data);

    Advertisement(const Bluetooth::AdvertisementWatcher::ReceivedData &data);

    int16_t GetRssi() const;
    const auto &GetTimestamp() const;
    AddressType GetAddress() const;
    std::vector<uint8_t> GetDesensitizedData() const;
    const AdvState &GetAdvState() const;

private:
    Bluetooth::AdvertisementWatcher::ReceivedData _data;
    AppleCP::AirPods _protocol;
    AdvState _state;

    const std::vector<uint8_t> &GetMfrData() const;
};

// AirPods use Random Non-resolvable device addresses for privacy reasons. This means we
// can't "Remember" the user's AirPods by any device property. Here we track our desired
// devices in some non-elegant ways, but obviously it is sometimes unreliable.
//
class StateManager
{
public:
    struct UpdateEvent {
        std::optional<State> oldState;
        State newState;
    };

    StateManager();

    std::optional<State> GetCurrentState() const;

    std::optional<UpdateEvent> OnAdvReceived(Advertisement adv);
    void Disconnect();

    void OnRssiMinChanged(int16_t rssiMin);

    // Called (while the internal mutex is held) when the last known state is
    // discarded because the device is lost, disconnects, or stops broadcasting.
    //
    void SetOnStateLost(std::function<void()> callback);

private:
    using Clock = std::chrono::steady_clock;
    using Timestamp = std::chrono::time_point<Clock>;

    mutable std::mutex _mutex;

    Helper::Timer _lostTimer;
    Helper::Sides<Helper::Timer> _stateResetTimer;
    Helper::Sides<std::optional<std::pair<Advertisement, Timestamp>>> _adv;
    std::optional<State> _cachedState;
    int16_t _rssiMin{std::numeric_limits<int16_t>::max()};

    bool IsPossibleDesiredAdv(const Advertisement &adv) const;
    void UpdateAdv(Advertisement adv);
    std::optional<UpdateEvent> UpdateState();
    void ResetAll();
    void DoLost();
    void DoStateReset(Side side);

    std::function<void()> _onStateLost;
};
} // namespace Details

class Manager
{
public:
    Manager();

    void StartScanner();
    void StopScanner();

    void OnRssiMinChanged(int16_t rssiMin);
    void OnAutomaticEarDetectionChanged(bool enable);
    void OnBoundDeviceAddressChanged(uint64_t address);

private:
    std::mutex _mutex;
    Bluetooth::AdvertisementWatcher _adWatcher;
    Details::StateManager _stateMgr;
    std::optional<Bluetooth::Device> _boundDevice;
    QString _deviceName;
    bool _deviceConnected{false};
    bool _automaticEarDetection{false};

    // Mirrors `_automaticEarDetection` for the (lock-free) state-loss path.
    std::atomic<bool> _autoEarDetectionAtomic{false};
    // -1: unknown (no advertisement processed yet), 0: not both in ear, 1: both in ear.
    std::atomic<int> _lastBothInEar{-1};

    // Automatic ear detection edge debounce: the two pods take turns broadcasting
    // and can briefly disagree about the in-ear bits while one is moved in or out,
    // which would otherwise fire pause/resume back to back. A state change is only
    // acted on once it has held stable for a short while. UpdateState only emits
    // when the decoded state CHANGES, so the debounce is driven by a QTimer
    // scheduled when the edge is first seen (see OnBothInEarDebounced), rather
    // than by waiting for further state-change events.
    //
    static constexpr std::chrono::milliseconds BothInEarDebounce{800};
    std::atomic<bool> _bothInEarDebounceActive{false};

    void OnBoundDeviceConnectionStateChanged(Bluetooth::DeviceState state, bool initial = false);
    void OnStateChanged(Details::StateManager::UpdateEvent updateEvent);
    void OnLidOpened(bool opened);
    void OnBothInEar(bool isBothInEar);
    void OnBothInEarDebounced();
    bool OnAdvertisementReceived(const Bluetooth::AdvertisementWatcher::ReceivedData &data);
    void OnAdvWatcherStateChanged(
        Bluetooth::AdvertisementWatcher::State state, const std::optional<std::string> &optError);
};

std::vector<Core::Bluetooth::Device> GetDevices();

} // namespace Core::AirPods
