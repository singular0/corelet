#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include "model/types.h"

namespace model {

// Persistent message log.
//
// CMD_SYNC_NEXT_MESSAGE *pops* from the daemon's inbox, so a message the app
// has collected exists nowhere else. Without this the chat pane would be empty
// on every launch and every message read once would be gone forever, so the
// app owns its own history rather than treating the daemon as storage.
//
// The format is JSON Lines: appending one message is one short write with no
// rewrite of what came before, and a truncated final line (power loss on a
// handheld) costs exactly one message instead of the whole file.
class History {
public:
    // Kept in memory and on disk per channel. A uConsole has limited RAM and
    // nobody scrolls back further than this in a mesh chat.
    static constexpr int MaxPerChannel = 500;

    explicit History(QString path);

    // Reads the log, discarding malformed lines rather than refusing to start:
    // a corrupt history is an annoyance, not a reason to lose the app.
    void load();

    // Returns a copy with the channel's current wire slot applied. Slots are
    // deliberately absent from storage and may change between connections.
    QVector<Message> messages(const QByteArray& deviceId,
                              const QByteArray& channelKeyFingerprint,
                              int channelIndex) const;
    void append(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                const Message& msg);
    // Forgets a channel's messages, on disk as well as in memory. Device
    // identity and the channel-key fingerprint are the complete scope;
    // neither a device switch nor a reused slot can inherit these messages.
    void remove(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint);

private:
    void appendLine(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                    const Message& msg);
    // Rewrites the file from the trimmed in-memory state.
    void compact();

    QString path_;
    // An empty inner key is reserved for direct messages, which belong to the
    // device but not to one of its channels.
    QHash<QByteArray, QHash<QByteArray, QVector<Message>>> byDevice_;
    // How many lines the file holds, so trimming happens on a whole-file
    // rewrite rather than per append.
    int linesOnDisk_ = 0;
};

}  // namespace model
