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

#include "GlobalMedia_linux.h"

#include <QString>
#include <QStringList>
#include <QDBusReply>
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include "../Utils.h"
#include "../Logger.h"
#include "../Error.h"

namespace Core::GlobalMedia {

using namespace std::chrono_literals;

namespace Details {
namespace {

constexpr auto kMprisServicePrefix = "org.mpris.MediaPlayer2.";
constexpr auto kMprisObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";

class MprisPlayer final : public MediaProgramAbstract
{
public:
    explicit MprisPlayer(QString serviceName) : _serviceName{std::move(serviceName)} {}

    bool IsAvailable() override
    {
        const auto iface = QDBusConnection::sessionBus().interface();
        return iface != nullptr && iface->isServiceRegistered(_serviceName);
    }

    bool IsPlaying() const override
    {
        return GetPlaybackStatus() == "Playing";
    }

    bool Play() override
    {
        return Call("Play");
    }

    bool Pause() override
    {
        return Call("Pause");
    }

    std::wstring GetProgramName() const override
    {
        return _serviceName.toStdWString();
    }

    Priority GetPriority() const override
    {
        return Priority::MusicPlayer;
    }

private:
    QString GetPlaybackStatus() const
    {
        QDBusInterface iface{
            _serviceName, kMprisObjectPath, kMprisPlayerInterface,
            QDBusConnection::sessionBus()};

        if (!iface.isValid()) {
            return {};
        }
        return iface.property("PlaybackStatus").toString();
    }

    bool Call(const char *method)
    {
        QDBusInterface iface{
            _serviceName, kMprisObjectPath, kMprisPlayerInterface,
            QDBusConnection::sessionBus()};

        if (!iface.isValid()) {
            LOG(Warn, "MprisPlayer: Interface is invalid. Service: '{}'",
                _serviceName.toStdString());
            return false;
        }

        const QDBusReply<void> reply = iface.call(method);
        if (!reply.isValid()) {
            LOG(Warn, "MprisPlayer: Call '{}' failed. Service: '{}', Error: '{}'", method,
                _serviceName.toStdString(), reply.error().message().toStdString());
            return false;
        }

        LOG(Info, "MprisPlayer: Call '{}' succeeded. Service: '{}'", method,
            _serviceName.toStdString());
        return true;
    }

    QString _serviceName;
};

std::vector<std::unique_ptr<MediaProgramAbstract>> EnumerateMprisPlayers()
{
    std::vector<std::unique_ptr<MediaProgramAbstract>> result;

    const auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || bus.interface() == nullptr) {
        LOG(Warn, "EnumerateMprisPlayers: Session D-Bus is not connected.");
        return result;
    }

    const QDBusReply<QStringList> reply = bus.interface()->registeredServiceNames();
    if (!reply.isValid()) {
        LOG(Warn, "EnumerateMprisPlayers: registeredServiceNames failed. Error: '{}'",
            reply.error().message().toStdString());
        return result;
    }

    const QStringList serviceNames = reply.value();
    for (const auto &serviceName : serviceNames) {
        if (serviceName.startsWith(kMprisServicePrefix)) {
            result.emplace_back(std::make_unique<MprisPlayer>(serviceName));
        }
    }

    return result;
}
} // namespace
} // namespace Details

void Controller::Play()
{
    std::lock_guard<std::mutex> lock{_mutex};

    LOG(Info, "GlobalMedia Play. Programs to resume: {}", _pausedPrograms.size());

    for (auto &program : _pausedPrograms) {
        if (!program->IsAvailable()) {
            LOG(Warn, "GlobalMedia Play: The program is no longer available.");
            continue;
        }

        if (!program->Play()) {
            LOG(Warn, "GlobalMedia Play: Resume failed.");
        }
    }

    _pausedPrograms.clear();
}

void Controller::Pause()
{
    std::lock_guard<std::mutex> lock{_mutex};

    auto players = Details::EnumerateMprisPlayers();

    LOG(Info, "GlobalMedia Pause. MPRIS players count: {}", players.size());

    for (auto &player : players) {
        if (!player->IsPlaying()) {
            continue;
        }

        if (player->Pause()) {
            _pausedPrograms.emplace_back(std::move(player));
        }
        else {
            LOG(Warn, "GlobalMedia Pause: Pause failed.");
        }
    }
}
} // namespace Core::GlobalMedia
