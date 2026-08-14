#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>

#include "model/types.h"

class QSqlDatabase;

namespace model {

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

    // Returns messages with the channel's current wire slot applied. Slots are
    // deliberately absent from storage and may change between connections.
    QVector<Message> messages(const QByteArray& deviceId,
                              const QByteArray& channelKeyFingerprint,
                              int channelIndex) const;
    std::optional<Message> latestMessage(const QByteArray& deviceId,
                                         const QByteArray& channelKeyFingerprint,
                                         int channelIndex) const;
    void append(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                const Message& msg);
    // Forgets one channel in one device database. Device identity and the
    // channel-key fingerprint are the complete scope; a reused slot cannot
    // inherit these messages.
    void remove(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint);

private:
    static constexpr int DeviceIdSize = 32;
    static constexpr int ChannelFingerprintSize = 32;

    static bool validScope(const QByteArray& deviceId,
                           const QByteArray& channelKeyFingerprint, int channelIndex);
    QSqlDatabase databaseFor(const QByteArray& deviceId) const;

    QString directory_;
    // Connections are lazy: devices untouched in this process cost no file
    // descriptors, queries, or memory.
    mutable QHash<QByteArray, QString> connectionNames_;
};

}  // namespace model
