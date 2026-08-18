#pragma once

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <functional>

#include "model/types.h"
#include "protocol/frame_codec.h"
#include "protocol/text_limits.h"
#include "protocol/transport.h"

class QTimer;

namespace proto {

// Talks the MeshCore companion protocol over whatever link it is handed: a
// companion daemon on its Unix socket or on TCP, or a device on BLE.
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
        double latitude = 0;
        double longitude = 0;
        // SELF_INFO uses 0,0 to mean that the node does not advertise a
        // location, so coordinates need a presence bit of their own.
        bool hasLocation = false;
        double freqMhz = 0;
        double bwKhz = 0;
        int sf = 0;
        int cr = 0;
        int txPowerDbm = 0;
        // GET_BATTERY_VOLTAGE supplies millivolts rather than charge state. The
        // client retains both the raw reading and the percentage the UI needs;
        // -1 means the far end has no battery reading.
        int batteryMillivolts = -1;
        int batteryPercent = -1;
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
    bool isRunning() const { return running_; }
    const DeviceInfo& device() const { return device_; }
    const QVector<model::Channel>& channels() const { return channels_; }
    const QVector<model::Contact>& contacts() const { return contacts_; }
    // The peer a direct conversation is with, or null while the address book
    // has nothing that matches -- which is what a conversation known only by
    // the six-byte prefix the wire gave looks like until the list catches up.
    // Valid only until contacts() next changes, so read what is wanted out of
    // it rather than keeping it.
    const model::Contact* contactFor(const model::Conversation& conversation) const;

    // `token` is handed straight back in sendResult(). Nothing in the protocol
    // identifies which send an answer belongs to, so the caller's own tag is
    // what lets it find the message it put on screen. Text longer than
    // maxMessageBytes(device().name) is refused through sendResult() rather
    // than truncated: what a caller is handed back is what it typed.
    void sendChannelMessage(int channelIndex, const QString& text, int token);

    // Writes a channel into a slot, creating it or replacing what was there.
    // The far end owns the keys, so this is the only way the app can add one.
    // A name over MaxChannelNameBytes is refused through channelSaveResult().
    void setChannel(int channelIndex, const QString& name, const QByteArray& secret);

    // Empties a slot. There is no delete command: a slot with an all-zero key is
    // what "unused" means on the wire, so removing a channel is a SET_CHANNEL
    // like any other. The key is gone from the device afterwards -- a private
    // channel is unrecoverable without a copy of it.
    void clearChannel(int channelIndex);

