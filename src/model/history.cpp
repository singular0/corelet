#include "model/history.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace model {

namespace {

// SQLite's own words are the only useful part of a failure -- "attempt to write
// a readonly database", "disk I/O error" -- and they are what the user needs to
// see, since only they can do anything about it.
QString sqlError(const QSqlError& error) {
    const QString text = error.text().simplified();
    return text.isEmpty() ? QStringLiteral("the database reported no reason") : text;
}

Message storedMessage(const QSqlQuery& query, const Conversation& conversation,
                      int channelIndex) {
    Message msg;
    msg.conversation = conversation;
    msg.channelIndex = conversation.isChannel() ? channelIndex : -1;
    msg.timestamp = QDateTime::fromSecsSinceEpoch(query.value(0).toLongLong());
    msg.text = query.value(1).toString();
    msg.sender = query.value(2).toString();
    msg.outgoing = query.value(3).toBool();
    msg.hasSignal = !query.value(4).isNull();
    if (msg.hasSignal) {
        msg.snr = query.value(4).toFloat();
        msg.pathLen = query.value(5).toInt();
    }
    // Nothing recorded is what an incoming message stores and what every row
    // written before version 3 holds, and both mean the same as they always
    // did: the daemon took it, and nothing beyond that is known here.
    if (!query.value(6).isNull()) msg.sendState = sendStateFromInt(query.value(6).toInt());
    return msg;
}

// What a row records about how far one of our own sends got. Nothing for an
// incoming message: the field is meaningless for one, and leaving it out is
// also what makes an older build's rows read back correctly. A send still in
// flight cannot be stored -- a message is written down only once the daemon has
// taken it -- so nothing on disk ever claims a wait that nobody is holding.
QVariant sendStateColumn(const Message& msg) {
    if (!msg.outgoing) return {};
    return int(msg.sendState == Message::SendState::Pending ? Message::SendState::Sent
                                                            : msg.sendState);
}

// The two columns that together are one conversation, bound in the order every
// statement here lists them.
void bindConversation(QSqlQuery& query, const Conversation& conversation) {
    query.addBindValue(int(conversation.kind));
    query.addBindValue(conversation.id);
}

constexpr auto SelectColumns =
    "timestamp, text, sender, outgoing, snr, path_len, send_state";

}  // namespace

History::History(QString directory) : directory_(std::move(directory)) {}

History::~History() {
    const QList<QString> names = connectionNames_.values();
    connectionNames_.clear();
    for (const QString& name : names) {
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            if (db.isValid()) db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
}

QByteArray History::orphanDeviceId() {
    return QByteArray(DeviceIdSize, '\0');
}

Conversation History::orphanChannel(int wireSlot) {
    QByteArray fingerprint(Conversation::IdSize, '\0');
    // Slots are a single wire byte; anything else is not a slot at all and
    // shares the one conversation 0xFF stands for.
    fingerprint[Conversation::IdSize - 1] =
        char(wireSlot < 0 || wireSlot > 0xFE ? 0xFF : wireSlot);
    return Conversation::channel(fingerprint);
}

HistoryResult History::preflight(const QByteArray& deviceId) {
    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);
    return HistoryResult::success();
}

HistoryMessages History::messages(const QByteArray& deviceId,
                                  const Conversation& conversation, int channelIndex) const {
    if (deviceId.size() != DeviceIdSize || !conversation.isValid())
        return {HistoryResult::failure(QStringLiteral("the conversation has no identity")), {}};

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return {HistoryResult::failure(error), {}};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral("SELECT %1 FROM messages "
                                      "WHERE conv_kind = ? AND conv_id = ? "
                                      "ORDER BY id DESC LIMIT ?")
                           .arg(QLatin1String(SelectColumns))))
        return {HistoryResult::failure(sqlError(query.lastError())), {}};
    bindConversation(query, conversation);
    query.addBindValue(MaxPerConversation);
    if (!query.exec()) return {HistoryResult::failure(sqlError(query.lastError())), {}};

    QVector<Message> result;
    result.reserve(MaxPerConversation);
    while (query.next()) result.append(storedMessage(query, conversation, channelIndex));

    // The descending index walk makes LIMIT cheap; the view still consumes
    // messages in their original append order.
    std::reverse(result.begin(), result.end());
    return {HistoryResult::success(), std::move(result)};
}

