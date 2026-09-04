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

#include "Bluetooth_linux.h"

#include <atomic>
#include <mutex>

#include <QMap>
#include <QTimer>
#include <QString>
#include <QVariantMap>
#include <QStringList>
#include <QRegularExpression>

#include <QDBusReply>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusConnection>

#include "../Logger.h"
#include "../Error.h"

using InterfaceMap = QMap<QString, QVariantMap>;
using ManagedObjectMap = QMap<QDBusObjectPath, InterfaceMap>;

Q_DECLARE_METATYPE(InterfaceMap)
Q_DECLARE_METATYPE(ManagedObjectMap)

namespace Core::Bluetooth {

using namespace std::placeholders;

namespace {

constexpr auto kBluezService = "org.bluez";
constexpr auto kBluezDeviceInterface = "org.bluez.Device1";
constexpr auto kBluezAdapterInterface = "org.bluez.Adapter1";
constexpr auto kDBusPropertiesInterface = "org.freedesktop.DBus.Properties";

void EnsureMetaTypesRegistered()
{
    static const bool onceFlag = [] {
        qDBusRegisterMetaType<InterfaceMap>();
        qDBusRegisterMetaType<ManagedObjectMap>();
        return true;
    }();
    Q_UNUSED(onceFlag);
}

uint64_t MacAddressToUint64(const QString &address)
{
    // "AA:BB:CC:DD:EE:FF" -> 0xAABBCCDDEEFF
    //
    QString hex = address;
    hex.remove(':');

    bool ok = false;
    const uint64_t result = hex.toULongLong(&ok, 16);
    if (!ok) {
        LOG(Warn, "MacAddressToUint64: Malformed address string.");
        return 0;
    }
    return result;
}
} // namespace

//////////////////////////////////////////////////
// Details::DBusDeviceInfo
//

namespace Details {

class DBusDeviceInfo final : public QObject
{
    Q_OBJECT

public:
    DBusDeviceInfo(const QString &path, const QVariantMap &properties) : _path{path}
    {
        UpdateProperties(properties);

        const bool connected = QDBusConnection::systemBus().connect(
            kBluezService, _path, kDBusPropertiesInterface, "PropertiesChanged", this,
            SLOT(OnPropertiesChanged(QString, QVariantMap, QStringList)));

        if (!connected) {
            LOG(Warn, "DBusDeviceInfo: Subscribe to PropertiesChanged failed. Path: '{}'",
                _path.toStdString());
        }
    }

    uint64_t GetAddress() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _address;
    }

    std::string GetName() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _name;
    }

    uint16_t GetVendorId() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _vendorId;
    }

    uint16_t GetProductId() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _productId;
    }

    DeviceState GetConnectionState() const
    {
        std::lock_guard<std::mutex> lock{_mutex};
        return _connected ? DeviceState::Connected : DeviceState::Disconnected;
    }

Q_SIGNALS:
    void ConnectionStateChanged(Core::Bluetooth::DeviceState state);
    void NameChanged(const std::string &name);

