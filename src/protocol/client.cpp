#include "protocol/client.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>

namespace proto {

namespace {

// Both ends answer every command promptly — the daemon is a single poll() loop
// with no blocking work, and a device answers from its radio task. A missing
// reply therefore means the link is wedged, and since the queue is strictly
// serialized, waiting forever would freeze sending altogether. Fail the command
// and move on instead.
constexpr int ReplyTimeoutMs = 10000;

constexpr int ReconnectMinMs = 1000;
constexpr int ReconnectMaxMs = 15000;

// Timestamps arrive from other nodes' clocks, which on a mesh device may be
// unset. Anything before 2020 is not a real message time, so fall back to
// arrival time rather than showing 1970.
constexpr qint64 PlausibleEpoch = 1577836800;  // 2020-01-01Z

QDateTime messageTime(quint32 wire) {
    if (qint64(wire) < PlausibleEpoch) return QDateTime::currentDateTime();
    return QDateTime::fromSecsSinceEpoch(wire);
}

// Channel messages carry no sender key; the sender's name is prepended to the
// text as "Name: body" by the sending node. Split it back out for display,
// tolerating a body that legitimately contains a colon.
void splitSender(const QString& raw, QString& sender, QString& body) {
    const int sep = raw.indexOf(QStringLiteral(": "));
    // A "sender" that long is a colon in prose, not a node name.
    if (sep > 0 && sep <= 32) {
        sender = raw.left(sep);
        body = raw.mid(sep + 2);
    } else {
        body = raw;
    }
}

}  // namespace

CompanionClient::CompanionClient(QObject* parent) : QObject(parent) {
    replyTimer_ = new QTimer(this);
    replyTimer_->setSingleShot(true);
    replyTimer_->setInterval(ReplyTimeoutMs);
    connect(replyTimer_, &QTimer::timeout, this, &CompanionClient::onReplyTimeout);

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &CompanionClient::reconnect);
}

CompanionClient::~CompanionClient() = default;

void CompanionClient::start(Transport* transport) {
    if (!transport) return;

    stop();
    delete transport_;
    transport_ = transport;
    transport_->setParent(this);

    connect(transport_, &Transport::opened, this, &CompanionClient::onOpened);
    connect(transport_, &Transport::closed, this, &CompanionClient::onClosed);
    connect(transport_, &Transport::frameReceived, this, &CompanionClient::handleFrame);
    // Opening a link can take a while — a BLE scan especially — so whatever the
    // transport can say about its progress goes straight to the status bar.
    connect(transport_, &Transport::progress, this,
            [this](const QString& detail) { setState(State::Connecting, detail); });

    running_ = true;
    backoffMs_ = 0;
    reconnect();
}

void CompanionClient::stop() {
    running_ = false;
    reconnectTimer_->stop();
    if (transport_) transport_->close();
    resetConnection();
    setState(State::Disconnected);
}

void CompanionClient::reconnect() {
    if (!running_ || !transport_) return;
    setState(State::Connecting,
             QStringLiteral("connecting to %1").arg(transport_->description()));
    transport_->close();
    resetConnection();
    transport_->open();
}

void CompanionClient::resetConnection() {
    QQueue<Pending> dropped;
    dropped.swap(queue_);
    inFlight_ = false;
    syncPending_ = false;
    channelsOutstanding_ = 0;
    replyTimer_->stop();

    // Last, and against an already-clean queue: an abort handler reaches the UI,
    // which may well answer by starting another command.
    for (const Pending& p : dropped)
        if (p.onAbort) p.onAbort();
}

void CompanionClient::setState(State s, const QString& detail) {
    if (state_ == s && detail.isEmpty()) return;
    state_ = s;
    Q_EMIT stateChanged(s, detail);
}

// ---------------------------------------------------------------------------
// Link lifecycle
// ---------------------------------------------------------------------------

