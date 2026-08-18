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
constexpr int BatteryPollMs = 60000;

// Companion firmware reports a single-cell battery voltage, while the pane
// needs a percentage. Use a deliberately simple linear estimate: a discharged
// cell is 3.0 V and a full one is 4.2 V. Values outside that range are clamped,
// and zero is the protocol's "unavailable" value.
int batteryPercent(quint16 millivolts) {
    constexpr int EmptyMv = 3000;
    constexpr int FullMv = 4200;
    if (millivolts == 0) return -1;
    return std::clamp((int(millivolts) - EmptyMv) * 100 / (FullMv - EmptyMv), 0, 100);
}

// Timestamps arrive from other nodes' clocks, which on a mesh device may be
// unset. Anything before 2020 is not a real message time, so fall back to
// arrival time rather than showing 1970.
constexpr qint64 PlausibleEpoch = 1577836800;  // 2020-01-01Z

QDateTime messageTime(quint32 wire) {
    if (qint64(wire) < PlausibleEpoch) return QDateTime::currentDateTime();
    return QDateTime::fromSecsSinceEpoch(wire);
}

// An advert's timestamp comes from the advertising node's clock, and a node
// that has never been told the time advertises from 1970. There is nothing to
// fall back on the way an arriving message has its arrival, so an implausible
// one becomes no time at all and the UI says so.
QDateTime advertTime(quint32 wire) {
    if (qint64(wire) < PlausibleEpoch) return {};
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

    batteryTimer_ = new QTimer(this);
    batteryTimer_->setInterval(BatteryPollMs);
    connect(batteryTimer_, &QTimer::timeout, this, &CompanionClient::requestBattery);
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
    const bool wasRunning = running_;
    running_ = false;
    reconnectTimer_->stop();
    if (transport_) transport_->close();
    resetConnection();
    // Stopping the retry loop is user-visible even if its most recent attempt
    // had already put the state in Disconnected. Listeners still need to turn
    // a Disconnect action back into Connect.
    if (wasRunning && state_ == State::Disconnected)
        Q_EMIT stateChanged(State::Disconnected, {});
    else
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
    setMessageSyncing(false);
    // Each session preflights its own storage: the endpoint may now be serving
    // a different radio, and that device's database is a different file which
    // may or may not open.
    storageAvailable_ = false;
    channelsOutstanding_ = 0;
    incomingContacts_.clear();
    contactFetches_.clear();
    replyTimer_->stop();
    batteryTimer_->stop();

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
            // All three carry the node's key and nothing else, so what changed
            // about it has to be asked for; a node heard for the first time and
            // one heard again are the same fetch.
            requestContact(r.take(32));
            break;
        case PushSendConfirmed:
        case PushLogRxData:
        default:
            // The rest of the push surface -- per-message ack confirmation, raw
            // RX logging -- is deliberately ignored rather than half-handled.
            break;
    }
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

void CompanionClient::beginHandshake() {
    channels_.clear();
    channelsOutstanding_ = 0;
    contacts_.clear();

    // APP_START: version byte, six reserved, then the app name.
    Writer start(CmdAppStart);
    start.u8(1).padded({}, 6).tail(QByteArrayLiteral("corelet"));
    enqueue(start.bytes(), [this](quint8 code, Reader& r) {
        if (code != RespSelfInfo) {
            // Whatever is on the other end, it is not speaking this protocol.
            transport_->close();
            onClosed(QStringLiteral("unexpected handshake reply"));
            return true;
        }

        DeviceInfo info;
        r.skip(1);  // adv_type
        info.txPowerDbm = r.i8();
        r.skip(1);  // max tx power
        info.pubkey = r.take(32);
        const qint32 latE6 = r.i32();
        const qint32 lonE6 = r.i32();
        info.hasLocation = latE6 != 0 || lonE6 != 0;
        info.latitude = latE6 / 1000000.0;
        info.longitude = lonE6 / 1000000.0;
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

    // Behind the channels: the sidebar is what the window opens on, and the
    // address book is a screen the user has to ask for.
    requestContacts();
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
            requestBattery();
            batteryTimer_->start();
            // Anything received while no app was attached is still queued in
            // the daemon; collect it before going idle.
            requestSync();
        }
        return true;
    });
}

