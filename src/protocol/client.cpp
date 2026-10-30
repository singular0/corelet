#include "protocol/client.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <cmath>

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

// RESP_SENT carries the node's own estimate of how long the peer's ack will
// take, and it is a number the far end chose: a zero would report every direct
// message unconfirmed the instant it went out, and a nonsense one would arm a
// timer for days. A mesh round trip is seconds, and the node's retry ladder has
// given up well inside two minutes.
constexpr quint32 MinAckWindowMs = 1000;
constexpr quint32 MaxAckWindowMs = 120000;

// How many direct sends stay open to a late confirmation. Each is a token and
// four bytes, and the daemon will not have more than 64 of its own retries in
// flight, so this is about bounding a long session rather than a live limit.
constexpr int MaxAwaitedAcks = 64;

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
    contactsOutstanding_ = false;
    contactFetches_.clear();
    replyTimer_->stop();
    batteryTimer_->stop();

    // Last, and against an already-clean queue: an abort handler reaches the UI,
    // which may well answer by starting another command.
    for (const Pending& p : dropped)
        if (p.onAbort) p.onAbort();
    abandonAcks();
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
            // ack(4) then the round trip in milliseconds, which nothing here
            // shows: what a reader wants is that the message arrived.
            confirmAck(r.take(AckHashSize));
            break;
        case PushLogRxData:
        default:
            // The rest of the push surface -- raw RX logging above all -- is
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
    contacts_.clear();
    device_ = {};
    rxDelayMilli_ = 0;

    // First, as the official app sends it: upstream firmware frames every
    // message it receives from here on in the V3 shape this client parses only
    // once an app has said it understands that, and the reply carries the
    // repeat state a radio change has to send back.
    requestDeviceQuery();

    // SELF_INFO is what proves the far end speaks the protocol at all, and the
    // window needs it at once: it names the device database, which has to be
    // open before the inbox drain at the end of this handshake.
    requestSelfInfo([this] { Q_EMIT deviceInfoChanged(device_); });

    // The daemon cannot step the system clock, so it tracks an offset from
    // whatever the app tells it. Adverts are rejected as replays without a
    // sane wall clock, so this is not cosmetic.
    Writer time(CmdSetDeviceTime);
    time.u32(quint32(QDateTime::currentSecsSinceEpoch()));
    enqueue(time.bytes());

    requestTuning([this] { Q_EMIT deviceInfoChanged(device_); });

    // Slots are sparse and the store always reports all eight, so ask for every
    // one and keep those with a non-zero key.
    channelsOutstanding_ = MaxChannels;
    for (int i = 0; i < MaxChannels; i++) requestChannel(i);

    // Behind the channels: the sidebar is what the window opens on, and the
    // address book is a screen the user has to ask for.
    requestContacts();
}

void CompanionClient::requestDeviceQuery(AbortHandler onAbort) {
    Writer w(CmdDeviceQuery);
    w.u8(AppProtocolVersion);
    enqueue(
        w.bytes(),
        [this](quint8 code, Reader& r) {
            // Nothing here needs the rest; an answer that is not DEVICE_INFO is
            // left for APP_START to judge.
            if (code != RespDeviceInfo) return true;
            // fw_ver(1) max_contacts/2(1) max_channels(1) ble_pin(4) build(12)
            // model(40) version(20), then upstream's v9 repeat byte. Judged by
            // whether it is there rather than by fw_ver: coreletd claims v3 and
            // appends it anyway, because a missing repeat byte in
            // SET_RADIO_PARAMS turns repeat off.
            r.skip(1 + 1 + 1 + 4 + 12 + 40 + 20);
            if (r.ok() && r.remaining() > 0)
                device_.repeat = r.u8() != 0;
            else
                device_.repeat.reset();
            return true;
        },
        std::move(onAbort));
}