private Q_SLOTS:
    void OnPropertiesChanged(
        const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
    {
        Q_UNUSED(invalidated);

        if (interface != kBluezDeviceInterface) {
            return;
        }

        std::optional<DeviceState> newState;
        std::optional<std::string> newName;
        {
            std::lock_guard<std::mutex> lock{_mutex};

            if (changed.contains("Connected")) {
                const bool connected = changed.value("Connected").toBool();
                if (connected != _connected) {
                    _connected = connected;
                    newState = connected ? DeviceState::Connected : DeviceState::Disconnected;
                }
            }

            if (changed.contains("Name")) {
                auto name = changed.value("Name").toString().toStdString();
                if (name != _name) {
                    _name = name;
                    newName = std::move(name);
                }
            }
        }

        if (newState.has_value()) {
            LOG(Info, "Bluetooth device connection state changed. Connected: {}",
                newState.value() == DeviceState::Connected);
            Q_EMIT ConnectionStateChanged(newState.value());
        }
        if (newName.has_value()) {
            Q_EMIT NameChanged(newName.value());
        }
    }

private:
    void UpdateProperties(const QVariantMap &properties)
    {
        std::lock_guard<std::mutex> lock{_mutex};

        _address = MacAddressToUint64(properties.value("Address").toString());

        auto name = properties.value("Name").toString();
        if (name.isEmpty()) {
            name = properties.value("Alias").toString();
        }
        _name = name.toStdString();

        _connected = properties.value("Connected").toBool();

        // Modalias example: "bluetooth:v004Cp200Fd0100"
        //
        const auto modalias = properties.value("Modalias").toString();
        if (!modalias.isEmpty()) {
            static const QRegularExpression kModaliasRegex{
                "v([0-9A-Fa-f]{4})p([0-9A-Fa-f]{4})"};

            const auto match = kModaliasRegex.match(modalias);
            if (match.hasMatch()) {
                _vendorId = static_cast<uint16_t>(match.captured(1).toUInt(nullptr, 16));
                _productId = static_cast<uint16_t>(match.captured(2).toUInt(nullptr, 16));
            }
            else {
                LOG(Warn, "DBusDeviceInfo: Unrecognized modalias format: '{}'",
                    modalias.toStdString());
            }
        }
    }

    mutable std::mutex _mutex;
    QString _path;
    uint64_t _address{0};
    std::string _name;
    uint16_t _vendorId{0}, _productId{0};
    bool _connected{false};
};
} // namespace Details

//////////////////////////////////////////////////
// Device
//

Device::Device(std::shared_ptr<Details::DBusDeviceInfo> info) : _info{std::move(info)}
{
    Subscribe();
}

Device::Device(const Device &rhs) : _info{rhs._info}
{
    Subscribe();
}

Device::Device(Device &&rhs) noexcept
{
    rhs.Unsubscribe();
    _info = std::move(rhs._info);
    Subscribe();
}

Device::~Device()
{
    Unsubscribe();
}

Device &Device::operator=(const Device &rhs)
{
    if (this != &rhs) {
        Unsubscribe();
        _info = rhs._info;
        Subscribe();
    }
    return *this;
}

Device &Device::operator=(Device &&rhs) noexcept
{
    if (this != &rhs) {
        Unsubscribe();
        rhs.Unsubscribe();
        _info = std::move(rhs._info);
        Subscribe();
    }
    return *this;
}

void Device::Subscribe()
{
    if (!_info) {
        return;
    }

    _connections.push_back(QObject::connect(
        _info.get(), &Details::DBusDeviceInfo::ConnectionStateChanged,
        [this](DeviceState state) { CbConnectionStatusChanged().Invoke(state); }));

    _connections.push_back(QObject::connect(
        _info.get(), &Details::DBusDeviceInfo::NameChanged,
        [this](const std::string &name) { CbNameChanged().Invoke(name); }));
}

void Device::Unsubscribe()
{
    for (const auto &connection : _connections) {
        QObject::disconnect(connection);
    }
    _connections.clear();
}

uint64_t Device::GetAddress() const
{
    return _info ? _info->GetAddress() : 0;
}

std::string Device::GetName() const
{
    return _info ? _info->GetName() : std::string{};
}

uint16_t Device::GetVendorId() const
{
    return _info ? _info->GetVendorId() : 0;
}

uint16_t Device::GetProductId() const
{
    return _info ? _info->GetProductId() : 0;
}

DeviceState Device::GetConnectionState() const
{
    return _info ? _info->GetConnectionState() : DeviceState::Disconnected;
}

//////////////////////////////////////////////////
// DeviceManager
//

namespace DeviceManager {
namespace {

std::vector<Device> EnumerateDevices(std::optional<DeviceState> optState)
{
    EnsureMetaTypesRegistered();

    std::vector<Device> result;

    auto bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        LOG(Warn, "EnumerateDevices: System D-Bus is not connected.");
        return result;
    }