    // Gates inbox collection on the app having somewhere to put what it
    // collects. SYNC_NEXT_MESSAGE pops the message out of the daemon's inbox,
    // so a drain that runs ahead of storage destroys messages; with this off,
    // the backlog simply stays where it is until it can be written down.
    //
    // The owner must preflight its storage and set this before the handshake
    // finishes, and turn it off from inside messageReceived/directMessageReceived
    // when a write fails -- those are delivered directly, so the answer is in
    // hand before the next message is asked for. Turning it back on while the
    // link is up resumes the drain.
    void setStorageAvailable(bool available);

Q_SIGNALS:
    void stateChanged(State state, const QString& detail);
    void deviceInfoChanged(const DeviceInfo& info);
    void channelsChanged(const QVector<model::Channel>& channels);
    // The whole address book, as the device enumerated it. Emitted once per
    // handshake, and with an empty list if the far end has nothing to say.
    void contactsChanged(const QVector<model::Contact>& contacts);
    // One contact an advert or a path update refreshed, already merged into
    // contacts(). Separate from the list signal because adverts arrive all day:
    // a view can repaint the one row instead of rebuilding a list somebody is
    // reading down.
    void contactChanged(const model::Contact& contact);
    // Covers a complete inbox drain, not each individual SYNC_NEXT_MESSAGE in
    // it. A backlog can take long enough to be worth exposing as link activity.
    void messageSyncChanged(bool syncing);
    void messageReceived(const model::Message& msg);
    // A direct message the daemon handed us, already placed in the conversation
    // with its peer -- by the peer's whole key when the address book has it,
    // and by the six-byte prefix the wire carries when it does not.
    void directMessageReceived(const model::Message& msg);
    // Exactly one of these arrives per sendChannelMessage(), including when the
    // link dies with the command still queued.
    void sendResult(int token, bool ok, const QString& error);
    // Answer to setChannel(). Emitted after channelsChanged() when it succeeds,
    // so a listener can open the slot it just created.
    void channelSaveResult(int channelIndex, bool ok, const QString& error);
    // Answer to clearChannel(), likewise emitted after channelsChanged() so a
    // listener sees the slot already gone from the list.
    void channelRemoveResult(int channelIndex, bool ok, const QString& error);

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
    void requestBattery();
    void requestChannel(int index);
    // The contact stream: CONTACTS_START, a CONTACT per node, END_OF_CONTACTS.
    // Multi-frame, so its handler returns false until the terminator arrives.
    // Asked for once during the handshake, and again whenever a direct message
    // arrives from a key this end cannot name -- the daemon matched that peer,
    // so a list that does not have it is a stale copy. At most one stream is
    // ever in flight: a backlog of messages from one unknown peer must not
    // queue a re-enumeration each.
    void requestContacts();
    // Re-reads one contact after a push said something about it changed. A push
    // carries the key and nothing else, so what changed has to be asked for.
    void requestContact(const QByteArray& pubkey);
    void upsertContact(const model::Contact& contact);
    static model::Contact parseContact(Reader& r);
    // The conversation a channel slot names, or an invalid one when nothing
    // configured is in that slot -- a message for which is a message the app
    // cannot place, not one to drop.
    model::Conversation channelConversation(int slot) const;
    // The conversation a direct message's six-byte peer prefix names: the
    // peer's whole key once the address book has it, and the prefix itself
    // until then, which is still enough to file the message under and to
    // address a reply with.
    model::Conversation directConversation(const QByteArray& prefix) const;
    // Re-reads one slot after writing it and reports the write's outcome.
    // `cleared` says which write it was: the slot is expected to be occupied
    // afterwards, or empty, and the answer goes to the matching signal.
    void readBackChannel(int index, bool cleared);
    void requestSync();
    void setMessageSyncing(bool syncing);

    void setState(State s, const QString& detail = {});
    void resetConnection();

    // Shared tail parsing for both V3 message shapes.
    model::Message parseMessageTail(Reader& r, qint8 snrQ4, int channelIndex);

    Transport* transport_ = nullptr;
    QTimer* replyTimer_ = nullptr;
    QTimer* reconnectTimer_ = nullptr;
    QTimer* batteryTimer_ = nullptr;

    bool running_ = false;
    int backoffMs_ = 0;

    QQueue<Pending> queue_;
    bool inFlight_ = false;
    // Guards against stacking a sync per push: one drain loop is enough, and
    // the daemon pushes MSG_WAITING once per stored message.
    bool syncPending_ = false;
    bool messageSyncing_ = false;
    // Off until the owner says otherwise, and off again on every reconnect, so
    // a session that never preflights its storage never pops a message.
    bool storageAvailable_ = false;

    State state_ = State::Disconnected;
    DeviceInfo device_;
    QVector<model::Channel> channels_;
    int channelsOutstanding_ = 0;
    QVector<model::Contact> contacts_;
    // Collected across the frames of one contact stream, and only swapped into
    // contacts_ once the device says that is all of them: a half-read list must
    // never be shown as the address book.
    QVector<model::Contact> incomingContacts_;
    // Whether a contact stream is already queued or running. See requestContacts().
    bool contactsOutstanding_ = false;
    // Keys with a fetch already queued. A busy mesh repeats one node's advert by
    // several routes, and an advert that also moved the path pushes twice, so
    // without this the queue fills with re-reads of the same contact.
    QSet<QByteArray> contactFetches_;
};

}  // namespace proto
