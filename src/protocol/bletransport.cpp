#include "protocol/bletransport.h"

#include <QBluetoothAddress>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>

namespace proto {

const QBluetoothUuid NusService {QStringLiteral("{6e400001-b5a3-f393-e0a9-e50e24dcca9e}")};
const QBluetoothUuid NusWriteChar {QStringLiteral("{6e400002-b5a3-f393-e0a9-e50e24dcca9e}")};
const QBluetoothUuid NusNotifyChar {QStringLiteral("{6e400003-b5a3-f393-e0a9-e50e24dcca9e}")};

namespace {

// Long enough for a node that is between advertisements, short enough that a
// device which is simply off reports as missing rather than hanging the UI.
constexpr int DiscoveryTimeoutMs = 12000;

QString serviceErrorText(QLowEnergyService::ServiceError error) {
    switch (error) {
        case QLowEnergyService::CharacteristicWriteError:
            return QStringLiteral("device rejected a command");
        case QLowEnergyService::DescriptorWriteError:
            return QStringLiteral("could not enable notifications");
        case QLowEnergyService::CharacteristicReadError:
        case QLowEnergyService::DescriptorReadError:
            return QStringLiteral("read failed");
        case QLowEnergyService::OperationError:
            return QStringLiteral("service not ready");
        default:
            return QStringLiteral("bluetooth error");
    }
}

}  // namespace

QString bleDeviceId(const QBluetoothDeviceInfo& info) {
    // CoreBluetooth never hands out a peripheral's hardware address, so on macOS
    // the address is null and its per-host UUID is the only handle there is.
    if (!info.address().isNull()) return info.address().toString();
    return info.deviceUuid().toString(QUuid::WithoutBraces);
}

bool looksLikeMeshCore(const QBluetoothDeviceInfo& info) {
    // Stock firmware advertises as "MeshCore-xxxx", but the name is the node's
    // and users rename them, so an advertised UART service counts too.
    if (info.name().startsWith(QStringLiteral("MeshCore"), Qt::CaseInsensitive)) return true;
    return info.serviceUuids().contains(NusService);
}

ConnectTarget bleTarget(const QString& nameOrId) {
    ConnectTarget target;
    target.kind = ConnectTarget::Kind::Ble;
    if (!QBluetoothAddress(nameOrId).isNull() || !QUuid::fromString(nameOrId).isNull())
        target.bleId = nameOrId;
    else
        target.bleName = nameOrId;
    return target;
}

BleTransport::BleTransport(QString id, QString name, QObject* parent)
    : Transport(parent), id_(std::move(id)), name_(std::move(name)) {}

BleTransport::~BleTransport() { teardown(); }

QString BleTransport::description() const { return name_.isEmpty() ? id_ : name_; }

bool BleTransport::isOpen() const { return ready_; }

void BleTransport::open() {
    if (live_) return;
    live_ = true;
    ready_ = false;

    if (device_.isValid())
        connectToDevice(device_);
    else
        startDiscovery();
}

void BleTransport::close() {
    live_ = false;
    teardown();
}

// ---------------------------------------------------------------------------
// Finding the device
// ---------------------------------------------------------------------------

void BleTransport::startDiscovery() {
    nameMatch_ = QBluetoothDeviceInfo();
    Q_EMIT progress(QStringLiteral("scanning for %1").arg(description()));

    agent_ = new QBluetoothDeviceDiscoveryAgent(this);
    agent_->setLowEnergyDiscoveryTimeout(DiscoveryTimeoutMs);

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
            [this](const QBluetoothDeviceInfo& info) {
                if (!(info.coreConfigurations() &
                      QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
                    return;

                if (!id_.isEmpty() && bleDeviceId(info) == id_) {
                    connectToDevice(info);
                    return;
                }
                if (!name_.isEmpty() && info.name() == name_) {
                    // With no handle to match on there is nothing better coming,
                    // so take it now instead of waiting out the scan.
                    if (id_.isEmpty())
                        connectToDevice(info);
                    else
                        nameMatch_ = info;
                }
            });

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::finished, this, [this] {
        if (nameMatch_.isValid())
            connectToDevice(nameMatch_);
        else
            fail(QStringLiteral("%1 not found").arg(description()));
    });

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
            [this](QBluetoothDeviceDiscoveryAgent::Error error) {
                fail(error == QBluetoothDeviceDiscoveryAgent::PoweredOffError
                         ? QStringLiteral("bluetooth is off")
                         : agent_->errorString());
            });

