#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <optional>

#include "protocol/client.h"
#include "protocol/frame_codec.h"
#include "protocol/protocol.h"
#include "protocol/transport.h"

namespace {

using Client = proto::CompanionClient;
using Setting = Client::Setting;

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

// A node that keeps the settings it is sent and reports them back, as coreletd
// does, with switches for the ways a real one departs from that.
class FakeNode : public proto::Transport {
public:
    QByteArray name = QByteArrayLiteral("fake-node");
    qint32 latE6 = 0;
    qint32 lonE6 = 0;
    quint32 freqKhz = 869618;
    quint32 bwHz = 62500;
    quint8 sf = 8;
    quint8 cr = 8;
    bool repeat = true;
    qint8 txPower = 20;
    quint8 maxTxPower = 22;
    quint32 airtimeMilli = 9000;

    // An older coreletd: every setter answers RESP_DISABLED.
    bool disabled = false;
    // An older coreletd still: TX power answered OK and changed nothing.
    bool ignoreTxPower = false;
    // A command nobody answers, which leaves it in flight for a link to die under.
    std::optional<quint8> silentOn;

    // Every command payload, in the order it arrived.
    QVector<QByteArray> sent;

    int count(quint8 cmd) const {
        int n = 0;
        for (const QByteArray& payload : sent)
            if (quint8(payload.at(0)) == cmd) n++;
        return n;
    }
    int indexOf(quint8 cmd, int from = 0) const {
        for (int i = from; i < sent.size(); i++)
            if (quint8(sent.at(i).at(0)) == cmd) return i;
        return -1;
    }