void CompanionClient::onOpened() {
    // Only a completed handshake proves the far end really speaks the companion
    // protocol, so the backoff is not reset here.
    setState(State::Handshaking);
    beginHandshake();
}

void CompanionClient::onClosed(const QString& reason) {
    resetConnection();
    if (!running_) {
        setState(State::Disconnected, reason);
        return;
    }

    backoffMs_ = backoffMs_ ? qMin(backoffMs_ * 2, ReconnectMaxMs) : ReconnectMinMs;
    const QString retry = QStringLiteral("retrying in %1 s").arg(backoffMs_ / 1000);
    setState(State::Disconnected,
             reason.isEmpty() ? retry : QStringLiteral("%1 · %2").arg(reason, retry));
    reconnectTimer_->start(backoffMs_);
}

// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------

void CompanionClient::enqueue(const QByteArray& payload, ReplyHandler handler,
                              AbortHandler onAbort) {
    queue_.enqueue(Pending {payload, std::move(handler), std::move(onAbort)});
    pump();
}

void CompanionClient::pump() {
    if (inFlight_ || queue_.isEmpty()) return;
    if (!transport_ || !transport_->isOpen()) return;

    inFlight_ = true;
    transport_->send(queue_.head().payload);
    replyTimer_->start();
}

void CompanionClient::onReplyTimeout() {
    if (!inFlight_) return;
    // A far end that stopped answering is not going to start again on this
    // link; drop it and let the reconnect path rebuild the session. close() is
    // silent by contract, so the teardown is driven from here.
    transport_->close();
    onClosed(QStringLiteral("stopped responding"));
}

void CompanionClient::handleFrame(const QByteArray& frame) {
    if (frame.isEmpty()) return;

    Reader r(frame);
    const quint8 code = r.u8();

    if (isPush(code)) {
        handlePush(code, r);
        return;
    }

    if (!inFlight_) return;  // stray reply with nothing outstanding

    // Copy the handler out before running it: it may enqueue the next command,
    // and that must not run against a half-updated queue.
    ReplyHandler handler = queue_.head().handler;
    bool done = true;
    if (handler) done = handler(code, r);

    // A handler may have torn the connection down — a bad handshake reply does
    // exactly that — which clears the queue this frame belonged to.
    if (!inFlight_ || queue_.isEmpty()) return;

    if (!done) {
        replyTimer_->start();  // more frames coming for this command
        return;
    }

    queue_.dequeue();
    inFlight_ = false;
    replyTimer_->stop();
    pump();
}