    auto call = QDBusMessage::createMethodCall(
        kBluezService, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

    QDBusReply<ManagedObjectMap> reply = bus.call(call);
    if (!reply.isValid()) {
        LOG(Warn, "EnumerateDevices: GetManagedObjects failed. Error: '{}'",
            reply.error().message().toStdString());
        return result;
    }

    const ManagedObjectMap objects = reply.value();
    for (auto iter = objects.cbegin(); iter != objects.cend(); ++iter) {
        const InterfaceMap &interfaces = iter.value();

        const auto deviceIter = interfaces.constFind(kBluezDeviceInterface);
        if (deviceIter == interfaces.cend()) {
            continue;
        }

        const QVariantMap &properties = deviceIter.value();

        const bool paired = properties.value("Paired").toBool();
        const bool connected = properties.value("Connected").toBool();

        if (optState.has_value()) {
            const auto state = optState.value();
            const bool matched = (state == DeviceState::Paired && paired) ||
                                 (state == DeviceState::Connected && connected) ||
                                 (state == DeviceState::Disconnected && paired && !connected);
            if (!matched) {
                continue;
            }
        }

        result.emplace_back(
            std::make_shared<Details::DBusDeviceInfo>(iter.key().path(), properties));
    }

    return result;
}
} // namespace

std::vector<Device> GetDevicesByState(DeviceState state)
{
    return EnumerateDevices(state);
}

std::optional<Device> FindDevice(uint64_t address)
{
    auto devices = EnumerateDevices(DeviceState::Paired);
    for (auto &device : devices) {
        if (device.GetAddress() == address) {
            return std::move(device);
        }
    }
    return std::nullopt;
}
} // namespace DeviceManager

//////////////////////////////////////////////////
// AdvertisementWatcher
//

class AdvertisementWatcher::Impl final : public QObject
{
    Q_OBJECT

public:
    explicit Impl(AdvertisementWatcher &owner) : _owner{owner}
    {
        EnsureMetaTypesRegistered();

        auto bus = QDBusConnection::systemBus();

        bus.connect(
            kBluezService, {}, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", this,
            SLOT(OnInterfacesAdded(QDBusObjectPath, InterfaceMap)));

        bus.connect(
            kBluezService, {}, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", this,
            SLOT(OnInterfacesRemoved(QDBusObjectPath, QStringList)));

        bus.connect(
            kBluezService, {}, kDBusPropertiesInterface, "PropertiesChanged", this,
            SLOT(OnPropertiesChanged(QString, QVariantMap, QStringList, QDBusMessage)));

        _retryTimer.setSingleShot(true);
        connect(&_retryTimer, &QTimer::timeout, this, &Impl::TryStart);
    }

    bool Start()
    {
        _stopRequested = false;
        TryStart();
        return true;
    }

    bool Stop()
    {
        _stopRequested = true;
        _retryTimer.stop();

        if (_discovering && !_adapterPath.isEmpty()) {
            auto call = QDBusMessage::createMethodCall(
                kBluezService, _adapterPath, kBluezAdapterInterface, "StopDiscovery");

            const QDBusReply<void> reply = QDBusConnection::systemBus().call(call);
            if (!reply.isValid()) {
                LOG(Warn, "Bluetooth AdvWatcher: StopDiscovery failed. Error: '{}'",
                    reply.error().message().toStdString());
            }
        }

        _discovering = false;
        return true;
    }

private Q_SLOTS:
    void TryStart()
    {
        if (_stopRequested || _discovering) {
            return;
        }

        auto bus = QDBusConnection::systemBus();
        if (!bus.isConnected()) {
            ReportStoppedAndRetry("System D-Bus is not connected");
            return;
        }

        if (!FindAdapterAndSeedCache()) {
            ReportStoppedAndRetry("No Bluetooth adapter available");
            return;
        }

        // Ask BlueZ for a LE scan and disable duplicate advertisement filtering, so that
        // we keep receiving `ManufacturerData` updates while scanning
        //
        QVariantMap filter;
        filter["Transport"] = QString{"le"};
        filter["DuplicateData"] = true;

        {
            auto call = QDBusMessage::createMethodCall(
                kBluezService, _adapterPath, kBluezAdapterInterface, "SetDiscoveryFilter");
            call << filter;

            const QDBusReply<void> reply = bus.call(call);
            if (!reply.isValid()) {
                LOG(Warn, "Bluetooth AdvWatcher: SetDiscoveryFilter failed. Error: '{}'",
                    reply.error().message().toStdString());
            }
        }

        {
            auto call = QDBusMessage::createMethodCall(
                kBluezService, _adapterPath, kBluezAdapterInterface, "StartDiscovery");

            const QDBusReply<void> reply = bus.call(call);
            if (!reply.isValid() && reply.error().name() != "org.bluez.Error.InProgress") {
                ReportStoppedAndRetry(reply.error().message().toStdString());
                return;
            }
        }

        _discovering = true;
        LOG(Info, "Bluetooth AdvWatcher: Discovery started. Adapter: '{}'",
            _adapterPath.toStdString());
        _owner.CbStateChanged().Invoke(State::Started, std::nullopt);
    }

