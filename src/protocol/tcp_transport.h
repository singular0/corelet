#pragma once

#include <QString>

#include "protocol/stream_transport.h"

class QTcpSocket;

namespace proto {

// TCP to a MeshCore companion daemon. The stream and its framing are
// StreamTransport's; all that is left here is the socket.
class TcpTransport : public StreamTransport {
    Q_OBJECT

public:
    TcpTransport(QString host, quint16 port, QObject* parent = nullptr);

    QString description() const override;

private:
    void connectDevice() override;
    void abortDevice() override;
    bool isConnected() const override;

    QTcpSocket* socket_ = nullptr;
    QString host_;
    quint16 port_ = 0;
};

}  // namespace proto
