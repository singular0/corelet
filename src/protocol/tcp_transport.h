#pragma once

#include <QByteArray>
#include <QString>

#include "protocol/frame_codec.h"
#include "protocol/transport.h"

class QTcpSocket;

namespace proto {

// TCP to a umeshcored.
//
// The stream says nothing about where a frame ends, so every frame carries a
// length prefix and the reader has to re-assemble them across reads.
class TcpTransport : public Transport {
    Q_OBJECT

public:
    TcpTransport(QString host, quint16 port, QObject* parent = nullptr);

    QString description() const override;
    void open() override;
    void close() override;
    bool isOpen() const override;
    void send(const QByteArray& payload) override;

private:
    void fail(const QString& reason);

    QTcpSocket* socket_ = nullptr;
    QString host_;
    quint16 port_ = 0;

    FrameReader reader_;
    // Guards the one-closed()-per-open() rule: Qt reports a refused connection
    // as an error with no disconnected() after it, and a dropped one as both.
    bool live_ = false;
};

}  // namespace proto
