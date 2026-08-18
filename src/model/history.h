#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>
#include <utility>

#include "model/types.h"

class QSqlDatabase;

namespace model {

// The outcome of one history operation. Storage failure is never cosmetic here:
// collecting a message pops it out of the daemon's inbox, so an append that did
// not happen is a message that exists nowhere at all. Every operation therefore
// says whether it worked, and why it did not in words fit to put on screen.
struct HistoryResult {
    bool ok = false;
    QString error;

    explicit operator bool() const { return ok; }

    static HistoryResult success() { return {true, {}}; }
    static HistoryResult failure(QString error) { return {false, std::move(error)}; }
};

// A read together with its outcome. An empty conversation and an unreadable
// database both come back with no rows, and only the result tells them apart --
// which matters, because showing one as the other is how a full history silently
// looks like a fresh install.
struct HistoryMessages {
    HistoryResult result;
    QVector<Message> messages;
};

struct HistoryLatest {
    HistoryResult result;
    std::optional<Message> message;
};

struct HistoryConversations {
    HistoryResult result;
    QVector<Conversation> conversations;
};

// Device-scoped persistent message storage. Each public key gets its own
// SQLite database, so opening one node never reads or rewrites another node's
// history and conversation lookups remain indexed as the collection grows.
class History {
public:
    // Kept on disk and loaded into the chat view per conversation. A uConsole
    // has limited RAM and nobody scrolls back further than this in a mesh chat.
    static constexpr int MaxPerConversation = 500;

    // What this binary writes and knows how to read. Every database carries its
    // own in `PRAGMA user_version`: an older one is stepped up to this on open,
    // and a newer one is refused rather than guessed at, because the only thing
    // a database from a future build can be is somebody's message history.
    static constexpr int SchemaVersion = 2;

    explicit History(QString directory);
    ~History();

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Opens and initializes the device's database without writing a message to
    // it. SYNC_NEXT_MESSAGE destroys the daemon's copy of whatever it hands
    // back, so the app has to know it has somewhere to put a message before it
    // asks for the first one.
    HistoryResult preflight(const QByteArray& deviceId);

    // Returns messages with `channelIndex` applied as the conversation's
    // current wire slot. Slots are deliberately absent from storage and may
    // change between connections; a direct conversation has none at all.
    HistoryMessages messages(const QByteArray& deviceId, const Conversation& conversation,
                             int channelIndex = -1) const;
    HistoryLatest latestMessage(const QByteArray& deviceId, const Conversation& conversation,
                                int channelIndex = -1) const;
    HistoryResult append(const QByteArray& deviceId, const Conversation& conversation,
                         const Message& msg);
    // Forgets one conversation in one device database. Device identity and the
    // conversation are the complete scope; a reused channel slot cannot inherit
    // another channel's messages.
    HistoryResult remove(const QByteArray& deviceId, const Conversation& conversation);

    // Every peer this device has direct messages with, including those still
    // known only by the six-byte prefix the wire gave. This is where the
    // sidebar's direct conversations come from: unlike channels, nothing on the
    // node enumerates them -- a conversation exists because something was said.
    HistoryConversations directConversations(const QByteArray& deviceId) const;

    // Folds messages stored under a peer's six-byte prefix into the
    // conversation with that whole key. Collection can hand over a direct
    // message before the address book has the peer in it, and the prefix is all
    // the wire carries; this is what those messages join when the peer turns up.
    HistoryResult resolvePeer(const QByteArray& deviceId, const QByteArray& peerKey);

    // Scope for a message the app cannot place. Collection has already taken it
    // off the node, so an unknown device or a channel whose key is not in hand
    // must not be a reason to drop one.
    //
    // Neither value can collide with the real thing: a device is a 32-byte
    // public key and a channel conversation is the SHA-256 of a channel key, and
    // no key anyone holds hashes to nearly all zeros. `orphanChannel` keeps the
    // wire slot the message arrived on in its last byte, which is the only clue
    // left as to where it belongs if the key turns up later. A direct message
    // needs none of this: its peer prefix is already an identity to file it
    // under, however little of one.
    static QByteArray orphanDeviceId();
    static Conversation orphanChannel(int wireSlot);

private:
    static constexpr int DeviceIdSize = 32;

    // Sets `*error` on every path that returns an unusable database, so no
    // caller has to invent a reason for a failure it did not see.
    QSqlDatabase databaseFor(const QByteArray& deviceId, QString* error) const;
    // Brings a just-opened database up to SchemaVersion. Returns an empty
    // string on success and the reason otherwise; the caller closes and
    // discards the connection on a failure, since a half-migrated database is
    // not one to keep writing messages into.
    static QString migrate(QSqlDatabase& db);
    // The one 1 -> 2 step, in C++ rather than SQL because the direct messages
    // of a version 1 database carry their peer as hex in the sender column and
    // SQLite has no portable way to decode it.
    static QString upgradeToConversations(QSqlDatabase& db);

    QString directory_;
    // Connections are lazy: devices untouched in this process cost no file
    // descriptors, queries, or memory.
    mutable QHash<QByteArray, QString> connectionNames_;
};

}  // namespace model