HistoryLatest History::latestMessage(const QByteArray& deviceId,
                                     const Conversation& conversation,
                                     int channelIndex) const {
    if (deviceId.size() != DeviceIdSize || !conversation.isValid())
        return {HistoryResult::failure(QStringLiteral("the conversation has no identity")), {}};

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return {HistoryResult::failure(error), {}};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral("SELECT %1 FROM messages "
                                      "WHERE conv_kind = ? AND conv_id = ? "
                                      "ORDER BY id DESC LIMIT 1")
                           .arg(QLatin1String(SelectColumns))))
        return {HistoryResult::failure(sqlError(query.lastError())), {}};
    bindConversation(query, conversation);
    if (!query.exec()) return {HistoryResult::failure(sqlError(query.lastError())), {}};
    // No row is an empty conversation, which is a perfectly good answer.
    if (!query.next()) return {HistoryResult::success(), std::nullopt};
    return {HistoryResult::success(), storedMessage(query, conversation, channelIndex)};
}

HistoryResult History::append(const QByteArray& deviceId, const Conversation& conversation,
                              const Message& msg, qint64* rowId) {
    // Nothing is stored yet, and a caller holding a row number from an append
    // that failed would settle a send onto somebody else's message.
    if (rowId) *rowId = 0;
    if (deviceId.size() != DeviceIdSize || !conversation.isValid())
        return HistoryResult::failure(QStringLiteral("the conversation has no identity"));

    QString error;
    QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);
    if (!db.transaction()) return HistoryResult::failure(sqlError(db.lastError()));

    QSqlQuery insert(db);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO messages "
            "(conv_kind, conv_id, timestamp, text, sender, outgoing, snr, path_len, "
            "send_state) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"))) {
        const QString reason = sqlError(insert.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    bindConversation(insert, conversation);
    insert.addBindValue(msg.timestamp.toSecsSinceEpoch());
    insert.addBindValue(msg.text.isNull() ? QStringLiteral("") : msg.text);
    insert.addBindValue(msg.sender.isNull() ? QStringLiteral("") : msg.sender);
    insert.addBindValue(msg.outgoing);
    insert.addBindValue(msg.hasSignal ? QVariant(double(msg.snr)) : QVariant());
    insert.addBindValue(msg.hasSignal ? QVariant(msg.pathLen) : QVariant());
    insert.addBindValue(sendStateColumn(msg));
    if (!insert.exec()) {
        const QString reason = sqlError(insert.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    // Read here rather than after the commit, which is one more statement away
    // and leaves the driver with a different query's id to hand back.
    const QVariant inserted = insert.lastInsertId();

    // The conversation/id index finds the retention boundary without scanning
    // any other conversation or rewriting the database.
    QSqlQuery trim(db);
    if (!trim.prepare(QStringLiteral(
            "DELETE FROM messages WHERE conv_kind = ? AND conv_id = ? AND id < COALESCE(("
            "SELECT id FROM messages WHERE conv_kind = ? AND conv_id = ? "
            "ORDER BY id DESC LIMIT 1 OFFSET ?), 0)"))) {
        const QString reason = sqlError(trim.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    bindConversation(trim, conversation);
    bindConversation(trim, conversation);
    trim.addBindValue(MaxPerConversation - 1);
    if (!trim.exec()) {
        const QString reason = sqlError(trim.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    // The message is only stored once the commit returns.
    if (!db.commit()) {
        const QString reason = sqlError(db.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    if (rowId) *rowId = inserted.toLongLong();
    return HistoryResult::success();
}

HistoryResult History::settleSend(const QByteArray& deviceId, qint64 rowId,
                                  Message::SendState state) {
    if (deviceId.size() != DeviceIdSize || rowId <= 0)
        return HistoryResult::failure(QStringLiteral("there is no stored message to settle"));
    // Storage holds what a send came to, never a wait still in progress. Writing
    // one down would leave a row a later session can only draw as waiting on an
    // answer nothing is listening for.
    if (state == Message::SendState::Pending)
        return HistoryResult::failure(QStringLiteral("a send in flight is not a stored state"));

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);

    QSqlQuery query(db);
    // The row number is the whole address: by the time an ack arrives the
    // conversation may have been reloaded, and neither the on-screen row nor the
    // tag it was drawn under is anything storage ever knew about. `outgoing`
    // is belt and braces -- how a send fared is not something to write onto a
    // message somebody else sent us.
    if (!query.prepare(QStringLiteral(
            "UPDATE messages SET send_state = ? WHERE id = ? AND outgoing = 1")))
        return HistoryResult::failure(sqlError(query.lastError()));
    query.addBindValue(int(state));
    query.addBindValue(rowId);
    if (!query.exec()) return HistoryResult::failure(sqlError(query.lastError()));
    // Matching no row is a success: retention drops the oldest messages, and a
    // conversation busy enough to trim one out from under its own ack is not
    // storage refusing to work.
    return HistoryResult::success();
}

HistoryResult History::remove(const QByteArray& deviceId, const Conversation& conversation) {
    if (deviceId.size() != DeviceIdSize || !conversation.isValid())
        return HistoryResult::failure(QStringLiteral("the conversation has no identity"));

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral(
            "DELETE FROM messages WHERE conv_kind = ? AND conv_id = ?")))
        return HistoryResult::failure(sqlError(query.lastError()));
    bindConversation(query, conversation);
    if (!query.exec()) return HistoryResult::failure(sqlError(query.lastError()));
    return HistoryResult::success();
}

HistoryConversations History::directConversations(const QByteArray& deviceId) const {
    if (deviceId.size() != DeviceIdSize)
        return {HistoryResult::failure(QStringLiteral("no device identity is known")), {}};

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return {HistoryResult::failure(error), {}};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    // Newest first, so the caller has the conversations somebody is actually in
    // at the front of the list whatever it decides to do with the order.
    if (!query.prepare(QStringLiteral(
            "SELECT conv_id FROM messages WHERE conv_kind = ? "
            "GROUP BY conv_id ORDER BY MAX(id) DESC")))
        return {HistoryResult::failure(sqlError(query.lastError())), {}};
    query.addBindValue(int(ConversationKind::Direct));
    if (!query.exec()) return {HistoryResult::failure(sqlError(query.lastError())), {}};

    QVector<Conversation> result;
    while (query.next()) {
        const Conversation peer = Conversation::direct(query.value(0).toByteArray());
        // A row of some other shape is not a peer this build can address; it is
        // not worth a failure either, since every other row still reads.
        if (peer.isValid()) result.append(peer);
    }
    return {HistoryResult::success(), std::move(result)};
}

HistoryResult History::resolvePeer(const QByteArray& deviceId, const QByteArray& peerKey) {
    if (deviceId.size() != DeviceIdSize || peerKey.size() != Conversation::IdSize)
        return HistoryResult::failure(QStringLiteral("the conversation has no identity"));

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);

    QSqlQuery query(db);
    // Only the prefix rows move: a conversation already under the whole key is
    // the one they are joining. Two peers sharing six bytes of key is the same
    // 2^48 assumption the daemon makes when it matches an incoming message to a
    // contact, so nothing here is more optimistic than the wire already is.
    if (!query.prepare(QStringLiteral(
            "UPDATE messages SET conv_id = ? WHERE conv_kind = ? AND conv_id = ?")))
        return HistoryResult::failure(sqlError(query.lastError()));
    query.addBindValue(peerKey);
    query.addBindValue(int(ConversationKind::Direct));
    query.addBindValue(peerKey.left(Conversation::PeerPrefixSize));
    if (!query.exec()) return HistoryResult::failure(sqlError(query.lastError()));
    return HistoryResult::success();
}

QSqlDatabase History::databaseFor(const QByteArray& deviceId, QString* error) const {
    const auto fail = [error](const QString& text) {
        if (error) *error = text;
        return QSqlDatabase();
    };

    if (deviceId.size() != DeviceIdSize)
        return fail(QStringLiteral("no device identity is known"));

    const auto existing = connectionNames_.constFind(deviceId);
    if (existing != connectionNames_.constEnd()) {
        QSqlDatabase db = QSqlDatabase::database(*existing, false);
        if (!db.isValid()) return fail(QStringLiteral("the database connection was lost"));
        if (!db.isOpen() && !db.open()) return fail(sqlError(db.lastError()));
        return db;
    }

    if (!QDir().mkpath(directory_))
        return fail(QStringLiteral("cannot create %1").arg(QDir::toNativeSeparators(directory_)));

    const QString deviceName = QString::fromLatin1(deviceId.toHex());
    const QString connectionName =
        QStringLiteral("corelet-history-%1-%2")
            .arg(quintptr(this), 0, 16)
            .arg(deviceName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(QDir(directory_).filePath(deviceName + QStringLiteral(".sqlite3")));
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        const QString reason = sqlError(db.lastError());
        db = {};
        QSqlDatabase::removeDatabase(connectionName);
        return fail(reason);
    }

    QString initError;
    {
        QSqlQuery query(db);
        const auto step = [&](const QString& statement) {
            if (!initError.isEmpty()) return;
            if (!query.exec(statement)) initError = sqlError(query.lastError());
        };
        // WAL keeps appends short and leaves readers unblocked if a future UI
        // reads history off the main thread.
        step(QStringLiteral("PRAGMA journal_mode = WAL"));
        // NORMAL, considered against FULL and kept. History is the only copy of
        // a collected message, but in WAL mode NORMAL already survives
        // everything short of losing power: the app being killed, crashing or
        // torn down mid-write costs nothing, because the WAL is in the OS page
        // cache and the kernel outlives the process, and WAL cannot corrupt the
        // database the way a rollback journal at NORMAL can. What it does not
        // survive is a battery pull or a kernel panic in the seconds after a
        // commit, and buying that back costs an fsync to an SD card on every
        // received message -- on the write path of a backlog drain, on hardware
        // where that is the expensive syscall. Not worth it for the window it
        // closes.
        step(QStringLiteral("PRAGMA synchronous = NORMAL"));
    }
    if (initError.isEmpty()) initError = migrate(db);
    if (!initError.isEmpty()) {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(connectionName);
        return fail(initError);
    }

    connectionNames_.insert(deviceId, connectionName);
    return db;
}

QString History::migrate(QSqlDatabase& db) {
    QSqlQuery read(db);
    if (!read.exec(QStringLiteral("PRAGMA user_version")) || !read.next())
        return sqlError(read.lastError());
    const int found = read.value(0).toInt();
    read.finish();

    // A database written by a later build is somebody's message history in a
    // shape this one cannot read. Refusing it leaves it intact for the build
    // that can; opening it anyway would append rows in the old shape and trim
    // conversations by rules that no longer apply.
    if (found > SchemaVersion)
        return QStringLiteral("the message history is version %1 and this build of Corelet "
                              "reads version %2; use a newer Corelet")
            .arg(found)
            .arg(SchemaVersion);

    QString error;
    const auto step = [&](const QString& statement) {
        if (!error.isEmpty()) return;
        QSqlQuery query(db);
        if (!query.exec(statement)) error = sqlError(query.lastError());
    };

    if (found < SchemaVersion) {
        // Each version's steps run in one transaction with the version bump
        // that records them, so a database is either wholly at the version it
        // claims or wholly back where it started. Version 0 is a database with
        // nothing in it yet, which the current DDL below builds directly.
        //
        // Every step is guarded by the versions it applies to rather than by
        // "older than current", so a version 1 database takes both of them in
        // order and adding a fourth version does not silently re-run either.
        if (!db.transaction()) return sqlError(db.lastError());
        if (found == 1) error = upgradeToConversations(db);
        if (error.isEmpty() && found >= 1 && found < 3) error = upgradeToSendState(db);
        step(QStringLiteral("PRAGMA user_version = %1").arg(SchemaVersion));
        if (!error.isEmpty()) {
            db.rollback();
            return error;
        }
        if (!db.commit()) {
            const QString reason = sqlError(db.lastError());
            db.rollback();
            return reason;
        }
    }

    // The current shape, applied on every open rather than only to a brand-new
    // file: it creates the tables of a database that has none, and is a no-op
    // for one that has them. That is also what repairs a database left holding
    // a version but no table by an open that failed between the two.
    step(QStringLiteral("CREATE TABLE IF NOT EXISTS messages ("
                        "id INTEGER PRIMARY KEY, "
                        // model::ConversationKind, and the identity that kind
                        // is addressed by: a channel key's SHA-256, or a peer's
                        // public key -- or the six bytes of one the wire gave,
                        // until the address book resolves it.
                        "conv_kind INTEGER NOT NULL, "
                        "conv_id BLOB NOT NULL, "
                        "timestamp INTEGER NOT NULL, "
                        "text TEXT NOT NULL, "
                        "sender TEXT NOT NULL DEFAULT '', "
                        "outgoing INTEGER NOT NULL DEFAULT 0, "
                        "snr REAL, "
                        "path_len INTEGER, "
                        // model::Message::SendState, and only for one of our own
                        // sends: how far a message got is meaningless for one
                        // that arrived, so an incoming row leaves it null.
                        "send_state INTEGER)"));
    step(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_by_conversation "
                        "ON messages(conv_kind, conv_id, id)"));
    // Written back even when it is already this, because it is the one *write*
    // an open performs: opening a database says nothing about being able to add
    // to it, and a read-only file or a full card has to fail here rather than
    // while storing a message the node has already discarded.
    step(QStringLiteral("PRAGMA user_version = %1").arg(SchemaVersion));
    return error;
}

QString History::upgradeToConversations(QSqlDatabase& db) {
    QString error;
    const auto step = [&](const QString& statement) {
        if (!error.isEmpty()) return;
        QSqlQuery query(db);
        if (!query.exec(statement)) error = sqlError(query.lastError());
    };

    // Version 1 kept one nullable `channel` column: a fingerprint for a channel
    // and NULL for the single conversation every direct message from every peer
    // shared. The rows move rather than the column being widened, because what
    // a direct row belongs to has to be worked out one row at a time.
    step(QStringLiteral("ALTER TABLE messages RENAME TO messages_v1"));
    step(QStringLiteral("DROP INDEX IF EXISTS messages_by_channel"));
    step(QStringLiteral("CREATE TABLE messages ("
                        "id INTEGER PRIMARY KEY, "
                        "conv_kind INTEGER NOT NULL, "
                        "conv_id BLOB NOT NULL, "
                        "timestamp INTEGER NOT NULL, "
                        "text TEXT NOT NULL, "
                        "sender TEXT NOT NULL DEFAULT '', "
                        "outgoing INTEGER NOT NULL DEFAULT 0, "
                        "snr REAL, "
                        "path_len INTEGER)"));
    // Channel rows carry their identity already, so they copy across whole and
    // keep their ids -- which is the order the retention trim reads them in.
    step(QStringLiteral("INSERT INTO messages "
                        "(id, conv_kind, conv_id, timestamp, text, sender, outgoing, snr, "
                        "path_len) "
                        "SELECT id, %1, channel, timestamp, text, sender, outgoing, snr, "
                        "path_len FROM messages_v1 WHERE channel IS NOT NULL")
                  .arg(int(ConversationKind::Channel)));
    if (!error.isEmpty()) return error;

    // Direct rows kept the peer as the hex of the six-byte prefix the daemon
    // matched on, in the column that now holds a sender's name. Decoding it is
    // what gives each peer its own conversation, and there is no portable SQL
    // for that -- so the rows are carried over here, bounded by the version 1
    // retention cap of 500.
    QVector<QVariantList> directRows;
    {
        QSqlQuery read(db);
        read.setForwardOnly(true);
        if (!read.exec(QStringLiteral(
                "SELECT id, timestamp, text, sender, outgoing, snr, path_len "
                "FROM messages_v1 WHERE channel IS NULL ORDER BY id")))
            return sqlError(read.lastError());
        while (read.next()) {
            QVariantList row;
            for (int column = 0; column < 7; column++) row.append(read.value(column));
            directRows.append(row);
        }
    }

    QSqlQuery insert(db);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO messages "
            "(id, conv_kind, conv_id, timestamp, text, sender, outgoing, snr, path_len) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")))
        return sqlError(insert.lastError());

    for (const QVariantList& row : directRows) {
        const QByteArray prefix = QByteArray::fromHex(row.at(3).toString().toLatin1());
        insert.addBindValue(row.at(0));
        insert.addBindValue(int(ConversationKind::Direct));
        // Anything that is not the prefix this version wrote leaves a message
        // whose peer cannot be recovered. An all-zero prefix is not a key
        // anybody holds, so those share one conversation of their own rather
        // than being dropped or filed under somebody real.
        insert.addBindValue(prefix.size() == Conversation::PeerPrefixSize
                                ? prefix
                                : QByteArray(Conversation::PeerPrefixSize, '\0'));
        insert.addBindValue(row.at(1));
        insert.addBindValue(row.at(2));
        // The prefix was never a name to show; the peer is the conversation now.
        insert.addBindValue(QStringLiteral(""));
        insert.addBindValue(row.at(4));
        insert.addBindValue(row.at(5));
        insert.addBindValue(row.at(6));
        if (!insert.exec()) return sqlError(insert.lastError());
    }

    step(QStringLiteral("DROP TABLE messages_v1"));
    return error;
}

QString History::upgradeToSendState(QSqlDatabase& db) {
    QSqlQuery query(db);
    // A database left holding a version but no table -- an open that failed
    // between recording the one and creating the other -- has nothing to alter,
    // and the current DDL builds it with the column already in place.
    if (!query.exec(QStringLiteral("SELECT 1 FROM sqlite_master "
                                   "WHERE type = 'table' AND name = 'messages'")))
        return sqlError(query.lastError());
    const bool present = query.next();
    query.finish();
    if (!present) return {};

    // Nothing is backfilled. A row written before this column says nothing about
    // how its send fared, and null is exactly that -- the same thing an incoming
    // message stores, and what reads back as the plain Sent every stored message
    // used to be.
    if (!query.exec(QStringLiteral("ALTER TABLE messages ADD COLUMN send_state INTEGER")))
        return sqlError(query.lastError());
    return {};
}

}  // namespace model
