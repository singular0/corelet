#pragma once

#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QLowEnergyCharacteristic>
#include <QString>

#include "protocol/transport.h"

class QBluetoothDeviceDiscoveryAgent;
class QLowEnergyController;
class QLowEnergyService;

namespace proto {

// MeshCore exposes the companion protocol over the Nordic UART service: the
// firmware's "RX" characteristic is what we write to, its "TX" is what it
// notifies us on. Names are from the device's point of view, so they read
// backwards here.
extern const QBluetoothUuid NusService;
extern const QBluetoothUuid NusWriteChar;   // app -> device
extern const QBluetoothUuid NusNotifyChar;  // device -> app

// The adapter's handle for a device: a MAC address under BlueZ, an opaque UUID
// under CoreBluetooth, which never exposes the real address. Both are local to
// this machine, so this is an identifier to store and match on, not something
// to show a user or send anywhere.
QString bleDeviceId(const QBluetoothDeviceInfo& info);

// Whether a scan result is worth offering as a MeshCore node: either it says so
// in its name, or it advertises the UART service the protocol rides on.
bool looksLikeMeshCore(const QBluetoothDeviceInfo& info);

// A BLE target from one string, as `--ble` takes it: an address or UUID is a
// handle, anything else is the name the device advertises. Telling them apart
// here means a name match during discovery is taken at once rather than held
// back in case a better handle match turns up.
ConnectTarget bleTarget(const QString& nameOrId);

// BLE to a MeshCore device.
//
// Unlike TCP there is no framing to do: the firmware writes one companion frame
// per notification and expects one per write, so payloads pass through
// untouched. Nothing here knows what is inside them.
class BleTransport : public Transport {
    Q_OBJECT

public:
    // `id` is a bleDeviceId(); `name` is the advertised name, used as a fallback
    // when the adapter's handle has gone stale (CoreBluetooth's UUIDs do not
    // survive forever, and a BlueZ address can change if the node re-pairs).
    BleTransport(QString id, QString name, QObject* parent = nullptr);
    ~BleTransport() override;

    QString description() const override;
    void open() override;
    void close() override;
    bool isOpen() const override;
    void send(const QByteArray& payload) override;

private:
    // The device has to be resolved to a live QBluetoothDeviceInfo before it can
    // be connected to — an address alone is not enough on CoreBluetooth — so an
    // unresolved target is scanned for first.
    void startDiscovery();
    void stopDiscovery();
    void connectToDevice(const QBluetoothDeviceInfo& info);
    void discoverServices();
    void openService();
    void subscribe();

    void teardown();
    void fail(const QString& reason);

    QString id_;
    QString name_;
    // Kept across reconnects: re-scanning for a device we already found costs
    // seconds on every drop. Cleared when the link fails, since a stale handle
    // is exactly what a failed connect looks like.
    QBluetoothDeviceInfo device_;
    // A device whose name matches but whose handle does not. Held until the scan
    // ends rather than taken immediately, so an exact handle always wins.
    QBluetoothDeviceInfo nameMatch_;

    QBluetoothDeviceDiscoveryAgent* agent_ = nullptr;
    QLowEnergyController* controller_ = nullptr;
    QLowEnergyService* service_ = nullptr;
    QLowEnergyCharacteristic writeChar_;

    bool live_ = false;
    bool ready_ = false;
};

}  // namespace proto
