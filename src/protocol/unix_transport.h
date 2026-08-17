#pragma once

#include <QString>

#include "protocol/stream_transport.h"

class QLocalSocket;

namespace proto {

// A Unix domain socket to a MeshCore companion daemon, which is the endpoint
// the daemon offers by default: the same length-prefixed stream as TCP, but
// access is decided by the socket's owner, group and mode rather than by who
// can reach a port — the only access control there is, since the protocol
// itself authenticates nobody.
class UnixTransport : public StreamTransport {
    Q_OBJECT

public:
    explicit UnixTransport(QString path, QObject* parent = nullptr);

    QString description() const override;

private:
    void connectDevice() override;
    void abortDevice() override;
    bool isConnected() const override;

    QLocalSocket* socket_ = nullptr;
    QString path_;
};

}  // namespace proto