bool CompanionClient::parseSelfInfo(Reader& r, DeviceInfo& info) {
    // adv_type(1) tx(1) max_tx(1) pubkey(32) lat(4) lon(4) multi-acks(1)
    // location policy(1) telemetry mode(1) manual add(1) freq(4) bw(4) sf(1)
    // cr(1) name
    DeviceInfo parsed = info;
    r.skip(1);  // adv_type
    parsed.txPowerDbm = r.i8();
    parsed.maxTxPowerDbm = r.u8();
    parsed.pubkey = r.take(32);
    const qint32 latE6 = r.i32();
    const qint32 lonE6 = r.i32();
    parsed.hasLocation = latE6 != 0 || lonE6 != 0;
    parsed.latitude = latE6 / 1000000.0;
    parsed.longitude = lonE6 / 1000000.0;
    r.skip(4);
    parsed.freqMhz = r.u32() / 1000.0;
    parsed.bwKhz = r.u32() / 1000.0;
    parsed.sf = r.u8();
    parsed.cr = r.u8();
    parsed.name = QString::fromUtf8(r.rest());
    if (!r.ok()) return false;
    info = parsed;
    return true;
}

void CompanionClient::requestSelfInfo(std::function<void()> then, AbortHandler onAbort) {
    // APP_START: version byte, six reserved, then the app name. Sent again to
    // re-read the node after a settings change, since nothing else answers
    // with SELF_INFO; both ends take a repeat as the same app saying hello.
    Writer w(CmdAppStart);
    w.u8(1).padded({}, 6).tail(QByteArrayLiteral("corelet"));
    enqueue(
        w.bytes(),
        [this, then = std::move(then)](quint8 code, Reader& r) {
            if (code != RespSelfInfo) {
                // Whatever is on the other end, it is not speaking this protocol.
                transport_->close();
                onClosed(QStringLiteral("unexpected handshake reply"));
                return true;
            }
            if (!parseSelfInfo(r, device_)) return true;
            if (then) then();
            return true;
        },
        std::move(onAbort));
}

