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

#include "AppleCP.h"

namespace Core::AppleCP {

bool AirPods::IsValid(const std::vector<uint8_t> &data)
{
    if (data.size() != sizeof(AirPods)) {
        return false;
    }

    constexpr uint8_t shouldRemainingLength =
        sizeof(AirPods) - (offsetof(Header, remainingLength) + sizeof(Header::remainingLength));

    Header *packet = (Header *)(data.data());
    if (packet->packetType != PacketType::ProximityPairing ||
        packet->remainingLength != shouldRemainingLength)
    {
        return false;
    }

    return true;
}

Core::AirPods::Model AirPods::GetModel(uint16_t modelId)
{
    switch (modelId) {
    case 0x2002:
        return Core::AirPods::Model::AirPods_1;
    case 0x200F:
        return Core::AirPods::Model::AirPods_2;
    case 0x2013:
        return Core::AirPods::Model::AirPods_3;
    case 0x200E:
        return Core::AirPods::Model::AirPods_Pro;
    case 0x2014: // AirPods Pro 2 (Lightning)
    case 0x2024: // AirPods Pro 2 (USB-C)
        return Core::AirPods::Model::AirPods_Pro_2;
    case 0x2019: // AirPods 4
        return Core::AirPods::Model::AirPods_4;
    case 0x201B: // AirPods 4 (ANC)
        return Core::AirPods::Model::AirPods_4_ANC;
    case 0x200A: // AirPods Max
    case 0x201F: // AirPods Max (USB-C)
        return Core::AirPods::Model::AirPods_Max;
        // case 0x2003:
        //    return Core::AirPods::Model::Powerbeats_3;
        // case 0x2005:
        //    return Core::AirPods::Model::Beats_X;
        // case 0x2006:
        //    return Core::AirPods::Model::Beats_Solo3;
    default:
        return Core::AirPods::Model::Unknown;
    }
}

Core::AirPods::Side AirPods::GetBroadcastedSide() const
{
    return broadcastFrom == 1 ? Core::AirPods::Side::Left : Core::AirPods::Side::Right;
}

bool AirPods::IsLeftBroadcasted() const
{
    return GetBroadcastedSide() == Core::AirPods::Side::Left;
}

bool AirPods::IsRightBroadcasted() const
{
    return GetBroadcastedSide() == Core::AirPods::Side::Right;
}

Core::AirPods::Model AirPods::GetModel() const
{
    return GetModel(modelId);
}

Core::AirPods::Battery AirPods::GetLeftBattery() const
{
    const auto val = (IsCurrUsedForLeft() ? battery.curr : battery.anot);
    return (val >= 0 && val <= 10) ? val : Core::AirPods::Battery{};
}

Core::AirPods::Battery AirPods::GetRightBattery() const
{
    const auto val = (IsCurrUsedForLeft() ? battery.anot : battery.curr);
    return (val >= 0 && val <= 10) ? val : Core::AirPods::Battery{};
}

Core::AirPods::Battery AirPods::GetCaseBattery() const
{
    const auto val = battery.caseBox;
    return (val >= 0 && val <= 10) ? val : Core::AirPods::Battery{};
}

bool AirPods::IsLeftCharging() const
{
    return (IsCurrUsedForLeft() ? battery.currCharging : battery.anotCharging) != 0;
}

bool AirPods::IsRightCharging() const
{
    return (IsCurrUsedForLeft() ? battery.anotCharging : battery.currCharging) != 0;
}

bool AirPods::IsBothPodsInCase() const
{
    return bothInCase;
}

bool AirPods::IsLidOpened() const
{
    return lidState == 1 || lidState == 2 || lidState == 3 || lidState == 4 || lidState == 5 ||
           lidState == 6 || lidState == 7 || lidState == 0;
}

bool AirPods::IsCaseCharging() const
{
    return battery.caseCharging;
}

bool AirPods::IsLeftInEar() const
{
    // If it's charging, the "ear" will be set in one of the multiple devices, idk why..
    // so we need to filter it.
    //
    // The in-ear bits are also not reliable when both pods are in the case: they
    // can read as "in ear" even with the case shut, which would break automatic
    // ear detection (media is never paused/resumed because the state never
    // appears to change).
    //
    if (bothInCase || IsLeftCharging()) {
        return false;
    }
    // 'curr' normally belongs to the broadcasting pod and 'anot' to the other one;
    // that assignment is reversed when the broadcasting pod sits in the case (the
    // "flipped" bit). Gate both sides on the same test so they never disagree.
    //
    return IsCurrUsedForLeft() ? currInEar : anotInEar;
}

bool AirPods::IsRightInEar() const
{
    if (bothInCase || IsRightCharging()) {
        return false;
    }
    return IsCurrUsedForLeft() ? anotInEar : currInEar;
}

// 'curr' holds the values of the "current" pod and 'anot' of the "another" pod.
// Normally the "current" pod is the one broadcasting, but when that pod is sitting
// in the case (status bit 6, "flipped") the roles are reversed and 'curr' describes
// the other (worn) pod. Left/right therefore must follow `broadcastFrom XOR flipped`,
// otherwise the two pods swap values on screen every time the pod in the case takes
// a turn at broadcasting (observed on AirPods 4 as a right pod flickering between
// 30% and 40%). With the flip not set this reduces to the original mapping and is
// unchanged for worn pods.
//
bool AirPods::IsCurrUsedForLeft() const
{
    return IsLeftBroadcasted() != IsFlipped();
}

bool AirPods::IsFlipped() const
{
    return unk7 != 0;
}

AirPods AirPods::Desensitize() const
{
    auto result = *this;

    // This field may be some kind of hash or encrypted payload.
    // So it may contain personal information about the user.
    //
    std::memset(result.unk12, 0, sizeof(result.unk12));

    return result;
}
} // namespace Core::AppleCP
