#include "model/history.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace model {

namespace {

// Qt distinguishes null and non-null empty byte arrays when binding them.
// History does not: every empty key is the direct-message conversation.
QVariant storedChannel(const QByteArray& channelKeyFingerprint) {
    return channelKeyFingerprint.isEmpty() ? QVariant() : QVariant(channelKeyFingerprint);
}

// SQLite's own words are the only useful part of a failure -- "attempt to write
// a readonly database", "disk I/O error" -- and they are what the user needs to
// see, since only they can do anything about it.
QString sqlError(const QSqlError& error) {
    const QString text = error.text().simplified();
    return text.isEmpty() ? QStringLiteral("the database reported no reason") : text;
}

Message storedMessage(const QSqlQuery& query, int channelIndex) {
    Message msg;
    msg.channelIndex = channelIndex;
    msg.timestamp = QDateTime::fromSecsSinceEpoch(query.value(0).toLongLong());
    msg.text = query.value(1).toString();
    msg.sender = query.value(2).toString();
    msg.outgoing = query.value(3).toBool();
    msg.hasSignal = !query.value(4).isNull();
    if (msg.hasSignal) {
        msg.snr = query.value(4).toFloat();
        msg.pathLen = query.value(5).toInt();
    }
    return msg;
}

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

QByteArray History::orphanChannel(int wireSlot) {
    QByteArray fingerprint(ChannelFingerprintSize, '\0');
    // Slots are a single wire byte; anything else is not a slot at all and
    // shares the one conversation 0xFF stands for.
    fingerprint[ChannelFingerprintSize - 1] =
        char(wireSlot < 0 || wireSlot > 0xFE ? 0xFF : wireSlot);
    return fingerprint;
}

HistoryResult History::preflight(const QByteArray& deviceId) {
    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);
    return HistoryResult::success();
}

HistoryMessages History::messages(const QByteArray& deviceId,
                                  const QByteArray& channelKeyFingerprint,
                                  int channelIndex) const {
    if (!validScope(deviceId, channelKeyFingerprint, channelIndex))
        return {HistoryResult::failure(QStringLiteral("the conversation has no identity")), {}};

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return {HistoryResult::failure(error), {}};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT timestamp, text, sender, outgoing, snr, path_len "
            "FROM messages WHERE channel IS ? ORDER BY id DESC LIMIT ?")))
        return {HistoryResult::failure(sqlError(query.lastError())), {}};
    query.addBindValue(storedChannel(channelKeyFingerprint));
    query.addBindValue(MaxPerChannel);
    if (!query.exec()) return {HistoryResult::failure(sqlError(query.lastError())), {}};

    QVector<Message> result;
    result.reserve(MaxPerChannel);
    while (query.next()) result.append(storedMessage(query, channelIndex));

    // The descending index walk makes LIMIT cheap; the view still consumes
    // messages in their original append order.
    std::reverse(result.begin(), result.end());
    return {HistoryResult::success(), std::move(result)};
}

HistoryLatest History::latestMessage(const QByteArray& deviceId,
                                     const QByteArray& channelKeyFingerprint,
                                     int channelIndex) const {
    if (!validScope(deviceId, channelKeyFingerprint, channelIndex))
        return {HistoryResult::failure(QStringLiteral("the conversation has no identity")), {}};

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return {HistoryResult::failure(error), {}};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT timestamp, text, sender, outgoing, snr, path_len "
            "FROM messages WHERE channel IS ? ORDER BY id DESC LIMIT 1")))
        return {HistoryResult::failure(sqlError(query.lastError())), {}};
    query.addBindValue(storedChannel(channelKeyFingerprint));
    if (!query.exec()) return {HistoryResult::failure(sqlError(query.lastError())), {}};
    // No row is an empty conversation, which is a perfectly good answer.
    if (!query.next()) return {HistoryResult::success(), std::nullopt};
    return {HistoryResult::success(), storedMessage(query, channelIndex)};
}

