#include "protocol/transport.h"

#include "protocol/ble_transport.h"
#include "protocol/tcp_transport.h"

namespace proto {

bool ConnectTarget::isValid() const {
    if (kind == Kind::Ble) return !bleId.isEmpty() || !bleName.isEmpty();
    return !host.isEmpty() && port != 0;
}

QString ConnectTarget::label() const {
    if (kind == Kind::Ble) return bleName.isEmpty() ? bleId : bleName;
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

Transport* createTransport(const ConnectTarget& target, QObject* parent) {
    if (target.kind == ConnectTarget::Kind::Ble)
        return new BleTransport(target.bleId, target.bleName, parent);
    return new TcpTransport(target.host, target.port, parent);
}

}  // namespace proto
