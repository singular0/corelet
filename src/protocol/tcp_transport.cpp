#include "protocol/tcp_transport.h"

#include <QTcpSocket>

namespace proto {

TcpTransport::TcpTransport(QString host, quint16 port, QObject* parent)
    : StreamTransport(parent), host_(std::move(host)), port_(port) {
    socket_ = new QTcpSocket(this);
    useDevice(socket_);

    connect(socket_, &QTcpSocket::connected, this, [this] { Q_EMIT opened(); });
    connect(socket_, &QTcpSocket::disconnected, this,
            [this] { fail(QStringLiteral("connection closed")); });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this] {
        // A refused connection is an error that never becomes a disconnect, so
        // the failure has to be reported from here as well.
        if (socket_->state() == QAbstractSocket::UnconnectedState) fail(socket_->errorString());
    });
}

QString TcpTransport::description() const {
    return QStringLiteral("%1:%2").arg(host_).arg(port_);
}

void TcpTransport::connectDevice() { socket_->connectToHost(host_, port_); }

void TcpTransport::abortDevice() { socket_->abort(); }

bool TcpTransport::isConnected() const {
    return socket_->state() == QAbstractSocket::ConnectedState;
}

}  // namespace proto