HistoryResult History::append(const QByteArray& deviceId,
                              const QByteArray& channelKeyFingerprint, const Message& msg) {
    if (!validScope(deviceId, channelKeyFingerprint, msg.channelIndex))
        return HistoryResult::failure(QStringLiteral("the conversation has no identity"));

    QString error;
    QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);
    if (!db.transaction()) return HistoryResult::failure(sqlError(db.lastError()));

    QSqlQuery insert(db);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO messages "
            "(channel, timestamp, text, sender, outgoing, snr, path_len) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"))) {
        const QString reason = sqlError(insert.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    insert.addBindValue(storedChannel(channelKeyFingerprint));
    insert.addBindValue(msg.timestamp.toSecsSinceEpoch());
    insert.addBindValue(msg.text.isNull() ? QStringLiteral("") : msg.text);
    insert.addBindValue(msg.sender.isNull() ? QStringLiteral("") : msg.sender);
    insert.addBindValue(msg.outgoing);
    insert.addBindValue(msg.hasSignal ? QVariant(double(msg.snr)) : QVariant());
    insert.addBindValue(msg.hasSignal ? QVariant(msg.pathLen) : QVariant());
    if (!insert.exec()) {
        const QString reason = sqlError(insert.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }

    // The channel/id index finds the retention boundary without scanning any
    // other conversation or rewriting the database.
    QSqlQuery trim(db);
    if (!trim.prepare(QStringLiteral(
            "DELETE FROM messages WHERE channel IS ? AND id < COALESCE(("
            "SELECT id FROM messages WHERE channel IS ? "
            "ORDER BY id DESC LIMIT 1 OFFSET ?), 0)"))) {
        const QString reason = sqlError(trim.lastError());
        db.rollback();
        return HistoryResult::failure(reason);
    }
    trim.addBindValue(storedChannel(channelKeyFingerprint));
    trim.addBindValue(storedChannel(channelKeyFingerprint));
    trim.addBindValue(MaxPerChannel - 1);
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
    return HistoryResult::success();
}

HistoryResult History::remove(const QByteArray& deviceId,
                              const QByteArray& channelKeyFingerprint) {
    if (deviceId.size() != DeviceIdSize ||
        channelKeyFingerprint.size() != ChannelFingerprintSize)
        return HistoryResult::failure(QStringLiteral("the conversation has no identity"));

    QString error;
    const QSqlDatabase db = databaseFor(deviceId, &error);
    if (!db.isOpen()) return HistoryResult::failure(error);

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral("DELETE FROM messages WHERE channel IS ?")))
        return HistoryResult::failure(sqlError(query.lastError()));
    query.addBindValue(storedChannel(channelKeyFingerprint));
    if (!query.exec()) return HistoryResult::failure(sqlError(query.lastError()));
    return HistoryResult::success();
}

bool History::validScope(const QByteArray& deviceId,
                         const QByteArray& channelKeyFingerprint, int channelIndex) {
    if (deviceId.size() != DeviceIdSize) return false;
    if (channelIndex < 0) return channelKeyFingerprint.isEmpty();
    return channelKeyFingerprint.size() == ChannelFingerprintSize;
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
        // Nothing reads this back yet. It is here because it is a *write*:
        // opening a database says nothing about being able to add to it, and a
        // read-only file or a full card has to fail here rather than while
        // storing a message the node has already discarded.
        step(QStringLiteral("PRAGMA user_version = 1"));
        step(QStringLiteral("CREATE TABLE IF NOT EXISTS messages ("
                            "id INTEGER PRIMARY KEY, "
                            // NULL is the device's direct-message conversation;
                            // channel conversations use a 32-byte fingerprint.
                            "channel BLOB, "
                            "timestamp INTEGER NOT NULL, "
                            "text TEXT NOT NULL, "
                            "sender TEXT NOT NULL DEFAULT '', "
                            "outgoing INTEGER NOT NULL DEFAULT 0, "
                            "snr REAL, "
                            "path_len INTEGER)"));
        step(QStringLiteral("CREATE INDEX IF NOT EXISTS messages_by_channel "
                            "ON messages(channel, id)"));
    }
    if (!initError.isEmpty()) {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(connectionName);
        return fail(initError);
    }

    connectionNames_.insert(deviceId, connectionName);
    return db;
}

}  // namespace model
