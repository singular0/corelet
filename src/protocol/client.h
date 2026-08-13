#pragma once

#include <QObject>
#include <QQueue>
#include <QVector>
#include <functional>

#include "model/types.h"
#include "protocol/frame_codec.h"
#include "protocol/transport.h"

class QTimer;

namespace proto {

// Talks the MeshCore companion protocol over whatever link it is handed: a
// umeshcored on TCP, or a device on BLE.
//
// Two things shape this class. First, replies are untagged: nothing in a
// response says which command it answers, so exactly one command may be in
// flight and the rest wait in a queue. Second, the far end interleaves
// unsolicited pushes (codes >= 0x80) with those replies, so every inbound
// frame is routed by code before the queue is consulted.
class CompanionClient : public QObject {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Handshaking,  // socket up, APP_START sent, still enumerating
        Ready,
    };

    struct DeviceInfo {
        QString name;
        QByteArray pubkey;
        double freqMhz = 0;
        double bwKhz = 0;
        int sf = 0;
        int cr = 0;
        int txPowerDbm = 0;
    };

    explicit CompanionClient(QObject* parent = nullptr);
    ~CompanionClient() override;

    // Takes ownership of the transport, opens it, and keeps reopening it until
    // stop(). The far end may legitimately not be there yet — the daemon is a
    // systemd unit the user has to enable, a radio can be out of range — so a
    // failed connection is a normal state to sit in, not a fatal error.
    // Starting again replaces the previous transport, which is how the app
    // switches between a daemon and a device.
    void start(Transport* transport);
    void stop();

    State state() const { return state_; }
    const DeviceInfo& device() const { return device_; }
    const QVector<model::Channel>& channels() const { return channels_; }

    // `token` is handed straight back in sendResult(). Nothing in the protocol
    // identifies which send an answer belongs to, so the caller's own tag is
    // what lets it find the message it put on screen.
    void sendChannelMessage(int channelIndex, const QString& text, int token);

    // Writes a channel into a slot, creating it or replacing what was there.
    // The far end owns the keys, so this is the only way the app can add one.
    void setChannel(int channelIndex, const QString& name, const QByteArray& secret);

Q_SIGNALS:
    void stateChanged(State state, const QString& detail);
    void deviceInfoChanged(const DeviceInfo& info);
    void channelsChanged(const QVector<model::Channel>& channels);
    void messageReceived(const model::Message& msg);
    // A direct message the daemon handed us. v1 has no DM view, but SYNC pops
    // from the daemon's inbox, so these must be captured rather than dropped.
    void directMessageReceived(const model::Message& msg);
    // Exactly one of these arrives per sendChannelMessage(), including when the
    // link dies with the command still queued.
    void sendResult(int token, bool ok, const QString& error);
    // Answer to setChannel(). Emitted after channelsChanged() when it succeeds,
    // so a listener can open the slot it just created.
    void channelSaveResult(int channelIndex, bool ok, const QString& error);

private Q_SLOTS:
    void onOpened();
    void onClosed(const QString& reason);
    void onReplyTimeout();
    void reconnect();

private:
    // Returns true when the command is complete. Multi-frame replies (the
    // contact stream) return false until their terminating frame arrives.
    using ReplyHandler = std::function<bool(quint8 code, Reader& r)>;
    // Run instead of the reply handler when the link goes down with the command
    // still queued. The queue does not survive a reconnect, so without this a
    // send would simply vanish and the app would show it as still on its way.
    using AbortHandler = std::function<void()>;

    struct Pending {
        QByteArray payload;
        ReplyHandler handler;
        AbortHandler onAbort;
    };

    void enqueue(const QByteArray& payload, ReplyHandler handler = {},
                 AbortHandler onAbort = {});
    void pump();
    void handleFrame(const QByteArray& frame);
    void handlePush(quint8 code, Reader& r);

    void beginHandshake();
    void requestChannel(int index);
    // Re-reads one slot after writing it and reports the write's outcome.
    void readBackChannel(int index);
    void requestSync();

    void setState(State s, const QString& detail = {});
    void resetConnection();

    // Shared tail parsing for both V3 message shapes.
    model::Message parseMessageTail(Reader& r, qint8 snrQ4, int channelIndex);

    Transport* transport_ = nullptr;
    QTimer* replyTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;

    bool running_ = false;
    int backoffMs_ = 0;

    QQueue<Pending> queue_;
    bool inFlight_ = false;
    // Guards against stacking a sync per push: one drain loop is enough, and
    // the daemon pushes MSG_WAITING once per stored message.
    bool syncPending_ = false;

    State state_ = State::Disconnected;
    DeviceInfo device_;
    QVector<model::Channel> channels_;
    int channelsOutstanding_ = 0;
};

}  // namespace proto
