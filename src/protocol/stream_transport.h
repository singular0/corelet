#pragma once

#include <QByteArray>

#include "protocol/frame_codec.h"
#include "protocol/transport.h"

class QIODevice;

namespace proto {

// Shared half of the two byte-stream links.
//
// A TCP connection and a Unix socket carry the identical length-prefixed
// stream: the bytes say nothing about where a frame ends, so frames are
// re-assembled across reads, and a link that goes away has to report itself
// exactly once however it went. Only the socket differs, and QTcpSocket and
// QLocalSocket share no base past QIODevice — so the framing, the send path and
// the one-closed()-per-open() guard live here, and a subclass supplies the
// three verbs its own socket class spells differently.
class StreamTransport : public Transport {
    Q_OBJECT

public:
    void open() override;
    void close() override;
    bool isOpen() const override;
    void send(const QByteArray& payload) override;

protected:
    using Transport::Transport;

    // Adopts the socket the subclass owns. Its connected/disconnected/error
    // signals stay with the subclass, which reports them as opened() and
    // fail(): only Qt's spelling of them differs, not their meaning.
    void useDevice(QIODevice* device);
    // Reports a link that has gone away, at most once per open().
    void fail(const QString& reason);

private:
    virtual void connectDevice() = 0;
    // Immediate reset, discarding anything queued: reconnecting must not
    // deliver bytes from the connection before it.
    virtual void abortDevice() = 0;
    virtual bool isConnected() const = 0;

    QIODevice* device_ = nullptr;
    FrameReader reader_;
    bool live_ = false;
};

}  // namespace proto