void CompanionClient::requestBattery() {
    if (state_ != State::Ready) return;

    Writer w(CmdGetBatteryVoltage);
    enqueue(w.bytes(), [this](quint8 code, Reader& r) {
        if (code != RespBatteryVoltage) return true;

        const quint16 millivolts = r.u16();
        if (!r.ok()) return true;
        const int percent = batteryPercent(millivolts);
        const int storedMillivolts = millivolts == 0 ? -1 : int(millivolts);
        if (storedMillivolts == device_.batteryMillivolts &&
            percent == device_.batteryPercent)
            return true;

        device_.batteryMillivolts = storedMillivolts;
        device_.batteryPercent = percent;
        Q_EMIT deviceInfoChanged(device_);
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
    // The field is a fixed 32 bytes and padded() would cut a name to fit, quite
    // possibly through the middle of a character. This is the one place every
    // caller passes through, so refusing here is what keeps that from happening.
    const QByteArray nameBytes = name.toUtf8();
    if (nameBytes.size() > MaxChannelNameBytes) {
        Q_EMIT channelSaveResult(channelIndex, false,
                                 QStringLiteral("the name needs %1 bytes and the field holds %2")
                                     .arg(nameBytes.size())
                                     .arg(MaxChannelNameBytes));
        return;
    }

    // index(1) name(32) secret(16)
    Writer w(CmdSetChannel);
    w.u8(quint8(channelIndex))
        .padded(nameBytes, ChannelNameField)
        .padded(secret, ChannelSecretSize);

    enqueue(w.bytes(), [this, channelIndex](quint8 code, Reader& r) {
        if (code == RespOk) {
            readBackChannel(channelIndex, /*cleared=*/false);
        } else {
            Q_EMIT channelSaveResult(
                channelIndex, false,
                code == RespErr ? errorText(r.u8()) : QStringLiteral("unexpected reply"));
        }
        return true;
    });
}

void CompanionClient::clearChannel(int channelIndex) {
    if (state_ != State::Ready) {
        Q_EMIT channelRemoveResult(channelIndex, false, QStringLiteral("not connected"));
        return;
    }
    if (channelIndex < 0 || channelIndex >= MaxChannels) {
        Q_EMIT channelRemoveResult(channelIndex, false, QStringLiteral("invalid channel"));
        return;
    }

    // An all-zero key is a slot nobody is using, which is how the enumeration
    // already tells configured slots from free ones. The name goes with it so
    // nothing is left behind for the next channel written here to inherit.
    Writer w(CmdSetChannel);
    w.u8(quint8(channelIndex))
        .padded(QByteArray(), ChannelNameField)
        .padded(QByteArray(ChannelSecretSize, '\0'), ChannelSecretSize);

    enqueue(w.bytes(), [this, channelIndex](quint8 code, Reader& r) {
        if (code == RespOk) {
            readBackChannel(channelIndex, /*cleared=*/true);
        } else {
            Q_EMIT channelRemoveResult(
                channelIndex, false,
                code == RespErr ? errorText(r.u8()) : QStringLiteral("unexpected reply"));
        }
        return true;
    });
}

void CompanionClient::readBackChannel(int index, bool cleared) {
    // The store is the authority on what a slot ended up holding, so what the
    // app shows comes from the slot rather than from what it asked for. A
    // cleared slot is read back for the same reason in reverse: the write is
    // only believed once the device agrees the channel is gone.
    Writer w(CmdGetChannel);
    w.u8(quint8(index));
    enqueue(w.bytes(), [this, index, cleared](quint8 code, Reader& r) {
        const auto report = [this, cleared](int slot, bool ok, const QString& error) {
            if (cleared)
                Q_EMIT channelRemoveResult(slot, ok, error);
            else
                Q_EMIT channelSaveResult(slot, ok, error);
        };

        if (code != RespChannelInfo) {
            report(index, false,
                   cleared ? QStringLiteral("removed, but the slot could not be read back")
                           : QStringLiteral("saved, but the slot could not be read back"));
            return true;
        }

        model::Channel ch;
        ch.index = r.u8();
        ch.name = r.fixedString(ChannelNameField);
        ch.secret = r.take(ChannelSecretSize);
        ch.type = model::Channel::classify(ch.name, ch.secret);

        if (cleared) {
            if (!r.ok()) {
                report(index, false,
                       QStringLiteral("removed, but the slot could not be read back"));
                return true;
            }
            if (ch.configured()) {
                report(index, false, QStringLiteral("the slot is still in use"));
                return true;
            }

            channels_.erase(std::remove_if(channels_.begin(), channels_.end(),
                                           [index](const model::Channel& c) {
                                               return c.index == index;
                                           }),
                            channels_.end());
            Q_EMIT channelsChanged(channels_);
            report(index, true, {});
            return true;
        }

        if (!r.ok() || !ch.configured()) {
            report(index, false, QStringLiteral("the slot is still empty"));
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
        report(ch.index, true, {});
        return true;
    });
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------

model::Contact CompanionClient::parseContact(Reader& r) {
    // pubkey(32) type(1) flags(1) out_path_len(1) out_path(64) name(32)
    // last_advert(4) lat(4) lon(4) last_modified(4)
    model::Contact c;
    c.pubkey = r.take(32);
    c.type = model::contactTypeFromInt(r.u8());
    r.skip(1);  // flags: nothing above the protocol reads them yet
    c.pathLen = r.u8();
    r.skip(ContactPathField);
    c.name = r.fixedString(ContactNameField);
    c.lastAdvert = advertTime(r.u32());
    // The advertised location and the store's own modification time follow.
    // Nothing shows either yet, so they are left where they are rather than
    // carried around unread.
    return c;
}

void CompanionClient::requestContacts() {
    // No `since` filter, which the daemon would accept: the app keeps no
    // contact cache between sessions, so an incremental answer would leave it
    // holding a list it has no way to complete.
    Writer w(CmdGetContacts);
    enqueue(w.bytes(), [this](quint8 code, Reader& r) {
        switch (code) {
            case RespContactsStart:
                incomingContacts_.clear();
                return false;  // the count only; the contacts follow
            case RespContact: {
                const model::Contact contact = parseContact(r);
                if (r.ok() && contact.pubkey.size() == 32) incomingContacts_.append(contact);
                return false;
            }
            default:
                // END_OF_CONTACTS, or an ERR from a far end with nothing to
                // say: either way what has arrived is now the address book,
                // and an empty one is worth announcing so a stale list from
                // the previous session goes away.
                contacts_ = incomingContacts_;
                incomingContacts_.clear();
                Q_EMIT contactsChanged(contacts_);
                return true;
        }
    });
}

void CompanionClient::requestContact(const QByteArray& pubkey) {
    if (pubkey.size() != 32) return;
    if (contactFetches_.contains(pubkey)) return;
    contactFetches_.insert(pubkey);

    Writer w(CmdGetContactByKey);
    w.tail(pubkey);
    enqueue(
        w.bytes(),
        [this, pubkey](quint8 code, Reader& r) {
            contactFetches_.remove(pubkey);
            // A NOT_FOUND is ordinary: the store is bounded, and a contact can
            // be dropped between the push and this reading it back.
            if (code != RespContact) return true;
            const model::Contact contact = parseContact(r);
            if (r.ok() && contact.pubkey.size() == 32) upsertContact(contact);
            return true;
        },
        [this, pubkey] { contactFetches_.remove(pubkey); });
}

void CompanionClient::upsertContact(const model::Contact& contact) {
    // The key is the identity: a node that renamed itself is the same contact,
    // and a second row for it would be a second person.
    auto it = std::find_if(contacts_.begin(), contacts_.end(),
                           [&](const model::Contact& c) { return c.pubkey == contact.pubkey; });
    if (it != contacts_.end())
        *it = contact;
    else
        contacts_.append(contact);
    Q_EMIT contactChanged(contact);
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void CompanionClient::setStorageAvailable(bool available) {
    if (storageAvailable_ == available) return;
    storageAvailable_ = available;
    if (!available) setMessageSyncing(false);
    // Whatever the drain stopped on is still sitting in the daemon's inbox, so
    // there is a backlog to pick up the moment there is somewhere to put it.
    if (available && state_ == State::Ready) requestSync();
}

void CompanionClient::requestSync() {
    // Asking pops the message off the node, so this is the one command that is
    // not safe to send speculatively: with nowhere to write the answer down,
    // sending it is how messages get destroyed.
    if (!storageAvailable_) return;
    if (syncPending_) return;
    syncPending_ = true;
    setMessageSyncing(true);

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
            // Both signals above are delivered directly, so the message is
            // either written down or storage has been turned off by the time
            // this runs -- which is what makes it safe to ask for the next one.
            requestSync();
        } else {
            // RESP_NO_MORE_MESSAGES (or an error) ends the drain.
            setMessageSyncing(false);
        }
        return true;
    });
}

void CompanionClient::setMessageSyncing(bool syncing) {
    if (messageSyncing_ == syncing) return;
    messageSyncing_ = syncing;
    Q_EMIT messageSyncChanged(syncing);
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

    // A message that will not fit one mesh payload is refused by the node after
    // it has been encrypted, and answered with a NOT_FOUND that says nothing
    // about length. The budget is knowable here -- the node prepends the name
    // SELF_INFO gave us -- so say so plainly instead.
    const QByteArray body = text.toUtf8();
    const int budget = maxMessageBytes(device_.name);
    if (body.size() > budget) {
        Q_EMIT sendResult(token, false,
                          QStringLiteral("the message needs %1 bytes and only %2 fit")
                              .arg(body.size())
                              .arg(budget));
        return;
    }

    // txt_type(1) channel_index(1) timestamp(4) message
    Writer w(CmdSendChannelTxtMsg);
    w.u8(TxtPlain)
        .u8(quint8(channelIndex))
        .u32(quint32(QDateTime::currentSecsSinceEpoch()))
        .tail(body);

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
