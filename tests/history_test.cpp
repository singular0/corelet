#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "model/history.h"

namespace {

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

QString databasePath(const QTemporaryDir& directory, const QByteArray& deviceId) {
    return QDir(directory.path())
        .filePath(QString::fromLatin1(deviceId.toHex()) + QStringLiteral(".sqlite3"));
}

// Runs statements straight against a device database, which is how a test
// builds a database History did not write: one left by an older build, or one
// stamped with a version only a later build understands.
bool runSql(const QString& path, const QStringList& statements) {
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("history-test-raw"));
        db.setDatabaseName(path);
        if (!db.open()) return false;
        QSqlQuery query(db);
        for (const QString& statement : statements) {
            if (query.exec(statement)) continue;
            qCritical("sql failed: %s: %s", qPrintable(statement),
                      qPrintable(query.lastError().text()));
            ok = false;
            break;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("history-test-raw"));
    return ok;
}

model::Message message(int channelIndex, int sequence) {
    model::Message msg;
    msg.channelIndex = channelIndex;
    msg.sender = QStringLiteral("sender-%1").arg(sequence);
    msg.text = QStringLiteral("message-%1").arg(sequence);
    msg.timestamp = QDateTime::fromSecsSinceEpoch(1'700'000'000 + sequence);
    msg.outgoing = sequence % 2 == 0;
    if (sequence % 3 == 0) {
        msg.hasSignal = true;
        msg.snr = -3.5f;
        msg.pathLen = 2;
    }
    return msg;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!check(directory.isValid(), "temporary directory is valid")) return 1;

    const QByteArray deviceA(32, '\x11');
    const QByteArray deviceB(32, '\x22');
    const model::Conversation channelA = model::Conversation::channel(QByteArray(32, '\x33'));
    const model::Conversation channelB = model::Conversation::channel(QByteArray(32, '\x44'));
    const QByteArray peerKey = QByteArray::fromHex("a1b2c3d4e5f6") + QByteArray(26, '\x77');
    const model::Conversation peer = model::Conversation::direct(peerKey);

    {
        model::History history(directory.path());
        if (!check(bool(history.preflight(deviceA)), "preflight opens a device database") ||
            !check(!history.preflight(QByteArray(31, '\x11')),
                   "preflight refuses a malformed device identity"))
            return 1;

        if (!check(bool(history.append(deviceA, channelA, message(3, 1))),
                   "append reports success"))
            return 1;
        model::Message nameless = message(4, 2);
        nameless.sender = QString();
        history.append(deviceA, channelB, nameless);
        history.append(deviceB, channelA, message(7, 3));
        history.append(deviceA, peer, message(-1, 4));

        const QString pathA = databasePath(directory, deviceA);
        const QString pathB = databasePath(directory, deviceB);
        if (!check(QFileInfo::exists(pathA), "device A database exists") ||
            !check(QFileInfo::exists(pathB), "device B database exists") ||
            !check(pathA != pathB, "devices use distinct databases"))
            return 1;

        const model::HistoryMessages a = history.messages(deviceA, channelA, 9);
        if (!check(bool(a.result), "reading a stored conversation succeeds") ||
            !check(a.messages.size() == 1, "device A channel A has one message") ||
            !check(a.messages.at(0).channelIndex == 9,
                   "stored message is rebound to current slot") ||
            !check(a.messages.at(0).conversation == channelA,
                   "a stored message knows the conversation it was read from") ||
            !check(a.messages.at(0).text == QStringLiteral("message-1"),
                   "message text round trips") ||
            !check(!a.messages.at(0).hasSignal, "absent signal remains absent"))
            return 1;

        // An unwritten conversation is a successful read of nothing, which is
        // what tells it apart from a database that cannot be read at all.
        const model::HistoryMessages untouched = history.messages(deviceB, channelB, 1);
        const model::HistoryLatest none = history.latestMessage(deviceB, channelB, 1);
        if (!check(bool(untouched.result) && untouched.messages.isEmpty(),
                   "an empty conversation reads as a success with no rows") ||
            !check(bool(none.result) && !none.message,
                   "latest message reports success when there is no row"))
            return 1;

        const model::HistoryMessages b = history.messages(deviceB, channelA, 7);
        if (!check(b.messages.size() == 1, "device B history is isolated") ||
            !check(b.messages.at(0).text == QStringLiteral("message-3"),
                   "device B message round trips") ||
            !check(b.messages.at(0).hasSignal && b.messages.at(0).pathLen == 2,
                   "signal metadata round trips") ||
            !check(history.messages(deviceA, peer).messages.size() == 1,
                   "a peer's direct messages are their own conversation") ||
            !check(history.messages(deviceA, peer).messages.at(0).channelIndex == -1,
                   "a direct message carries no channel slot"))
            return 1;

        // A conversation is the kind *and* the identity: a channel whose key
        // fingerprint happened to equal a peer's key would still be a different
        // conversation, and nothing may read one as the other.
        const model::Conversation collision = model::Conversation::channel(peerKey);
        history.append(deviceA, collision, message(5, 7));
        if (!check(history.messages(deviceA, peer).messages.size() == 1,
                   "a channel does not join the direct conversation with the same id") ||
            !check(history.messages(deviceA, collision, 5).messages.size() == 1,
                   "the channel keeps its own message"))
            return 1;

        // Scopes the app could not resolve: a message it cannot place is still
        // gone from the node, so it has to land somewhere it can be found.
        const QByteArray orphanDevice = model::History::orphanDeviceId();
        if (!check(orphanDevice != deviceA && orphanDevice.size() == 32,
                   "the orphan device is a device identity nobody has") ||
            !check(model::History::orphanChannel(3) != model::History::orphanChannel(4),
                   "orphan conversations keep the slot they arrived on") ||
            !check(model::History::orphanChannel(3) != channelA,
                   "an orphan conversation cannot collide with a real one") ||
            !check(bool(history.append(deviceA, model::History::orphanChannel(3),
                                       message(3, 5))),
                   "a message with an unresolved channel is stored") ||
            !check(bool(history.append(orphanDevice, channelA, message(3, 6))),
                   "a message with an unknown device is stored") ||
            !check(history.messages(deviceA, model::History::orphanChannel(3), 3)
                           .messages.size() == 1,
                   "the orphan conversation keeps its message") ||
            !check(history.messages(deviceA, channelA, 3).messages.size() == 1,
                   "an orphan does not join the channel it could not be placed in"))
            return 1;

        for (int i = 0; i <= model::History::MaxPerConversation; ++i)
            history.append(deviceA, channelA, message(3, 1000 + i));

        const QVector<model::Message> retained =
            history.messages(deviceA, channelA, 3).messages;
        const model::HistoryLatest latest = history.latestMessage(deviceA, channelA, 12);
        if (!check(retained.size() == model::History::MaxPerConversation,
                   "channel history is capped") ||
            !check(retained.first().text == QStringLiteral("message-1001"),
                   "retention drops the oldest messages") ||
            !check(retained.last().text == QStringLiteral("message-1500"),
                   "retention keeps the newest message") ||
            !check(bool(latest.result) && latest.message.has_value(),
                   "latest message query finds a row") ||
            !check(latest.message->channelIndex == 12,
                   "latest message is rebound to the current slot") ||
            !check(latest.message->text == QStringLiteral("message-1500"),
                   "latest message query returns the newest row"))
            return 1;
    }

    {
        model::History history(directory.path());
        if (!check(history.messages(deviceA, channelA, 3).messages.size() ==
                       model::History::MaxPerConversation,
                   "history survives reopening"))
            return 1;

        if (!check(bool(history.remove(deviceA, channelA)), "removal reports success") ||
            !check(history.messages(deviceA, channelA, 3).messages.isEmpty(),
                   "channel removal deletes its messages") ||
            !check(history.messages(deviceA, channelB, 4).messages.size() == 1,
                   "channel removal leaves sibling channels intact") ||
            !check(history.messages(deviceB, channelA, 7).messages.size() == 1,
                   "channel removal leaves other devices intact") ||
            !check(history.messages(deviceA, peer).messages.size() == 1,
                   "channel removal leaves direct messages intact"))
            return 1;
    }

    // A direct conversation exists because something was said in it: nothing on
    // the node enumerates them, so the sidebar is built from what is stored.
    {
        const QByteArray deviceC(32, '\x33');
        const QByteArray otherKey = QByteArray::fromHex("0102030405060708") +
                                    QByteArray(24, '\x09');
        const model::Conversation other = model::Conversation::direct(otherKey);
        // The peer of a message collected before the address book could name it:
        // all the wire carries is the prefix the daemon matched on.
        const model::Conversation unresolved =
            model::Conversation::direct(otherKey.left(model::Conversation::PeerPrefixSize));

        model::History history(directory.path());
        history.append(deviceC, channelA, message(1, 10));
        history.append(deviceC, peer, message(-1, 11));
        history.append(deviceC, unresolved, message(-1, 12));

        const model::HistoryConversations listed = history.directConversations(deviceC);
        if (!check(bool(listed.result), "listing direct conversations succeeds") ||
            !check(listed.conversations.size() == 2,
                   "each peer is one direct conversation and channels are not listed") ||
            !check(listed.conversations.contains(peer) &&
                       listed.conversations.contains(unresolved),
                   "a peer known only by its prefix is still a conversation"))
            return 1;

        // The whole key turning up is what the prefix rows were waiting for.
        history.append(deviceC, other, message(-1, 13));
        if (!check(bool(history.resolvePeer(deviceC, otherKey)), "resolving a peer succeeds") ||
            !check(history.messages(deviceC, other).messages.size() == 2,
                   "prefix messages join the conversation with the whole key") ||
            !check(history.messages(deviceC, unresolved).messages.isEmpty(),
                   "nothing is left under the prefix") ||
            !check(history.messages(deviceC, other).messages.at(0).text ==
                       QStringLiteral("message-12"),
                   "the adopted message keeps its place in the order") ||
            !check(history.messages(deviceC, peer).messages.size() == 1,
                   "resolving one peer leaves another alone") ||
            !check(history.directConversations(deviceC).conversations.size() == 2,
                   "the resolved conversation is no longer counted twice"))
            return 1;
    }

    // How far one of our own sends got outlives the session that sent it. Nothing
    // can settle a stored send later on, so the row number an append hands back
    // is the only thing an answer arriving a minute afterwards can find it by.
    {
        using SendState = model::Message::SendState;
        const QByteArray deviceD(32, '\x88');
        model::History history(directory.path());

        model::Message sent = message(-1, 20);
        sent.outgoing = true;
        sent.sendState = SendState::Pending;
        qint64 sentRow = 0;
        model::Message arrived = message(-1, 21);
        arrived.outgoing = false;
        qint64 arrivedRow = 0;
        if (!check(bool(history.append(deviceD, peer, sent, &sentRow)) && sentRow > 0,
                   "an append hands back the row it wrote") ||
            !check(bool(history.append(deviceD, peer, arrived, &arrivedRow)) &&
                       arrivedRow != sentRow,
                   "each message gets its own row"))
            return 1;

        const auto storedState = [&](int row) {
            return history.messages(deviceD, peer).messages.at(row).sendState;
        };
        // A send in flight is not a state storage can hold: the message is
        // written down only once the daemon has taken it, so that is what a row
        // says about one however it was handed over.
        if (!check(storedState(0) == SendState::Sent,
                   "a stored send reads back as taken by the daemon") ||
            !check(bool(history.settleSend(deviceD, sentRow, SendState::Delivered)),
                   "settling a stored send succeeds") ||
            !check(storedState(0) == SendState::Delivered,
                   "a peer's confirmation survives the session that heard it") ||
            !check(bool(history.settleSend(deviceD, sentRow, SendState::Unconfirmed)),
                   "a later answer replaces the one recorded") ||
            !check(storedState(0) == SendState::Unconfirmed,
                   "a wait that ran out is remembered as one") ||
            !check(storedState(1) == SendState::Sent,
                   "nothing is claimed about how a message that arrived was sent"))
            return 1;

        const model::HistoryLatest newest = history.latestMessage(deviceD, peer);
        const model::HistoryResult ownRowOnly =
            history.settleSend(deviceD, arrivedRow, SendState::Delivered);
        if (!check(bool(newest.result) && newest.message &&
                       newest.message->sendState == SendState::Sent,
                   "the sidebar's newest-message lookup reads the same column") ||
            !check(bool(ownRowOnly) && storedState(1) == SendState::Sent,
                   "a message somebody else sent us cannot be marked delivered") ||
            !check(bool(history.settleSend(deviceD, arrivedRow + 1000, SendState::Delivered)),
                   "a row retention has already dropped is not a storage failure") ||
            !check(!history.settleSend(deviceD, sentRow, SendState::Pending),
                   "a send still in flight is refused rather than written down") ||
            !check(!history.settleSend(deviceD, 0, SendState::Delivered),
                   "there is no row nought to settle"))
            return 1;

        model::History reopened(directory.path());
        if (!check(reopened.messages(deviceD, peer).messages.at(0).sendState ==
                       SendState::Unconfirmed,
                   "the mark is read back rather than reset on reload"))
            return 1;
    }

    // A version 1 database -- one nullable channel column, every direct message
    // from every peer sharing one conversation and identified by the hex of the
    // prefix in its sender column -- is stepped up rather than abandoned.
    {
        const QByteArray legacy(32, '\x44');
        const QString path = databasePath(directory, legacy);
        const QString channelHex = QString::fromLatin1(channelA.id.toHex());
        if (!check(runSql(path,
                          {QStringLiteral("PRAGMA user_version = 1"),
                           QStringLiteral("CREATE TABLE messages (id INTEGER PRIMARY KEY, "
                                          "channel BLOB, timestamp INTEGER NOT NULL, "
                                          "text TEXT NOT NULL, sender TEXT NOT NULL DEFAULT '', "
                                          "outgoing INTEGER NOT NULL DEFAULT 0, snr REAL, "
                                          "path_len INTEGER)"),
                           QStringLiteral("CREATE INDEX messages_by_channel "
                                          "ON messages(channel, id)"),
                           QStringLiteral("INSERT INTO messages "
                                          "(channel, timestamp, text, sender, outgoing, snr, "
                                          "path_len) VALUES "
                                          "(x'%1', 1700000001, 'on a channel', 'someone', 0, "
                                          "-3.5, 2)")
                               .arg(channelHex),
                           QStringLiteral("INSERT INTO messages "
                                          "(channel, timestamp, text, sender, outgoing) VALUES "
                                          "(NULL, 1700000002, 'from a peer', 'a1b2c3d4e5f6', 0)"),
                           QStringLiteral("INSERT INTO messages "
                                          "(channel, timestamp, text, sender, outgoing) VALUES "
                                          "(NULL, 1700000003, 'from someone else', "
                                          "'0102030405', 0)")}),
                   "a version 1 database can be planted"))
            return 1;

        // Six bytes is all a version 1 row ever held of its peer, so that is
        // what its conversation is under until the address book supplies the
        // rest of the key.
        const model::Conversation prefix =
            model::Conversation::direct(peerKey.left(model::Conversation::PeerPrefixSize));

        model::History history(directory.path());
        const model::HistoryMessages carried = history.messages(legacy, channelA, 2);
        const model::HistoryMessages moved = history.messages(legacy, prefix);
        const model::HistoryConversations directs = history.directConversations(legacy);
        if (!check(bool(carried.result) && carried.messages.size() == 1,
                   "channel messages survive the upgrade") ||
            !check(carried.messages.at(0).text == QStringLiteral("on a channel") &&
                       carried.messages.at(0).hasSignal &&
                       carried.messages.at(0).pathLen == 2,
                   "an upgraded channel message keeps everything it had") ||
            !check(bool(moved.result) && moved.messages.size() == 1,
                   "a direct message lands in the conversation with its peer") ||
            !check(moved.messages.at(0).text == QStringLiteral("from a peer"),
                   "the upgraded direct message keeps its text") ||
            !check(moved.messages.at(0).sender.isEmpty(),
                   "the peer prefix is no longer carried as a sender name") ||
            !check(bool(directs.result) && directs.conversations.size() == 2,
                   "one conversation per peer, not one for all of them"))
            return 1;

        // And the whole key, when it turns up, is what collects them.
        if (!check(bool(history.resolvePeer(legacy, peerKey)),
                   "an upgraded conversation resolves to its peer") ||
            !check(history.messages(legacy, peer).messages.size() == 1,
                   "the upgraded message follows the peer it was always from"))
            return 1;

        // A sender that is not the prefix this version wrote names no peer.
        // Those messages share a conversation nobody's key can reach rather
        // than being dropped or filed under somebody real.
        const model::Conversation unplaceable = model::Conversation::direct(
            QByteArray(model::Conversation::PeerPrefixSize, '\0'));
        if (!check(history.messages(legacy, unplaceable).messages.size() == 1,
                   "a direct message whose peer cannot be recovered is still kept"))
            return 1;

        // Reopening reads the upgraded shape rather than upgrading again.
        model::History reopened(directory.path());
        if (!check(reopened.messages(legacy, peer).messages.size() == 1,
                   "the upgrade is recorded and not repeated"))
            return 1;
    }

    // A version 2 database -- conversations already, but nothing written down
    // about how a send fared -- gains the column and keeps every row it had.
    {
        using SendState = model::Message::SendState;
        const QByteArray legacy(32, '\xa1');
        const QString path = databasePath(directory, legacy);
        if (!check(runSql(path,
                          {QStringLiteral("PRAGMA user_version = 2"),
                           QStringLiteral("CREATE TABLE messages (id INTEGER PRIMARY KEY, "
                                          "conv_kind INTEGER NOT NULL, conv_id BLOB NOT NULL, "
                                          "timestamp INTEGER NOT NULL, text TEXT NOT NULL, "
                                          "sender TEXT NOT NULL DEFAULT '', "
                                          "outgoing INTEGER NOT NULL DEFAULT 0, snr REAL, "
                                          "path_len INTEGER)"),
                           QStringLiteral("CREATE INDEX messages_by_conversation "
                                          "ON messages(conv_kind, conv_id, id)"),
                           QStringLiteral("INSERT INTO messages "
                                          "(conv_kind, conv_id, timestamp, text, sender, "
                                          "outgoing) VALUES (%1, x'%2', 1700000010, "
                                          "'sent before there was a column', '', 1)")
                               .arg(int(model::ConversationKind::Direct))
                               .arg(QString::fromLatin1(peerKey.toHex()))}),
                   "a version 2 database can be planted"))
            return 1;

        model::History history(directory.path());
        const model::HistoryMessages carried = history.messages(legacy, peer);
        if (!check(bool(carried.result) && carried.messages.size() == 1,
                   "a version 2 message survives the upgrade") ||
            !check(carried.messages.at(0).text ==
                       QStringLiteral("sent before there was a column"),
                   "the upgraded message keeps its text") ||
            !check(carried.messages.at(0).sendState == SendState::Sent,
                   "a send stored before the column reads as taken by the daemon"))
            return 1;

        // And the column the step added is one this build can write to.
        model::Message sent = message(-1, 30);
        sent.outgoing = true;
        qint64 rowId = 0;
        if (!check(bool(history.append(legacy, peer, sent, &rowId)),
                   "the upgraded database takes a new message") ||
            !check(bool(history.settleSend(legacy, rowId, SendState::Delivered)),
                   "the added column can be settled onto") ||
            !check(history.messages(legacy, peer).messages.at(1).sendState ==
                       SendState::Delivered,
                   "and it reads back"))
            return 1;

        // A version 1 database takes both steps, which is the only path where the
        // column is added to a table another step has just built.
        const QByteArray older(32, '\xa2');
        if (!check(runSql(databasePath(directory, older),
                          {QStringLiteral("PRAGMA user_version = 1"),
                           QStringLiteral("CREATE TABLE messages (id INTEGER PRIMARY KEY, "
                                          "channel BLOB, timestamp INTEGER NOT NULL, "
                                          "text TEXT NOT NULL, sender TEXT NOT NULL DEFAULT '', "
                                          "outgoing INTEGER NOT NULL DEFAULT 0, snr REAL, "
                                          "path_len INTEGER)")}),
                   "a version 1 database can be planted alongside"))
            return 1;

        qint64 twiceUpgraded = 0;
        if (!check(bool(history.append(older, peer, sent, &twiceUpgraded)),
                   "a database stepped up twice takes a message") ||
            !check(bool(history.settleSend(older, twiceUpgraded, SendState::Unconfirmed)) &&
                       history.messages(older, peer).messages.at(0).sendState ==
                           SendState::Unconfirmed,
                   "both steps ran, in order"))
            return 1;

        // A version 2 database that holds a version and no table has nothing for
        // the step to alter, and the current DDL builds it with the column in it.
        const QByteArray halfBuilt(32, '\xa3');
        if (!check(runSql(databasePath(directory, halfBuilt),
                          {QStringLiteral("PRAGMA user_version = 2")}),
                   "a version-only version 2 database can be planted"))
            return 1;

        qint64 repaired = 0;
        if (!check(bool(history.append(halfBuilt, peer, sent, &repaired)),
                   "a version 2 database with no table is repaired rather than refused") ||
            !check(bool(history.settleSend(halfBuilt, repaired, SendState::Delivered)),
                   "the repaired table has the column"))
            return 1;
    }

    // Every database says which shape it is in, and one written by a later
    // build is refused rather than appended to in a shape it does not use.
    {
        const QString pathA = databasePath(directory, deviceA);
        model::History history(directory.path());
        const model::HistoryResult stamped = history.preflight(deviceA);
        if (!check(bool(stamped), "an existing database still opens")) return 1;

        const QByteArray fromTheFuture(32, '\x55');
        if (!check(runSql(databasePath(directory, fromTheFuture),
                          {QStringLiteral("PRAGMA user_version = %1")
                               .arg(model::History::SchemaVersion + 1)}),
                   "a future-version database can be planted"))
            return 1;

        model::History later(directory.path());
        const model::HistoryResult refused = later.preflight(fromTheFuture);
        const model::HistoryResult appended =
            later.append(fromTheFuture, channelA, message(3, 1));
        if (!check(!refused && refused.error.contains(QStringLiteral("newer Corelet")),
                   "a database from a later build is refused by name") ||
            !check(!appended, "nothing is written to a database that was refused"))
            return 1;

        // A database whose version was recorded but whose table never was --
        // an open that failed between the two -- repairs itself rather than
        // failing every operation from then on.
        const QByteArray halfBuilt(32, '\x66');
        if (!check(runSql(databasePath(directory, halfBuilt),
                          {QStringLiteral("PRAGMA user_version = %1")
                               .arg(model::History::SchemaVersion)}),
                   "a version-only database can be planted"))
            return 1;

        model::History repaired(directory.path());
        if (!check(bool(repaired.append(halfBuilt, channelA, message(3, 1))),
                   "a database holding a version but no table is repaired on open"))
            return 1;

        if (!check(QFileInfo::exists(pathA), "the planted databases left device A alone"))
            return 1;
    }

    // Storage that cannot work at all has to say so on every operation rather
    // than look like an empty history: a silent failure here is a collected
    // message destroyed, since the node no longer holds a copy of it.
    {
        const QString blocked = directory.filePath(QStringLiteral("not-a-directory"));
        QFile file(blocked);
        if (!check(file.open(QIODevice::WriteOnly), "the blocking file can be created"))
            return 1;
        file.close();

        model::History history(blocked);
        const model::HistoryResult opened = history.preflight(deviceA);
        const model::HistoryResult appended = history.append(deviceA, channelA, message(3, 1));
        const model::HistoryMessages read = history.messages(deviceA, channelA, 3);
        const model::HistoryLatest newest = history.latestMessage(deviceA, channelA, 3);
        const model::HistoryResult removed = history.remove(deviceA, channelA);
        const model::HistoryConversations listed = history.directConversations(deviceA);
        if (!check(!opened && !opened.error.isEmpty(), "preflight reports why it failed") ||
            !check(!appended && !appended.error.isEmpty(), "append reports why it failed") ||
            !check(!read.result && read.messages.isEmpty(), "a failed read is not an empty one") ||
            !check(!newest.result && !newest.message,
                   "a failed latest lookup is not an absent row") ||
            !check(!removed && !removed.error.isEmpty(), "removal reports why it failed") ||
            !check(!listed.result && listed.conversations.isEmpty(),
                   "a failed conversation listing is not an empty address book"))
            return 1;
    }

    return 0;
}
