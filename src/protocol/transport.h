#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

namespace proto {

// The link the companion protocol runs over.
//
// The protocol itself is identical on every link; what differs is where a frame
// ends. TCP is a byte stream, so frames carry the length prefix and have to be
// de-framed incrementally. BLE is already message-oriented: one GATT packet is
// exactly one frame, prefix and all resynchronisation logic unnecessary. Hiding
// that behind this interface is what keeps CompanionClient from growing a
// transport switch in every method.
class Transport : public QObject {
    Q_OBJECT

public:
    explicit Transport(QObject* parent = nullptr) : QObject(parent) {}

    // What the user should see in the status bar: "127.0.0.1:5000", or a device
    // name for BLE.
    virtual QString description() const = 0;

    // Asynchronous. Exactly one closed() follows each open(), whether the
    // attempt failed outright or an established link later went away; close()
    // is silent, because the caller already knows.
    virtual void open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // An unframed command payload. The transport adds whatever its link needs.
    virtual void send(const QByteArray& payload) = 0;

Q_SIGNALS:
    void opened();
    void closed(const QString& reason);
    void frameReceived(const QByteArray& payload);
    // Progress while opening. A BLE scan takes seconds, and a status bar that
    // says nothing for that long reads as a hang.
    void progress(const QString& detail);
};

// Where to connect, as chosen on the command line or in the connect dialog and
// remembered between runs.
struct ConnectTarget {
    enum class Kind { Tcp, Ble };

    Kind kind = Kind::Tcp;

    // The daemon binds loopback by default and the protocol has no
    // authentication, so a local default is the only safe one.
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 5000;

    // Whatever the local adapter uses to name the device: a MAC address under
    // BlueZ, an opaque per-host UUID under CoreBluetooth. Either way it is a
    // handle for this machine only, so the advertised name is kept alongside it
    // both to label the UI and to re-find the device if the handle goes stale.
    QString bleId;
    QString bleName;

    bool isValid() const;
    QString label() const;
};

// Builds the transport for a target. Unparented by default: CompanionClient
// adopts what it is given.
Transport* createTransport(const ConnectTarget& target, QObject* parent = nullptr);

}  // namespace proto