    void OnInterfacesAdded(const QDBusObjectPath &objectPath, InterfaceMap interfaces)
    {
        if (interfaces.contains(kBluezAdapterInterface) && !_stopRequested && !_discovering) {
            LOG(Info, "Bluetooth AdvWatcher: Adapter appeared. Path: '{}'",
                objectPath.path().toStdString());
            TryStart();
        }

        const auto deviceIter = interfaces.constFind(kBluezDeviceInterface);
        if (deviceIter == interfaces.cend()) {
            return;
        }

        const QVariantMap &properties = deviceIter.value();

        auto &cache = _devices[objectPath.path()];
        cache.address = MacAddressToUint64(properties.value("Address").toString());
        if (properties.contains("RSSI")) {
            cache.rssi = static_cast<int16_t>(properties.value("RSSI").toInt());
        }
        if (properties.contains("ManufacturerData")) {
            cache.manufacturerData = ParseManufacturerData(properties.value("ManufacturerData"));
            EmitReceived(cache);
        }
    }

    void OnInterfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces)
    {
        if (interfaces.contains(kBluezDeviceInterface)) {
            _devices.remove(objectPath.path());
        }

        if (interfaces.contains(kBluezAdapterInterface) && objectPath.path() == _adapterPath) {
            _adapterPath.clear();
            OnDiscoveryLost("Bluetooth adapter removed");
        }
    }

    void OnPropertiesChanged(
        const QString &interface, const QVariantMap &changed, const QStringList &invalidated,
        const QDBusMessage &message)
    {
        Q_UNUSED(invalidated);

        if (interface == kBluezAdapterInterface) {
            if (message.path() != _adapterPath) {
                return;
            }

            if (changed.contains("Discovering") && !changed.value("Discovering").toBool()) {
                if (!_stopRequested && _discovering) {
                    OnDiscoveryLost("Discovery unexpectedly stopped");
                }
            }

            if (changed.contains("Powered") && changed.value("Powered").toBool()) {
                if (!_stopRequested && !_discovering) {
                    TryStart();
                }
            }
            return;
        }

        if (interface != kBluezDeviceInterface) {
            return;
        }

        auto deviceIter = _devices.find(message.path());
        if (deviceIter == _devices.end()) {
            DeviceCache cache;
            cache.address = FetchDeviceAddress(message.path());
            deviceIter = _devices.insert(message.path(), std::move(cache));
        }

        bool updated = false;

        if (changed.contains("RSSI")) {
            deviceIter->rssi = static_cast<int16_t>(changed.value("RSSI").toInt());
            updated = true;
        }
        if (changed.contains("ManufacturerData")) {
            deviceIter->manufacturerData = ParseManufacturerData(changed.value("ManufacturerData"));
            updated = true;
        }

        if (updated) {
            EmitReceived(deviceIter.value());
        }
    }

