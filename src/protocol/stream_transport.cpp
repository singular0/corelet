#include "protocol/stream_transport.h"

#include <QIODevice>

namespace proto {

void StreamTransport::useDevice(QIODevice* device) {
    device_ = device;
    connect(device_, &QIODevice::readyRead, this, [this] {
        reader_.feed(device_->readAll());
        while (auto frame = reader_.next()) Q_EMIT frameReceived(*frame);
    });
}

void StreamTransport::open() {
    if (live_) return;
    live_ = true;
    abortDevice();
    reader_.reset();
    connectDevice();
}

void StreamTransport::close() {
    live_ = false;
    abortDevice();
    reader_.reset();
}

bool StreamTransport::isOpen() const { return isConnected(); }

void StreamTransport::send(const QByteArray& payload) { device_->write(frameCommand(payload)); }

void StreamTransport::fail(const QString& reason) {
    if (!live_) return;
    live_ = false;
    Q_EMIT closed(reason);
}

}  // namespace proto
