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

// Device-scoped persistent message storage. Each public key gets its own
// SQLite database, so opening one node never reads or rewrites another node's
// history and channel lookups remain indexed as the collection grows.
class History {
public:
    // Kept on disk and loaded into the chat view per channel. A uConsole has
    // limited RAM and nobody scrolls back further than this in a mesh chat.
    static constexpr int MaxPerChannel = 500;

    explicit History(QString directory);
    ~History();

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Opens and initializes the device's database without writing a message to
    // it. SYNC_NEXT_MESSAGE destroys the daemon's copy of whatever it hands
    // back, so the app has to know it has somewhere to put a message before it
    // asks for the first one.
    HistoryResult preflight(const QByteArray& deviceId);

    // Returns messages with the channel's current wire slot applied. Slots are
    // deliberately absent from storage and may change between connections.
    HistoryMessages messages(const QByteArray& deviceId,
                             const QByteArray& channelKeyFingerprint,
                             int channelIndex) const;
    HistoryLatest latestMessage(const QByteArray& deviceId,
                                const QByteArray& channelKeyFingerprint,
                                int channelIndex) const;
    HistoryResult append(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                         const Message& msg);
    // Forgets one channel in one device database. Device identity and the
    // channel-key fingerprint are the complete scope; a reused slot cannot
    // inherit these messages.
    HistoryResult remove(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint);

    // Scope for a message the app cannot place. Collection has already taken it
    // off the node, so an unknown device or a channel whose key is not in hand
    // must not be a reason to drop one.
    //
    // Neither value can collide with the real thing: a device is a 32-byte
    // public key and a conversation is the SHA-256 of a channel key, and no key
    // anyone holds hashes to nearly all zeros. `orphanChannel` keeps the wire
    // slot the message arrived on in its last byte, which is the only clue left
    // as to where it belongs if the key turns up later.
    static QByteArray orphanDeviceId();
    static QByteArray orphanChannel(int wireSlot);

private:
    static constexpr int DeviceIdSize = 32;
    static constexpr int ChannelFingerprintSize = 32;

    static bool validScope(const QByteArray& deviceId,
                           const QByteArray& channelKeyFingerprint, int channelIndex);
    // Sets `*error` on every path that returns an unusable database, so no
    // caller has to invent a reason for a failure it did not see.
    QSqlDatabase databaseFor(const QByteArray& deviceId, QString* error) const;

    QString directory_;
    // Connections are lazy: devices untouched in this process cost no file
    // descriptors, queries, or memory.
    mutable QHash<QByteArray, QString> connectionNames_;
};

}  // namespace model