void CompanionClient::requestTuning(std::function<void()> then, AbortHandler onAbort) {
    Writer w(CmdGetTuningParams);
    enqueue(
        w.bytes(),
        [this, then = std::move(then)](quint8 code, Reader& r) {
            // rx_delay(4) airtime_factor(4), each in thousandths. An ERR is a
            // far end that predates the command -- coreletd before it took
            // settings -- and leaves the factor unknown rather than zero, which
            // would read as no limit at all.
            const quint32 rxDelay = r.u32();
            const quint32 factor = r.u32();
            if (code == RespTuningParams && r.ok()) {
                rxDelayMilli_ = rxDelay;
                device_.airtimeFactorMilli = int(qMin(factor, quint32(INT_MAX)));
            } else {
                device_.airtimeFactorMilli.reset();
            }
            if (then) then();
            return true;
        },
        std::move(onAbort));
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
// Adverts
// ---------------------------------------------------------------------------

void CompanionClient::sendAdvert(bool flood) {
    if (state_ != State::Ready) {
        Q_EMIT advertResult(flood, false, QStringLiteral("not connected"));
        return;
    }

    // flood(1). An absent byte means flood at the far end, but say it either
    // way: which of the two this is is the whole content of the command, and it
    // should not rest on a default the app cannot see.
    Writer w(CmdSendSelfAdvert);
    w.u8(flood ? 1 : 0);

    enqueue(
        w.bytes(),
        [this, flood](quint8 code, Reader& r) {
            if (code == RespOk) {
                // The node has taken the packet for transmission. Nothing
                // acknowledges an advert -- it is addressed to everybody -- so
                // there is no second answer coming and this is the end of it.
                Q_EMIT advertResult(flood, true, {});
            } else if (code == RespErr) {
                Q_EMIT advertResult(flood, false, errorText(r.u8()));
            } else {
                Q_EMIT advertResult(flood, false, QStringLiteral("unexpected reply"));
            }
            return true;
        },
        [this, flood] { Q_EMIT advertResult(flood, false, QStringLiteral("connection lost")); });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

namespace {

// The wire's units: SET_RADIO_PARAMS takes kHz and whole hertz, and
// SELF_INFO reports the same, rounded rather than truncated -- 869.525 is a
// hair under itself in binary.
quint32 toKhz(double mhz) { return quint32(std::lround(mhz * 1000.0)); }
quint32 toHz(double khz) { return quint32(std::lround(khz * 1000.0)); }
qint32 toE6(double degrees) { return qint32(std::lround(degrees * 1000000.0)); }

// A bandwidth arrives in whole hertz and the far end snaps it to one its radio
// has -- 7800 for the SX1262's 7.81 kHz -- so what it reports back is that and
// not what was sent. The radio's bandwidths are far enough apart that anything
// within 1% is the same one.
bool sameBandwidth(double a, double b) { return std::fabs(a - b) <= 0.01 * std::max(a, b); }

QString radioText(double freqMhz, double bwKhz, int sf, int cr) {
    return QStringLiteral("%1 MHz, %2 kHz, SF%3, CR%4")
        .arg(freqMhz, 0, 'f', 3)
        .arg(bwKhz)
        .arg(sf)
        .arg(cr);
}

}  // namespace

void CompanionClient::applySettings(const SettingsChange& change) {
    auto batch = std::make_shared<SettingsBatch>();
    batch->change = change;
    SettingsResult& result = batch->result;

    QVector<Setting>& requested = batch->requested;
    if (change.name) requested.append(Setting::Name);
    if (change.position) requested.append(Setting::Position);
    if (change.radio) requested.append(Setting::Radio);
    if (change.txPowerDbm) requested.append(Setting::TxPower);
    if (change.airtimeFactorMilli) requested.append(Setting::AirtimeFactor);

    if (state_ != State::Ready) {
        for (const Setting setting : requested)
            result.refused.insert(setting, QStringLiteral("not connected"));
        Q_EMIT settingsApplied(result);
        return;
    }

    const auto queue = [this, batch](Setting setting, const QByteArray& payload) {
        enqueue(
            payload,
            [this, batch, setting](quint8 code, Reader& r) {
                settleSetting(batch, setting, code, r);
                return true;
            },
            [this, batch] { abortSettings(batch); });
    };
    bool sent = false;

    // The name and the position share the advert's 32 bytes, and the far end
    // checks each against the other as it stands when the command arrives. So
    // a position going away goes first, leaving the name the room it is about
    // to need; otherwise the name goes first, and is held to the room it will
    // have beside the position that follows it.
    const bool positionAfter = change.position ? change.position->advertised : device_.hasLocation;
    const bool positionFirst = change.position && !change.position->advertised;

    const auto sendPosition = [&] {
        const SettingsChange::Position& p = *change.position;
        // Written so that a NaN fails it too.
        if (p.advertised && !(std::abs(p.latitude) <= 90.0 && std::abs(p.longitude) <= 180.0)) {
            result.refused.insert(Setting::Position, QStringLiteral("not a position on Earth"));
            return;
        }
        const qint32 lat = p.advertised ? toE6(p.latitude) : 0;
        const qint32 lon = p.advertised ? toE6(p.longitude) : 0;
        if (p.advertised && lat == 0 && lon == 0) {
            result.refused.insert(Setting::Position,
                                  QStringLiteral("0, 0 is how the node is told to stop "
                                                 "advertising a position"));
            return;
        }
        // lat(4) lon(4), in millionths of a degree.
        Writer w(CmdSetAdvertLatLon);
        w.u32(quint32(lat)).u32(quint32(lon));
        queue(Setting::Position, w.bytes());
        sent = true;
    };

    if (change.position && positionFirst) sendPosition();

    if (change.name) {
        const QByteArray bytes = change.name->toUtf8();
        const int budget = maxAdvertNameBytes(positionAfter);
        if (bytes.isEmpty()) {
            result.refused.insert(Setting::Name, QStringLiteral("a node needs a name"));
        } else if (bytes.size() > budget) {
            // Refused rather than cut, as coreletd refuses it: a name nobody
            // chose is worse than the one already there.
            result.refused.insert(
                Setting::Name,
                QStringLiteral("the name needs %1 bytes and the advert holds %2%3")
                    .arg(bytes.size())
                    .arg(budget)
                    .arg(positionAfter ? QStringLiteral(" beside a position") : QString()));
        } else {
            // The rest of the frame is the name; nothing terminates it.
            Writer w(CmdSetAdvertName);
            w.tail(bytes);
            queue(Setting::Name, w.bytes());
            sent = true;
        }
    }

    if (change.position && !positionFirst) sendPosition();

    if (change.radio) {
        const SettingsChange::Radio& radio = *change.radio;
        const bool usable = radio.freqMhz >= MinFreqMhz && radio.freqMhz <= MaxFreqMhz &&
                            radio.bwKhz >= 7.0 && radio.bwKhz <= 500.0 && radio.sf >= 5 &&
                            radio.sf <= 12 && radio.cr >= 5 && radio.cr <= 8;
        if (!usable) {
            result.refused.insert(Setting::Radio, QStringLiteral("not settings a radio can use"));
        } else if (radio.repeat && !repeatAllowedAt(toKhz(radio.freqMhz))) {
            result.refused.insert(Setting::Radio,
                                  QStringLiteral("repeat is only allowed at 433.000, 869.495 "
                                                 "and 918.000 MHz"));
        } else {
            // freq(4) in kHz, bw(4) in Hz, sf(1), cr(1), repeat(1). The last
            // byte always goes, whatever the far end claims to understand: an
            // absent one turns repeat off, so a preset change that left it out
            // would quietly stop the node relaying.
            Writer w(CmdSetRadioParams);
            w.u32(toKhz(radio.freqMhz))
                .u32(toHz(radio.bwKhz))
                .u8(quint8(radio.sf))
                .u8(quint8(radio.cr))
                .u8(radio.repeat ? 1 : 0);
            queue(Setting::Radio, w.bytes());
            sent = true;
        }
    }

    if (change.txPowerDbm) {
        const int dbm = *change.txPowerDbm;
        const int ceiling = std::max(device_.maxTxPowerDbm, device_.txPowerDbm);
        if (dbm < MinTxPowerDbm || dbm > ceiling) {
            result.refused.insert(Setting::TxPower,
                                  QStringLiteral("the radio takes %1 to %2 dBm")
                                      .arg(MinTxPowerDbm)
                                      .arg(ceiling));
        } else {
            // A signed byte, as both ends read it.
            Writer w(CmdSetRadioTxPower);
            w.u8(quint8(qint8(dbm)));
            queue(Setting::TxPower, w.bytes());
            sent = true;
        }
    }

    if (change.airtimeFactorMilli) {
        const int factor = *change.airtimeFactorMilli;
        if (factor < 0 || factor > MaxAirtimeFactorMilli) {
            result.refused.insert(Setting::AirtimeFactor,
                                  QStringLiteral("the airtime factor runs from 0 to 9"));
        } else {
            // rx_delay(4) airtime_factor(4), each in thousandths.
            Writer w(CmdSetTuningParams);
            w.u32(rxDelayMilli_).u32(quint32(factor));
            queue(Setting::AirtimeFactor, w.bytes());
            sent = true;
        }
    }

    if (!sent) {
        batch->finished = true;
        Q_EMIT settingsApplied(result);
        return;
    }

    // Read back everything a change can have touched. The node is the
    // authority on what it is now running, so what the app shows comes from
    // here rather than from what it asked for.
    const AbortHandler abort = [this, batch] { abortSettings(batch); };
    requestDeviceQuery(abort);
    requestSelfInfo({}, abort);
    requestTuning([this, batch] { finishSettings(batch); }, abort);
}

void CompanionClient::settleSetting(const std::shared_ptr<SettingsBatch>& batch,
                                    Setting setting, quint8 code, Reader& r) {
    batch->answered.insert(setting);
    switch (code) {
        case RespOk:
            batch->accepted.insert(setting);
            break;
        case RespDisabled:
            // The node's own policy rather than anything wrong with the value:
            // an older coreletd answers every setter this way, and an operator
            // can turn them off. Remembered so the settings show read-only.
            batch->result.disabled = true;
            device_.settingsDisabled = true;
            break;
        case RespErr:
            batch->result.refused.insert(setting, errorText(r.u8()));
            break;
        default:
            batch->result.refused.insert(setting, QStringLiteral("unexpected reply"));
            break;
    }
}

void CompanionClient::finishSettings(const std::shared_ptr<SettingsBatch>& batch) {
    if (batch->finished) return;
    batch->finished = true;

    const SettingsChange& change = batch->change;
    SettingsResult& result = batch->result;
    const auto unlike = [&](Setting setting, const QString& reported) {
        result.refused.insert(setting,
                              QStringLiteral("accepted, but the node reports %1").arg(reported));
    };

    if (batch->accepted.contains(Setting::Name) && device_.name != *change.name)
        unlike(Setting::Name, QStringLiteral("\"%1\"").arg(device_.name));

    if (batch->accepted.contains(Setting::Position)) {
        const SettingsChange::Position& p = *change.position;
        const bool same = p.advertised ? device_.hasLocation &&
                                             toE6(device_.latitude) == toE6(p.latitude) &&
                                             toE6(device_.longitude) == toE6(p.longitude)
                                       : !device_.hasLocation;
        if (!same)
            unlike(Setting::Position,
                   device_.hasLocation ? QStringLiteral("%1, %2")
                                             .arg(device_.latitude, 0, 'f', 6)
                                             .arg(device_.longitude, 0, 'f', 6)
                                       : QStringLiteral("no position"));
    }

    if (batch->accepted.contains(Setting::Radio)) {
        const SettingsChange::Radio& radio = *change.radio;
        if (toKhz(device_.freqMhz) != toKhz(radio.freqMhz) ||
            !sameBandwidth(device_.bwKhz, radio.bwKhz) || device_.sf != radio.sf ||
            device_.cr != radio.cr)
            unlike(Setting::Radio, radioText(device_.freqMhz, device_.bwKhz, device_.sf,
                                             device_.cr));
        else if (device_.repeat && *device_.repeat != radio.repeat)
            unlike(Setting::Radio, *device_.repeat ? QStringLiteral("repeat still on")
                                                   : QStringLiteral("repeat still off"));
    }

    if (batch->accepted.contains(Setting::TxPower) && device_.txPowerDbm != *change.txPowerDbm)
        unlike(Setting::TxPower, QStringLiteral("%1 dBm").arg(device_.txPowerDbm));

    if (batch->accepted.contains(Setting::AirtimeFactor) &&
        device_.airtimeFactorMilli != change.airtimeFactorMilli)
        unlike(Setting::AirtimeFactor,
               device_.airtimeFactorMilli
                   ? QStringLiteral("an airtime factor of %1")
                         .arg(*device_.airtimeFactorMilli / 1000.0)
                   : QStringLiteral("no airtime factor"));

    for (const Setting setting : std::as_const(batch->accepted))
        if (!result.refused.contains(setting)) result.applied.insert(setting);

    Q_EMIT deviceInfoChanged(device_);
    Q_EMIT settingsApplied(result);
}

void CompanionClient::abortSettings(const std::shared_ptr<SettingsBatch>& batch) {
    if (batch->finished) return;
    batch->finished = true;
    // Whatever was refused stands. Whatever was accepted was never read back,
    // and whatever was not answered may or may not have reached the node;
    // there is no link left to ask about either, so neither counts as applied,
    // and the next handshake reads the node afresh.
    for (const Setting setting : batch->requested)
        if (!batch->answered.contains(setting) && !batch->result.refused.contains(setting))
            batch->result.refused.insert(setting, QStringLiteral("connection lost"));
    Q_EMIT settingsApplied(batch->result);
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
    // One stream at a time. A backlog of direct messages from a peer this end
    // cannot name would otherwise queue a full re-enumeration per message, and
    // the first answer names the peer for all of them anyway.
    if (contactsOutstanding_) return;
    contactsOutstanding_ = true;

    // No `since` filter, which the daemon would accept: the app keeps no
    // contact cache between sessions, so an incremental answer would leave it
    // holding a list it has no way to complete.
    Writer w(CmdGetContacts);
    enqueue(
        w.bytes(),
        [this](quint8 code, Reader& r) {
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
                contactsOutstanding_ = false;
                contacts_ = incomingContacts_;
                incomingContacts_.clear();
                Q_EMIT contactsChanged(contacts_);
                return true;
            }
        },
        [this] { contactsOutstanding_ = false; });
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

model::Conversation CompanionClient::channelConversation(int slot) const {
    for (const model::Channel& channel : channels_)
        if (channel.index == slot) return model::Conversation::channel(channel.keyFingerprint());
    return {};
}

model::Conversation CompanionClient::directConversation(const QByteArray& prefix) const {
    const model::Conversation byPrefix = model::Conversation::direct(prefix);
    if (const model::Contact* peer = contactFor(byPrefix))
        return model::Conversation::direct(peer->pubkey);
    return byPrefix;
}

const model::Contact* CompanionClient::contactFor(
    const model::Conversation& conversation) const {
    if (!conversation.isDirect() || conversation.id.isEmpty()) return nullptr;
    // A conversation carries either the peer's whole key or the six bytes the
    // wire gave for it, and six bytes is exactly what the daemon matched the
    // peer on, so one prefix comparison answers both.
    for (const model::Contact& contact : contacts_)
        if (contact.pubkey.startsWith(conversation.id)) return &contact;
    return nullptr;
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
                // A slot holding no configured channel leaves the conversation
                // invalid, which is what tells the owner to store the message
                // out of the way rather than throw it after the node already
                // discarded its copy.
                msg.conversation = channelConversation(channelIndex);
                if (r.ok()) Q_EMIT messageReceived(msg);
            } else {
                const QByteArray prefix = r.take(6);
                model::Message msg = parseMessageTail(r, snrQ4, -1);
                msg.conversation = directConversation(prefix);
                if (r.ok()) {
                    if (const model::Contact* peer = contactFor(msg.conversation))
                        msg.sender = peer->displayName();
                    else
                        // The daemon matched this key against its own contact
                        // store, so a list that cannot name it is a stale copy
                        // rather than a peer nobody has heard of. The message
                        // still lands under its prefix either way.
                        requestContacts();
                    Q_EMIT directMessageReceived(msg);
                }
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

void CompanionClient::sendDirectMessage(const model::Conversation& conversation,
                                        const QString& text, int token) {
    if (state_ != State::Ready) {
        Q_EMIT sendResult(token, false, QStringLiteral("not connected"));
        return;
    }
    if (!conversation.isDirect() || !conversation.isValid()) {
        Q_EMIT sendResult(token, false, QStringLiteral("no peer to send to"));
        return;
    }

    // Nothing is prepended to a direct message, so unlike a channel send the
    // budget is a constant -- but it is still a byte count, and going over it
    // comes back from the node as an unexplained refusal after the text has
    // been encrypted. Say what is wrong instead.
    const QByteArray body = text.toUtf8();
    if (body.size() > MaxDirectTextBytes) {
        Q_EMIT sendResult(token, false,
                          QStringLiteral("the message needs %1 bytes and only %2 fit")
                              .arg(body.size())
                              .arg(MaxDirectTextBytes));
        return;
    }

    // txt_type(1) attempt(1) timestamp(4) pubkey_prefix(6) message. The attempt
    // counter stays zero: the node runs its own retry ladder and this is always
    // the app asking once.
    Writer w(CmdSendTxtMsg);
    w.u8(TxtPlain)
        .u8(0)
        .u32(quint32(QDateTime::currentSecsSinceEpoch()))
        .padded(conversation.peerPrefix(), model::Conversation::PeerPrefixSize)
        .tail(body);

    enqueue(
        w.bytes(),
        [this, token](quint8 code, Reader& r) {
            if (code == RespSent) {
                // route(1): 0 for a known path, 1 for a flood, which is the
                // node's business rather than anything to show. Then the ack
                // the peer is expected to answer with, and how long the node
                // suggests waiting for it.
                r.skip(1);
                const QByteArray ack = r.take(AckHashSize);
                const quint32 window = r.u32();
                // Taken by the daemon is what this answers; whether it arrives
                // follows separately, or not at all.
                Q_EMIT sendResult(token, true, {});
                // A SENT too short to carry an ack -- an older firmware, a
                // truncated frame -- leaves the message where it is rather than
                // reporting a confirmation nothing could ever settle.
                if (r.ok()) awaitAck(token, ack, window);
            } else if (code == RespErr) {
                Q_EMIT sendResult(token, false, errorText(r.u8()));
            } else {
                Q_EMIT sendResult(token, false, QStringLiteral("unexpected reply"));
            }
            return true;
        },
        [this, token] { Q_EMIT sendResult(token, false, QStringLiteral("connection lost")); });
}

// ---------------------------------------------------------------------------
// Delivery acknowledgement
// ---------------------------------------------------------------------------

void CompanionClient::awaitAck(int token, const QByteArray& ack, quint32 suggestedMs) {
    const int windowMs = int(qBound(MinAckWindowMs, suggestedMs, MaxAckWindowMs));

    while (awaitedAcks_.size() >= MaxAwaitedAcks) {
        const AwaitedAck forgotten = awaitedAcks_.takeFirst();
        // Nothing can settle it once it is out of the list, and a row left on a
        // bare tick would never say so.
        if (!forgotten.lapsed) Q_EMIT sendUnconfirmed(forgotten.token);
    }
    awaitedAcks_.append(AwaitedAck {token, ack, false});

    // Bound to `this`, so a session torn down before the window is up takes its
    // timers with it. The entry is looked up again rather than captured: the
    // ack may have arrived, or the cap above may have dropped it.
    QTimer::singleShot(windowMs, this, [this, token] {
        auto it = std::find_if(awaitedAcks_.begin(), awaitedAcks_.end(),
                               [token](const AwaitedAck& a) { return a.token == token; });
        if (it == awaitedAcks_.end() || it->lapsed) return;
        it->lapsed = true;
        Q_EMIT sendUnconfirmed(token);
    });
}

void CompanionClient::confirmAck(const QByteArray& ack) {
    if (ack.size() != AckHashSize) return;
    auto it = std::find_if(awaitedAcks_.begin(), awaitedAcks_.end(),
                           [&ack](const AwaitedAck& a) { return a.ack == ack; });
    // Nothing of ours to settle: an ack for a send from a previous session the
    // daemon is still retrying, or the same one arriving twice, which the
    // firmware says outright can happen.
    if (it == awaitedAcks_.end()) return;

    const int token = it->token;
    awaitedAcks_.erase(it);
    Q_EMIT sendConfirmed(token);
}

void CompanionClient::abandonAcks() {
    QVector<AwaitedAck> dropped;
    dropped.swap(awaitedAcks_);
    // Anything already reported as unconfirmed stays as it is; the rest have
    // just run out of any way to be answered.
    for (const AwaitedAck& a : dropped)
        if (!a.lapsed) Q_EMIT sendUnconfirmed(a.token);
}

}  // namespace proto