void CompanionClient::handlePush(quint8 code, Reader& r) {
    switch (code) {
        case PushMsgWaiting:
            requestSync();
            break;
        case PushAdvert:
        case PushNewAdvert:
        case PushPathUpdated:
        case PushSendConfirmed:
        case PushLogRxData:
        default:
            // v1 shows channels only. The rest of the push surface (contact
            // discovery, per-message ack confirmation, raw RX logging) is
            // deliberately ignored rather than half-handled.
            break;
    }
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

void CompanionClient::beginHandshake() {
    channels_.clear();
    channelsOutstanding_ = 0;

    // APP_START: version byte, six reserved, then the app name.
    Writer start(CmdAppStart);
    start.u8(1).padded({}, 6).tail(QByteArrayLiteral("umeshcore-app"));
    enqueue(start.bytes(), [this](quint8 code, Reader& r) {
        if (code != RespSelfInfo) {
            // Whatever is on the other end, it is not speaking this protocol.
            transport_->close();
            onClosed(QStringLiteral("unexpected handshake reply"));
            return true;
        }

        DeviceInfo info;
        r.skip(1);  // adv_type
        info.txPowerDbm = r.u8();
        r.skip(1);  // max tx power
        info.pubkey = r.take(32);
        r.skip(4 + 4);  // advertised lat/lon
        r.skip(4);      // multi-acks, location policy, telemetry mode, manual add
        info.freqMhz = r.u32() / 1000.0;
        info.bwKhz = r.u32() / 1000.0;
        info.sf = r.u8();
        info.cr = r.u8();
        info.name = QString::fromUtf8(r.rest());
        if (!r.ok()) return true;

        device_ = info;
        Q_EMIT deviceInfoChanged(device_);
        return true;
    });

    // The daemon cannot step the system clock, so it tracks an offset from
    // whatever the app tells it. Adverts are rejected as replays without a
    // sane wall clock, so this is not cosmetic.
    Writer time(CmdSetDeviceTime);
    time.u32(quint32(QDateTime::currentSecsSinceEpoch()));
    enqueue(time.bytes());

    // Slots are sparse and the store always reports all eight, so ask for every
    // one and keep those with a non-zero key.
    channelsOutstanding_ = MaxChannels;
    for (int i = 0; i < MaxChannels; i++) requestChannel(i);
}

void CompanionClient::requestChannel(int index) {
    Writer w(CmdGetChannel);
    w.u8(quint8(index));
    enqueue(w.bytes(), [this](quint8 code, Reader& r) {
        if (code == RespChannelInfo) {
            model::Channel ch;
            ch.index = r.u8();
            ch.name = r.fixedString(ChannelNameField);
            ch.secret = r.take(ChannelSecretSize);
            ch.type = model::Channel::classify(ch.name, ch.secret);
            if (r.ok() && ch.configured()) channels_.append(ch);
        }
        // An ERR for one slot says nothing about the others; keep enumerating.

        if (--channelsOutstanding_ == 0) {
            std::sort(channels_.begin(), channels_.end(),
                      [](const model::Channel& a, const model::Channel& b) {
                          return a.index < b.index;
                      });
            Q_EMIT channelsChanged(channels_);
            backoffMs_ = 0;  // a full handshake proves the link is good
            setState(State::Ready);
            // Anything received while no app was attached is still queued in
            // the daemon; collect it before going idle.
            requestSync();
        }
        return true;
    });
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void CompanionClient::setChannel(int channelIndex, const QString& name,
                                 const QByteArray& secret) {
    if (state_ != State::Ready) {
        Q_EMIT channelSaveResult(channelIndex, false, QStringLiteral("not connected"));
        return;
    }
    // padded() would quietly stretch or clip a wrong-sized key into a different
    // channel, so refuse it here instead of joining something nobody is on.
    if (secret.size() != ChannelSecretSize || channelIndex < 0 || channelIndex >= MaxChannels) {
        Q_EMIT channelSaveResult(channelIndex, false, QStringLiteral("invalid channel"));
        return;
    }

    // index(1) name(32) secret(16)
    Writer w(CmdSetChannel);
    w.u8(quint8(channelIndex))
        .padded(name.toUtf8(), ChannelNameField)
        .padded(secret, ChannelSecretSize);

    enqueue(w.bytes(), [this, channelIndex](quint8 code, Reader& r) {
        if (code == RespOk) {
            readBackChannel(channelIndex);
        } else {
            Q_EMIT channelSaveResult(
                channelIndex, false,
                code == RespErr ? errorText(r.u8()) : QStringLiteral("unexpected reply"));
        }
        return true;
    });
}

void CompanionClient::readBackChannel(int index) {
    // The store truncates a long name to its 32-byte field, so what the app
    // shows comes from the slot rather than from what it asked for.
    Writer w(CmdGetChannel);
    w.u8(quint8(index));
    enqueue(w.bytes(), [this, index](quint8 code, Reader& r) {
        if (code != RespChannelInfo) {
            Q_EMIT channelSaveResult(
                index, false, QStringLiteral("saved, but the slot could not be read back"));
            return true;
        }

        model::Channel ch;
        ch.index = r.u8();
        ch.name = r.fixedString(ChannelNameField);
        ch.secret = r.take(ChannelSecretSize);
        ch.type = model::Channel::classify(ch.name, ch.secret);
        if (!r.ok() || !ch.configured()) {
            Q_EMIT channelSaveResult(index, false, QStringLiteral("the slot is still empty"));
            return true;
        }

        // Slots are the identity: writing an occupied one replaces it rather
        // than adding a second row for the same channel.
        auto it = std::find_if(channels_.begin(), channels_.end(),
                               [&](const model::Channel& c) { return c.index == ch.index; });
        if (it != channels_.end())
            *it = ch;
        else
            channels_.append(ch);
        std::sort(channels_.begin(), channels_.end(),
                  [](const model::Channel& a, const model::Channel& b) {
                      return a.index < b.index;
                  });

        Q_EMIT channelsChanged(channels_);
        Q_EMIT channelSaveResult(ch.index, true, {});
        return true;
    });
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void CompanionClient::requestSync() {
    if (syncPending_) return;
    syncPending_ = true;

    Writer w(CmdSyncNextMessage);
    enqueue(w.bytes(), [this](quint8 code, Reader& r) {
        syncPending_ = false;

        if (code == RespChannelMsgRecvV3 || code == RespContactMsgRecvV3) {
            const qint8 snrQ4 = r.i8();
            r.skip(2);  // reserved

            if (code == RespChannelMsgRecvV3) {
                const int channelIndex = r.u8();
                model::Message msg = parseMessageTail(r, snrQ4, channelIndex);
                if (r.ok()) Q_EMIT messageReceived(msg);
            } else {
                const QByteArray prefix = r.take(6);
                model::Message msg = parseMessageTail(r, snrQ4, -1);
                // No contact sync in v1, so the peer is identified by the key
                // prefix the daemon matched on.
                msg.sender = QString::fromLatin1(prefix.toHex());
                if (r.ok()) Q_EMIT directMessageReceived(msg);
            }

            // Drain: the daemon pushes MSG_WAITING once per message, but a
            // backlog collected at connect time has no pushes left to trigger.
            requestSync();
        }
        // RESP_NO_MORE_MESSAGES (or an error) ends the drain.
        return true;
    });
}

model::Message CompanionClient::parseMessageTail(Reader& r, qint8 snrQ4, int channelIndex) {
    model::Message msg;
    msg.channelIndex = channelIndex;
    msg.pathLen = r.u8();
    r.skip(1);  // txt_type: v1 renders every type as plain text
    msg.timestamp = messageTime(r.u32());
    msg.hasSignal = true;
    msg.snr = snrQ4 / 4.0f;  // the wire carries SNR in quarter-dB

    QString raw = QString::fromUtf8(r.rest());
    if (channelIndex >= 0)
        splitSender(raw, msg.sender, msg.text);
    else
        msg.text = raw;
    return msg;
}

void CompanionClient::sendChannelMessage(int channelIndex, const QString& text, int token) {
    if (state_ != State::Ready) {
        Q_EMIT sendResult(token, false, QStringLiteral("not connected"));
        return;
    }

    // txt_type(1) channel_index(1) timestamp(4) message
    Writer w(CmdSendChannelTxtMsg);
    w.u8(TxtPlain)
        .u8(quint8(channelIndex))
        .u32(quint32(QDateTime::currentSecsSinceEpoch()))
        .tail(text.toUtf8());

    enqueue(
        w.bytes(),
        [this, token](quint8 code, Reader& r) {
            if (code == RespOk) {
                // A channel message has no addressee and so no ack to wait for;
                // the daemon's OK means it reached the transmit queue.
                Q_EMIT sendResult(token, true, {});
            } else if (code == RespErr) {
                Q_EMIT sendResult(token, false, errorText(r.u8()));
            } else {
                Q_EMIT sendResult(token, false, QStringLiteral("unexpected reply"));
            }
            return true;
        },
        [this, token] { Q_EMIT sendResult(token, false, QStringLiteral("connection lost")); });
}

}  // namespace proto
