#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QVector>

#include <functional>

#include "model/types.h"
#include "protocol/client.h"
#include "protocol/frame_codec.h"
#include "protocol/protocol.h"
#include "protocol/transport.h"

namespace {

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

// A companion daemon inside the test process. It answers the handshake, because
// nothing below Ready will send a message, and then answers a send with whatever
// the case at hand set up. Replies go out through the event loop rather than
// from inside send(): no real link answers a command from within the call that
// wrote it, and the queue must not be re-entered that way.
class FakeDaemon : public proto::Transport {
public:
    // What RESP_SENT will claim the peer must answer with, and how long it
    // suggests waiting for that answer.
    QByteArray ack = QByteArray::fromHex("11223344");
    quint32 windowMs = 60000;

    QString description() const override { return QStringLiteral("fake daemon"); }
    void open() override {
        open_ = true;
        queue([this] { Q_EMIT opened(); });
    }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    void send(const QByteArray& payload) override {
        switch (quint8(payload.at(0))) {
            case proto::CmdAppStart: {
                // adv_type(1) tx(1) max_tx(1) pubkey(32) lat(4) lon(4) flags(4)
                // freq(4) bandwidth(4) sf(1) cr(1) name
                proto::Writer w(proto::RespSelfInfo);
                w.u8(1)
                    .u8(20)
                    .u8(30)
                    .padded(QByteArray(32, 'k'), 32)
                    .u32(0)
                    .u32(0)
                    .u32(0)
                    .u32(869525)
                    .u32(250)
                    .u8(11)
                    .u8(5)
                    .tail(QByteArrayLiteral("fake-node"));
                reply(w.bytes());
                return;
            }
            case proto::CmdGetChannel:
                // Every slot empty. What a send does is not a question about the
                // sidebar, and the client enumerates all eight either way.
                reply(proto::Writer(proto::RespErr).u8(proto::ErrNotFound).bytes());
                return;
            case proto::CmdGetContacts:
                reply(proto::Writer(proto::RespContactsStart).u32(0).bytes());
                reply(proto::Writer(proto::RespEndOfContacts).bytes());
                return;
            case proto::CmdGetBatteryVoltage:
                reply(proto::Writer(proto::RespBatteryVoltage).u16(3900).bytes());
                return;
            case proto::CmdSendTxtMsg: {
                // route(1) expected_ack(4) suggested_timeout(4)
                proto::Writer w(proto::RespSent);
                w.u8(0).padded(ack, proto::AckHashSize).u32(windowMs);
                reply(w.bytes());
                return;
            }
            case proto::CmdSendChannelTxtMsg:
                // Plain OK: a channel message has no addressee to acknowledge it.
                reply(proto::Writer(proto::RespOk).bytes());
                return;
            default:
                reply(proto::Writer(proto::RespOk).bytes());
                return;
        }
    }

    // An ack coming back off the air, which the daemon pushes unsolicited.
    void pushConfirmed(const QByteArray& confirmedAck) {
        proto::Writer w(proto::PushSendConfirmed);
        w.padded(confirmedAck, proto::AckHashSize).u32(1234);  // round trip, unread
        reply(w.bytes());
    }

private:
    void queue(std::function<void()> action) {
        QTimer::singleShot(0, this, [action = std::move(action)] { action(); });
    }
    void reply(const QByteArray& payload) {
        queue([this, payload] { Q_EMIT frameReceived(payload); });
    }

    bool open_ = false;
};

// Runs the event loop until `done` or the deadline, which is what makes a timer
// the client armed observable from a plain main(). The deadlines are generous
// because they are only there to keep a broken build from hanging: a package
// build under qemu emulation is the slowest thing that runs this.
bool spin(const std::function<bool()>& done, int limitMs = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < limitMs)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    return done();
}

