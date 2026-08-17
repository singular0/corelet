#include "protocol/transport.h"

#include "protocol/ble_transport.h"
#include "protocol/tcp_transport.h"
#include "protocol/unix_transport.h"

namespace proto {

bool ConnectTarget::isValid() const {
    switch (kind) {
        case Kind::Ble: return !bleId.isEmpty() || !bleName.isEmpty();
        // Anything Qt would resolve against the temporary directory is rejected
        // here rather than left to fail as a missing socket.
        case Kind::Unix: return socketPath.startsWith(QLatin1Char('/'));
        case Kind::Tcp: break;
    }
    return !host.isEmpty() && port != 0;
}

QString ConnectTarget::label() const {
    switch (kind) {
        case Kind::Ble: return bleName.isEmpty() ? bleId : bleName;
        case Kind::Unix: return socketPath;
        case Kind::Tcp: break;
    }
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

Transport* createTransport(const ConnectTarget& target, QObject* parent) {
    switch (target.kind) {
        case ConnectTarget::Kind::Ble:
            return new BleTransport(target.bleId, target.bleName, parent);
        case ConnectTarget::Kind::Unix: return new UnixTransport(target.socketPath, parent);
        case ConnectTarget::Kind::Tcp: break;
    }
    return new TcpTransport(target.host, target.port, parent);
}

}  // namespace proto
