#include "protocol/unix_transport.h"

#include <QLocalSocket>

namespace proto {

namespace {

// The status line is one elided row on a 480-row panel, and Qt's own strings
// are neither short nor plain ("QLocalSocket::connectToServer: Access denied").
// These two failures are the ones a user actually meets — a daemon that is not
// running, and a socket whose group they are not in — so they get wording that
// says which.
QString describeError(const QLocalSocket& socket) {
    switch (socket.error()) {
        case QLocalSocket::ServerNotFoundError: return QStringLiteral("no socket at that path");
        case QLocalSocket::SocketAccessError: return QStringLiteral("permission denied");
        case QLocalSocket::ConnectionRefusedError: return QStringLiteral("connection refused");
        default: return socket.errorString();
    }
}

}  // namespace

UnixTransport::UnixTransport(QString path, QObject* parent)
    : StreamTransport(parent), path_(std::move(path)) {
    socket_ = new QLocalSocket(this);
    useDevice(socket_);

    connect(socket_, &QLocalSocket::connected, this, [this] { Q_EMIT opened(); });
    connect(socket_, &QLocalSocket::disconnected, this,
            [this] { fail(QStringLiteral("connection closed")); });
    connect(socket_, &QLocalSocket::errorOccurred, this, [this] {
        // A socket that cannot be opened at all is an error that never becomes
        // a disconnect, so the failure has to be reported from here as well.
        // Unlike TCP this arrives from inside connectToServer(), which is safe:
        // CompanionClient answers closed() with a retry timer, not by reopening
        // the transport underneath itself.
        if (socket_->state() == QLocalSocket::UnconnectedState) fail(describeError(*socket_));
    });
}

QString UnixTransport::description() const { return path_; }

void UnixTransport::connectDevice() { socket_->connectToServer(path_); }

void UnixTransport::abortDevice() { socket_->abort(); }

bool UnixTransport::isConnected() const {
    return socket_->state() == QLocalSocket::ConnectedState;
}

}  // namespace proto
