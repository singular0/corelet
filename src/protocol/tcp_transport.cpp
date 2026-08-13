#include "protocol/tcp_transport.h"

#include <QTcpSocket>

namespace proto {

TcpTransport::TcpTransport(QString host, quint16 port, QObject* parent)
    : Transport(parent), host_(std::move(host)), port_(port) {
    socket_ = new QTcpSocket(this);

    connect(socket_, &QTcpSocket::connected, this, [this] { Q_EMIT opened(); });
    connect(socket_, &QTcpSocket::disconnected, this,
            [this] { fail(QStringLiteral("connection closed")); });
    connect(socket_, &QTcpSocket::readyRead, this, [this] {
        reader_.feed(socket_->readAll());
        while (auto frame = reader_.next()) Q_EMIT frameReceived(*frame);
    });
    connect(socket_, &QTcpSocket::errorOccurred, this, [this] {
        // A refused connection is an error that never becomes a disconnect, so
        // the failure has to be reported from here as well.
        if (socket_->state() == QAbstractSocket::UnconnectedState) fail(socket_->errorString());
    });
}

QString TcpTransport::description() const {
    return QStringLiteral("%1:%2").arg(host_).arg(port_);
}

void TcpTransport::open() {
    if (live_) return;
    live_ = true;
    socket_->abort();
    reader_.reset();
    socket_->connectToHost(host_, port_);
}

void TcpTransport::close() {
    live_ = false;
    socket_->abort();
    reader_.reset();
}

bool TcpTransport::isOpen() const { return socket_->state() == QAbstractSocket::ConnectedState; }

void TcpTransport::send(const QByteArray& payload) { socket_->write(frameCommand(payload)); }

void TcpTransport::fail(const QString& reason) {
    if (!live_) return;
    live_ = false;
    Q_EMIT closed(reason);
}

}  // namespace proto