// For the checks that something does *not* happen: give it every chance to.
void settle(int ms = 200) {
    spin([] { return false; }, ms);
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    auto* daemon = new FakeDaemon;  // adopted by the client
    proto::CompanionClient client;

    QVector<QPair<int, bool>> results;
    QVector<int> confirmed;
    QVector<int> unconfirmed;
    QObject::connect(&client, &proto::CompanionClient::sendResult,
                     [&](int token, bool ok, const QString&) {
                         results.append(qMakePair(token, ok));
                     });
    QObject::connect(&client, &proto::CompanionClient::sendConfirmed,
                     [&](int token) { confirmed.append(token); });
    QObject::connect(&client, &proto::CompanionClient::sendUnconfirmed,
                     [&](int token) { unconfirmed.append(token); });

    client.start(daemon);
    if (!check(spin([&] { return client.state() == proto::CompanionClient::State::Ready; }),
               "the handshake completes"))
        return 1;

    // Six bytes is all the wire carries of a peer, and all a send needs.
    const model::Conversation peer =
        model::Conversation::direct(QByteArray::fromHex("010203040506"));

    // A direct send the peer answers for.
    daemon->ack = QByteArray::fromHex("11223344");
    client.sendDirectMessage(peer, QStringLiteral("hello"), 1);
    if (!check(spin([&] { return !results.isEmpty(); }), "a direct send is answered") ||
        !check(results.at(0) == qMakePair(1, true), "RESP_SENT reports the send as taken"))
        return 1;

    daemon->pushConfirmed(QByteArray::fromHex("deadbeef"));
    settle();
    if (!check(confirmed.isEmpty(), "an ack for nobody's send confirms nothing"))
        return 1;

    daemon->pushConfirmed(QByteArray::fromHex("11223344"));
    if (!check(spin([&] { return !confirmed.isEmpty(); }), "the peer's ack confirms the send") ||
        !check(confirmed.at(0) == 1, "the confirmation carries the sender's own token"))
        return 1;

    // The firmware says outright that the same ack can arrive more than once.
    daemon->pushConfirmed(QByteArray::fromHex("11223344"));
    settle();
    if (!check(confirmed.size() == 1, "a repeated ack confirms the send only once"))
        return 1;

    // A send nothing answers inside the window the node suggested -- and then
    // answers afterwards, as a retry landing late does.
    daemon->ack = QByteArray::fromHex("55667788");
    daemon->windowMs = 1;  // clamped up to the client's floor
    client.sendDirectMessage(peer, QStringLiteral("anyone there"), 2);
    if (!check(spin([&] { return results.size() == 2; }), "the second send is answered") ||
        !check(unconfirmed.isEmpty(), "a send inside its window is not reported unconfirmed") ||
        !check(spin([&] { return !unconfirmed.isEmpty(); }, 8000),
               "a window that passes reports the send unconfirmed") ||
        !check(unconfirmed.at(0) == 2, "the report carries the sender's own token"))
        return 1;

    daemon->pushConfirmed(QByteArray::fromHex("55667788"));
    if (!check(spin([&] { return confirmed.size() == 2; }),
               "an ack after the window still confirms the send") ||
        !check(confirmed.at(1) == 2, "the late confirmation is the send that lapsed"))
        return 1;

    // A channel message has nobody to acknowledge it, so nothing waits on one.
    client.sendChannelMessage(0, QStringLiteral("hello everyone"), 3);
    if (!check(spin([&] { return results.size() == 3; }), "a channel send is answered") ||
        !check(results.at(2) == qMakePair(3, true), "RESP_OK reports the channel send as taken"))
        return 1;

    // A send still waiting when the link goes: a push only reaches a live
    // session, so that ack is never going to arrive.
    daemon->ack = QByteArray::fromHex("99aabbcc");
    daemon->windowMs = 60000;
    client.sendDirectMessage(peer, QStringLiteral("still here"), 4);
    if (!check(spin([&] { return results.size() == 4; }), "the last send is answered"))
        return 1;

    client.stop();
    settle();
    if (!check(unconfirmed.contains(4), "a link that goes down gives up on an outstanding ack") ||
        !check(!unconfirmed.contains(1), "a confirmed send is not reported unconfirmed later") ||
        !check(!unconfirmed.contains(3), "a channel send never waits for an ack"))
        return 1;

    return 0;
}