    agent_->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleTransport::stopDiscovery() {
    if (!agent_) return;
    // Disconnect first: stop() reports back through the same signals, and this
    // is routinely called from inside one of them.
    agent_->disconnect(this);
    agent_->stop();
    agent_->deleteLater();
    agent_ = nullptr;
}

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------

void BleTransport::connectToDevice(const QBluetoothDeviceInfo& info) {
    stopDiscovery();
    device_ = info;
    if (name_.isEmpty()) name_ = info.name();
    if (id_.isEmpty()) id_ = bleDeviceId(info);

    Q_EMIT progress(QStringLiteral("connecting to %1").arg(description()));

    controller_ = QLowEnergyController::createCentral(device_, this);
    connect(controller_, &QLowEnergyController::connected, this, &BleTransport::discoverServices);
    connect(controller_, &QLowEnergyController::discoveryFinished, this,
            &BleTransport::openService);
    connect(controller_, &QLowEnergyController::disconnected, this,
            [this] { fail(QStringLiteral("%1 disconnected").arg(description())); });
    connect(controller_, &QLowEnergyController::errorOccurred, this, [this] {
        const QString reason = controller_->errorString();
        // A stored handle is the usual suspect when a connect fails outright:
        // CoreBluetooth's peripheral UUIDs go stale across reboots and BlueZ
        // forgets devices it has not seen. Drop it so the retry rescans.
        device_ = QBluetoothDeviceInfo();
        fail(reason);
    });

    controller_->connectToDevice();
}

void BleTransport::discoverServices() {
    Q_EMIT progress(QStringLiteral("reading services"));
    controller_->discoverServices();
}

void BleTransport::openService() {
    if (!controller_->services().contains(NusService)) {
        fail(QStringLiteral("%1 is not a MeshCore device").arg(description()));
        return;
    }

    service_ = controller_->createServiceObject(NusService, this);
    if (!service_) {
        fail(QStringLiteral("could not open the UART service"));
        return;
    }

    connect(service_, &QLowEnergyService::stateChanged, this,
            [this](QLowEnergyService::ServiceState state) {
                if (state == QLowEnergyService::RemoteServiceDiscovered) subscribe();
            });
    connect(service_, &QLowEnergyService::characteristicChanged, this,
            [this](const QLowEnergyCharacteristic& characteristic, const QByteArray& value) {
                // One notification is one companion frame: the firmware writes
                // them whole, so there is nothing to de-frame.
                if (characteristic.uuid() == NusNotifyChar && !value.isEmpty())
                    Q_EMIT frameReceived(value);
            });
    connect(service_, &QLowEnergyService::descriptorWritten, this,
            [this](const QLowEnergyDescriptor&, const QByteArray& value) {
                if (ready_ || value.isEmpty() || value[0] == 0) return;
                ready_ = true;
                Q_EMIT opened();
            });
    connect(service_, &QLowEnergyService::errorOccurred, this,
            [this](QLowEnergyService::ServiceError error) {
                if (error != QLowEnergyService::NoError) fail(serviceErrorText(error));
            });

    service_->discoverDetails();
}

void BleTransport::subscribe() {
    writeChar_ = service_->characteristic(NusWriteChar);
    const QLowEnergyCharacteristic notifyChar = service_->characteristic(NusNotifyChar);
    if (!writeChar_.isValid() || !notifyChar.isValid()) {
        fail(QStringLiteral("UART characteristics missing"));
        return;
    }

    const QLowEnergyDescriptor cccd =
        notifyChar.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
    if (!cccd.isValid()) {
        fail(QStringLiteral("device cannot notify"));
        return;
    }

    Q_EMIT progress(QStringLiteral("subscribing"));
    // The link only counts as open once notifications are armed: pushes sent
    // before that are lost, and PUSH_MSG_WAITING is how messages arrive at all.
    // opened() is emitted from descriptorWritten().
    service_->writeDescriptor(cccd, QByteArray::fromHex("0100"));
}

void BleTransport::send(const QByteArray& payload) {
    if (!ready_ || !service_) return;

    // One write is one frame — the firmware does not reassemble — which holds
    // because a companion command is at most a couple of hundred bytes and every
    // MeshCore build negotiates an MTU well past that.
    const auto mode = (writeChar_.properties() & QLowEnergyCharacteristic::Write)
                          ? QLowEnergyService::WriteWithResponse
                          : QLowEnergyService::WriteWithoutResponse;
    service_->writeCharacteristic(writeChar_, payload, mode);
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void BleTransport::teardown() {
    ready_ = false;
    writeChar_ = QLowEnergyCharacteristic();
    stopDiscovery();

    // Everything here is deleted later and disconnected now: teardown runs from
    // inside these objects' own signal handlers.
    if (service_) {
        service_->disconnect(this);
        service_->deleteLater();
        service_ = nullptr;
    }
    if (controller_) {
        controller_->disconnect(this);
        controller_->disconnectFromDevice();
        controller_->deleteLater();
        controller_ = nullptr;
    }
}

void BleTransport::fail(const QString& reason) {
    if (!live_) return;
    live_ = false;
    teardown();
    Q_EMIT closed(reason);
}

}  // namespace proto