private:
    struct DeviceCache {
        uint64_t address{0};
        int16_t rssi{0};
        QMap<quint16, QByteArray> manufacturerData;
    };

    bool FindAdapterAndSeedCache()
    {
        auto call = QDBusMessage::createMethodCall(
            kBluezService, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");

        const QDBusReply<ManagedObjectMap> reply = QDBusConnection::systemBus().call(call);
        if (!reply.isValid()) {
            LOG(Warn, "Bluetooth AdvWatcher: GetManagedObjects failed. Error: '{}'",
                reply.error().message().toStdString());
            return false;
        }

        _adapterPath.clear();

        const ManagedObjectMap objects = reply.value();
        for (auto iter = objects.cbegin(); iter != objects.cend(); ++iter) {
            const InterfaceMap &interfaces = iter.value();

            if (_adapterPath.isEmpty() && interfaces.contains(kBluezAdapterInterface)) {
                _adapterPath = iter.key().path();
            }

            const auto deviceIter = interfaces.constFind(kBluezDeviceInterface);
            if (deviceIter != interfaces.cend()) {
                const QVariantMap &properties = deviceIter.value();

                auto &cache = _devices[iter.key().path()];
                cache.address = MacAddressToUint64(properties.value("Address").toString());
                if (properties.contains("RSSI")) {
                    cache.rssi = static_cast<int16_t>(properties.value("RSSI").toInt());
                }
                if (properties.contains("ManufacturerData")) {
                    cache.manufacturerData =
                        ParseManufacturerData(properties.value("ManufacturerData"));
                }
            }
        }

        return !_adapterPath.isEmpty();
    }

    uint64_t FetchDeviceAddress(const QString &devicePath) const
    {
        auto call = QDBusMessage::createMethodCall(
            kBluezService, devicePath, kDBusPropertiesInterface, "Get");
        call << QString{kBluezDeviceInterface} << QString{"Address"};

        const QDBusReply<QVariant> reply = QDBusConnection::systemBus().call(call);
        if (!reply.isValid()) {
            LOG(Warn, "Bluetooth AdvWatcher: Get device address failed. Error: '{}'",
                reply.error().message().toStdString());
            return 0;
        }

        return MacAddressToUint64(reply.value().toString());
    }

    static QMap<quint16, QByteArray> ParseManufacturerData(const QVariant &variant)
    {
        QMap<quint16, QByteArray> result;

        const auto argument = variant.value<QDBusArgument>();
        if (argument.currentType() != QDBusArgument::MapType) {
            return result;
        }

        argument.beginMap();
        while (!argument.atEnd()) {
            quint16 companyId{0};
            QVariant data;

            argument.beginMapEntry();
            argument >> companyId >> data;
            argument.endMapEntry();

            result.insert(companyId, data.toByteArray());
        }
        argument.endMap();

        return result;
    }

    void EmitReceived(const DeviceCache &cache)
    {
        if (cache.manufacturerData.isEmpty()) {
            return;
        }

        AdvertisementWatcher::ReceivedData data;

        data.rssi = cache.rssi;
        data.timestamp = std::chrono::steady_clock::now();
        data.address = cache.address;

        for (auto iter = cache.manufacturerData.cbegin(); iter != cache.manufacturerData.cend();
             ++iter)
        {
            const QByteArray &bytes = iter.value();
            data.manufacturerDataMap.try_emplace(
                iter.key(), std::vector<uint8_t>(bytes.cbegin(), bytes.cend()));
        }

        _owner.CbReceived().Invoke(data);
    }

    void OnDiscoveryLost(const std::string &reason)
    {
        _discovering = false;
        LOG(Warn, "Bluetooth AdvWatcher: {}", reason);
        _owner.CbStateChanged().Invoke(State::Stopped, reason);
        ScheduleRetry();
    }

    void ReportStoppedAndRetry(const std::string &reason)
    {
        LOG(Warn, "Bluetooth AdvWatcher: Start failed. Reason: '{}'", reason);
        _owner.CbStateChanged().Invoke(State::Stopped, reason);
        ScheduleRetry();
    }

    void ScheduleRetry()
    {
        if (!_stopRequested) {
            _retryTimer.start(std::chrono::milliseconds{kRetryInterval});
        }
    }

    static constexpr inline auto kRetryInterval = 3s;

    AdvertisementWatcher &_owner;
    QTimer _retryTimer;
    QString _adapterPath;
    QMap<QString, DeviceCache> _devices;
    bool _discovering{false};
    std::atomic<bool> _stopRequested{false};
};

AdvertisementWatcher::AdvertisementWatcher() = default;

AdvertisementWatcher::~AdvertisementWatcher() = default;

AdvertisementWatcher::Impl *AdvertisementWatcher::GetImpl()
{
    // Lazy initialization, to make sure the QApplication instance is created before
    // any QObject-based stuff here
    //
    if (!_impl) {
        _impl = std::make_unique<Impl>(*this);
    }
    return _impl.get();
}

bool AdvertisementWatcher::Start()
{
    return GetImpl()->Start();
}

bool AdvertisementWatcher::Stop()
{
    return GetImpl()->Stop();
}
} // namespace Core::Bluetooth

#include "Bluetooth_linux.moc"