    QString description() const override { return QStringLiteral("fake node"); }
    void open() override {
        open_ = true;
        queue([this] { Q_EMIT opened(); });
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    void send(const QByteArray& payload) override {
        sent.append(payload);
        const quint8 cmd = quint8(payload.at(0));
        if (silentOn && *silentOn == cmd) return;
        proto::Reader r(payload);
        r.skip(1);

        switch (cmd) {
            case proto::CmdDeviceQuery: {
                // fw_ver claims 3, as coreletd's does, and the repeat byte and
                // path-hash byte follow anyway.
                proto::Writer w(proto::RespDeviceInfo);
                w.u8(3).u8(50).u8(8).u32(0);
                w.padded(QByteArrayLiteral("29 Oct 2026"), 12);
                w.padded(QByteArrayLiteral("fake"), 40);
                w.padded(QByteArrayLiteral("v0"), 20);
                w.u8(repeat ? 1 : 0).u8(0);
                reply(w.bytes());
                return;
            }
            case proto::CmdAppStart: {
                proto::Writer w(proto::RespSelfInfo);
                w.u8(1)
                    .u8(quint8(txPower))
                    .u8(maxTxPower)
                    .padded(QByteArray(32, 'k'), 32)
                    .u32(quint32(latE6))
                    .u32(quint32(lonE6))
                    .u32(0)
                    .u32(freqKhz)
                    .u32(bwHz)
                    .u8(sf)
                    .u8(cr)
                    .tail(name);
                reply(w.bytes());
                return;
            }
            case proto::CmdGetTuningParams:
                reply(proto::Writer(proto::RespTuningParams).u32(0).u32(airtimeMilli).bytes());
                return;
            case proto::CmdGetChannel:
                reply(proto::Writer(proto::RespErr).u8(proto::ErrNotFound).bytes());
                return;
            case proto::CmdGetContacts:
                reply(proto::Writer(proto::RespContactsStart).u32(0).bytes());
                reply(proto::Writer(proto::RespEndOfContacts).bytes());
                return;
            case proto::CmdGetBatteryVoltage:
                reply(proto::Writer(proto::RespBatteryVoltage).u16(3900).bytes());
                return;
            case proto::CmdSetAdvertName:
            case proto::CmdSetAdvertLatLon:
            case proto::CmdSetRadioParams:
            case proto::CmdSetRadioTxPower:
            case proto::CmdSetTuningParams:
                if (disabled) {
                    reply(proto::Writer(proto::RespDisabled).bytes());
                    return;
                }
                apply(cmd, r);
                reply(proto::Writer(proto::RespOk).bytes());
                return;
            default:
                reply(proto::Writer(proto::RespOk).bytes());
                return;
        }
    }

private:
    void apply(quint8 cmd, proto::Reader& r) {
        switch (cmd) {
            case proto::CmdSetAdvertName:
                name = r.rest();
                break;
            case proto::CmdSetAdvertLatLon:
                latE6 = r.i32();
                lonE6 = r.i32();
                break;
            case proto::CmdSetRadioParams:
                freqKhz = r.u32();
                // Snapped to the radio's own, as the daemon snaps 7800 to 7.81.
                bwHz = r.u32();
                if (bwHz == 7800) bwHz = 7810;
                sf = r.u8();
                cr = r.u8();
                // Missing means off, upstream's rule and coreletd's.
                repeat = r.remaining() > 0 && r.u8() != 0;
                break;
            case proto::CmdSetRadioTxPower:
                if (!ignoreTxPower) txPower = r.i8();
                break;
            case proto::CmdSetTuningParams:
                r.skip(4);
                airtimeMilli = r.u32();
                break;
        }
    }

    void queue(std::function<void()> action) {
        QTimer::singleShot(0, this, [action = std::move(action)] { action(); });
    }
    void reply(const QByteArray& payload) {
        queue([this, payload] { Q_EMIT frameReceived(payload); });
    }

    bool open_ = false;
};

bool spin(const std::function<bool()>& done, int limitMs = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < limitMs)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    return done();
}

void settle(int ms = 200) {
    spin([] { return false; }, ms);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    auto* node = new FakeNode;  // adopted by the client
    Client client;

    QVector<Client::SettingsResult> results;
    QStringList order;  // which of the two signals came first, per save
    QObject::connect(&client, &Client::deviceInfoChanged,
                     [&](const Client::DeviceInfo&) { order.append(QStringLiteral("info")); });
    QObject::connect(&client, &Client::settingsApplied, [&](const Client::SettingsResult& result) {
        order.append(QStringLiteral("result"));
        results.append(result);
    });

    client.start(node);
    // Through the first battery reading too, which follows Ready and is
    // announced as a device change of its own.
    if (!check(spin([&] { return client.state() == Client::State::Ready; }),
               "the handshake completes") ||
        !check(spin([&] { return client.device().batteryMillivolts > 0; }),
               "the first battery reading arrives"))
        return 1;

    // --- what the handshake learns ------------------------------------------
    // Upstream firmware frames received messages in the V3 shape only for an
    // app that has said it understands it, so the query comes first.
    if (!check(node->indexOf(proto::CmdDeviceQuery) == 0, "DEVICE_QUERY opens the handshake") ||
        !check(node->sent.at(0).size() == 2 && quint8(node->sent.at(0).at(1)) == 3,
               "and says the app understands protocol 3") ||
        !check(client.device().repeat == true, "the repeat byte past the fixed fields is read") ||
        !check(client.device().maxTxPowerDbm == 22, "the radio's ceiling is read") ||
        !check(client.device().airtimeFactorMilli == 9000, "the airtime factor is read") ||
        !check(!client.device().settingsDisabled, "nothing is assumed disabled before trying"))
        return 1;

    // --- a preset change carries repeat as it stands ------------------------
    Client::SettingsChange preset;
    preset.radio = Client::SettingsChange::Radio {869.495, 62.5, 7, 5, true};
    int sentBefore = node->sent.size();
    const int selfInfoBefore = node->count(proto::CmdAppStart);
    order.clear();
    client.applySettings(preset);
    if (!check(spin([&] { return results.size() == 1; }), "a radio change is answered"))
        return 1;
    const int radioAt = node->indexOf(proto::CmdSetRadioParams, sentBefore);
    if (!check(radioAt >= 0, "SET_RADIO_PARAMS is sent") ||
        !check(node->sent.at(radioAt).size() == 12, "with the trailing repeat byte") ||
        !check(quint8(node->sent.at(radioAt).at(11)) == 1, "carrying repeat on") ||
        !check(node->repeat, "so the node goes on repeating") ||
        !check(node->count(proto::CmdAppStart) == selfInfoBefore + 1,
               "the node is read back afterwards") ||
        !check(order == QStringList({QStringLiteral("info"), QStringLiteral("result")}),
               "and the re-read is announced before the result") ||
        !check(results.last().refused.isEmpty(), "nothing is refused") ||
        !check(results.last().applied.contains(Setting::Radio), "the radio change applied") ||
        !check(client.device().sf == 7 && client.device().cr == 5,
               "and the device shows what the node now reports"))
        return 1;

    // Off has to be said as plainly as on: the byte is there either way.
    Client::SettingsChange off = preset;
    off.radio->repeat = false;
    sentBefore = node->sent.size();
    client.applySettings(off);
    if (!check(spin([&] { return results.size() == 2; }), "turning repeat off is answered") ||
        !check(quint8(node->sent.at(node->indexOf(proto::CmdSetRadioParams, sentBefore)).at(11)) ==
                   0,
               "with the byte at zero") ||
        !check(client.device().repeat == false, "and the re-read says it is off"))
        return 1;

    // --- refused here rather than sent --------------------------------------
    Client::SettingsChange elsewhere;
    elsewhere.radio = Client::SettingsChange::Radio {869.618, 62.5, 8, 8, true};
    sentBefore = node->sent.size();
    client.applySettings(elsewhere);
    if (!check(spin([&] { return results.size() == 3; }), "repeat off the list is answered") ||
        !check(results.last().refused.contains(Setting::Radio), "and refused") ||
        !check(node->indexOf(proto::CmdSetRadioParams, sentBefore) < 0,
               "without asking a node that would refuse the frequency with it"))
        return 1;

    // A bandwidth goes as whole hertz and comes back as the radio's own.
    Client::SettingsChange narrow;
    narrow.radio = Client::SettingsChange::Radio {869.618, 7.8, 8, 8, false};
    client.applySettings(narrow);
    if (!check(spin([&] { return results.size() == 4; }), "a narrow bandwidth is answered") ||
        !check(client.device().bwKhz == 7.81, "the node reports its own rounding") ||
        !check(results.last().refused.isEmpty(), "which is the bandwidth that was asked for"))
        return 1;

    // --- the name and the position share the advert -------------------------
    Client::SettingsChange longName;
    longName.name = QString(24, QLatin1Char('n'));
    longName.position = Client::SettingsChange::Position {true, 51.5, -0.12};
    sentBefore = node->sent.size();
    client.applySettings(longName);
    if (!check(spin([&] { return results.size() == 5; }), "a name and a position are answered") ||
        !check(results.last().refused.contains(Setting::Name),
               "24 bytes of name beside a position is refused") ||
        !check(node->indexOf(proto::CmdSetAdvertName, sentBefore) < 0, "and never sent") ||
        !check(results.last().applied.contains(Setting::Position), "the position still goes") ||
        !check(client.device().hasLocation, "and the node now advertises one"))
        return 1;

    // Taking the position away is what makes room for the longer name, so it
    // has to reach the node first.
    Client::SettingsChange roomier;
    roomier.name = QString(31, QLatin1Char('n'));
    roomier.position = Client::SettingsChange::Position {};
    sentBefore = node->sent.size();
    client.applySettings(roomier);
    if (!check(spin([&] { return results.size() == 6; }), "a longer name is answered"))
        return 1;
    const int clearAt = node->indexOf(proto::CmdSetAdvertLatLon, sentBefore);
    const int nameAt = node->indexOf(proto::CmdSetAdvertName, sentBefore);
    if (!check(clearAt >= 0 && nameAt >= 0, "both are sent") ||
        !check(clearAt < nameAt, "the position goes before the name it makes room for") ||
        !check(node->sent.at(clearAt).mid(1) == QByteArray(8, '\0'), "as 0, 0") ||
        !check(results.last().refused.isEmpty(), "and both take") ||
        !check(client.device().name == QString(31, QLatin1Char('n')), "the name is read back"))
        return 1;

    // --- the airtime factor keeps the RX delay it read ----------------------
    Client::SettingsChange airtime;
    airtime.airtimeFactorMilli = 3000;
    sentBefore = node->sent.size();
    client.applySettings(airtime);
    if (!check(spin([&] { return results.size() == 7; }), "an airtime change is answered"))
        return 1;
    const QByteArray tuning = node->sent.at(node->indexOf(proto::CmdSetTuningParams, sentBefore));
    proto::Reader tuningReader(tuning);
    tuningReader.skip(1);
    if (!check(tuningReader.u32() == 0, "the RX delay goes back as it was read") ||
        !check(tuningReader.u32() == 3000, "beside the new factor") ||
        !check(client.device().airtimeFactorMilli == 3000, "which the node reports"))
        return 1;

    // --- an OK is not believed over the re-read -----------------------------
    node->ignoreTxPower = true;
    Client::SettingsChange power;
    power.txPowerDbm = 10;
    client.applySettings(power);
    if (!check(spin([&] { return results.size() == 8; }), "a power change is answered") ||
        !check(results.last().refused.contains(Setting::TxPower),
               "a power the node accepted and does not report is not applied") ||
        !check(!results.last().applied.contains(Setting::TxPower), "and not counted as one"))
        return 1;
    node->ignoreTxPower = false;

    Client::SettingsChange tooLoud;
    tooLoud.txPowerDbm = 30;
    client.applySettings(tooLoud);
    if (!check(spin([&] { return results.size() == 9; }), "an impossible power is answered") ||
        !check(results.last().refused.contains(Setting::TxPower),
               "and refused against the ceiling the node reported"))
        return 1;

    // --- a node that takes no settings --------------------------------------
    node->disabled = true;
    Client::SettingsChange rename;
    rename.name = QStringLiteral("renamed");
    client.applySettings(rename);
    if (!check(spin([&] { return results.size() == 10; }), "a disabled setter is answered") ||
        !check(results.last().disabled, "RESP_DISABLED says so") ||
        !check(results.last().refused.isEmpty(), "rather than as a failure") ||
        !check(client.device().settingsDisabled, "and the node is remembered as read-only") ||
        !check(client.device().name == QString(31, QLatin1Char('n')),
               "showing the name it still has"))
        return 1;
    node->disabled = false;

    // --- a link that dies partway answers once ------------------------------
    node->silentOn = proto::CmdSetRadioTxPower;
    Client::SettingsChange stranded;
    stranded.name = QStringLiteral("stranded");
    stranded.txPowerDbm = 5;
    client.applySettings(stranded);
    if (!check(spin([&] { return node->count(proto::CmdSetRadioTxPower) == 2; }),
               "the power change reaches the node"))
        return 1;
    client.stop();
    settle();
    if (!check(results.size() == 11, "exactly one answer arrives") ||
        !check(results.last().refused.value(Setting::TxPower) == QStringLiteral("connection lost"),
               "the unanswered change is reported lost") ||
        !check(!results.last().refused.contains(Setting::Name),
               "the one the node answered is not") ||
        !check(results.last().applied.isEmpty(), "and nothing unverified counts as applied"))
        return 1;

    client.applySettings(rename);
    if (!check(results.size() == 12 && results.last().refused.contains(Setting::Name),
               "with no link a change is refused at once"))
        return 1;

    return 0;
}
