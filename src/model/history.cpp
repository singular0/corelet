#include "model/history.h"

#include <QDir>
#include <QSqlDatabase>
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

QVector<Message> History::messages(const QByteArray& deviceId,
                                   const QByteArray& channelKeyFingerprint,
                                   int channelIndex) const {
    if (!validScope(deviceId, channelKeyFingerprint, channelIndex)) return {};

    const QSqlDatabase db = databaseFor(deviceId);
    if (!db.isOpen()) return {};

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT timestamp, text, sender, outgoing, snr, path_len "
            "FROM messages WHERE channel IS ? ORDER BY id DESC LIMIT ?")))
        return {};
    query.addBindValue(storedChannel(channelKeyFingerprint));
    query.addBindValue(MaxPerChannel);
    if (!query.exec()) return {};

    QVector<Message> result;
    result.reserve(MaxPerChannel);
    while (query.next()) result.append(storedMessage(query, channelIndex));

    // The descending index walk makes LIMIT cheap; the view still consumes
    // messages in their original append order.
    std::reverse(result.begin(), result.end());
    return result;
}

std::optional<Message> History::latestMessage(const QByteArray& deviceId,
                                              const QByteArray& channelKeyFingerprint,
                                              int channelIndex) const {
    if (!validScope(deviceId, channelKeyFingerprint, channelIndex)) return std::nullopt;

    const QSqlDatabase db = databaseFor(deviceId);
    if (!db.isOpen()) return std::nullopt;

    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.prepare(QStringLiteral(
            "SELECT timestamp, text, sender, outgoing, snr, path_len "
            "FROM messages WHERE channel IS ? ORDER BY id DESC LIMIT 1")))
        return std::nullopt;
    query.addBindValue(storedChannel(channelKeyFingerprint));
    if (!query.exec() || !query.next()) return std::nullopt;
    return storedMessage(query, channelIndex);
}

void History::append(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                     const Message& msg) {
    if (!validScope(deviceId, channelKeyFingerprint, msg.channelIndex)) return;

    QSqlDatabase db = databaseFor(deviceId);
    if (!db.isOpen() || !db.transaction()) return;

    QSqlQuery insert(db);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO messages "
            "(channel, timestamp, text, sender, outgoing, snr, path_len) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)"))) {
        db.rollback();
        return;
    }
    insert.addBindValue(storedChannel(channelKeyFingerprint));
    insert.addBindValue(msg.timestamp.toSecsSinceEpoch());
    insert.addBindValue(msg.text.isNull() ? QStringLiteral("") : msg.text);
    insert.addBindValue(msg.sender.isNull() ? QStringLiteral("") : msg.sender);
    insert.addBindValue(msg.outgoing);
    insert.addBindValue(msg.hasSignal ? QVariant(double(msg.snr)) : QVariant());
    insert.addBindValue(msg.hasSignal ? QVariant(msg.pathLen) : QVariant());
    if (!insert.exec()) {
        db.rollback();
        return;
    }

    // The channel/id index finds the retention boundary without scanning any
    // other conversation or rewriting the database.
    QSqlQuery trim(db);
    if (!trim.prepare(QStringLiteral(
            "DELETE FROM messages WHERE channel IS ? AND id < COALESCE(("
            "SELECT id FROM messages WHERE channel IS ? "
            "ORDER BY id DESC LIMIT 1 OFFSET ?), 0)"))) {
        db.rollback();
        return;
    }
    trim.addBindValue(storedChannel(channelKeyFingerprint));
    trim.addBindValue(storedChannel(channelKeyFingerprint));
    trim.addBindValue(MaxPerChannel - 1);
    if (!trim.exec() || !db.commit()) db.rollback();
}

void History::remove(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint) {
    if (deviceId.size() != DeviceIdSize ||
        channelKeyFingerprint.size() != ChannelFingerprintSize)
        return;

    const QSqlDatabase db = databaseFor(deviceId);
    if (!db.isOpen()) return;

    QSqlQuery query(db);
    if (!query.prepare(QStringLiteral("DELETE FROM messages WHERE channel IS ?"))) return;
    query.addBindValue(storedChannel(channelKeyFingerprint));
    query.exec();
}

bool History::validScope(const QByteArray& deviceId,
                         const QByteArray& channelKeyFingerprint, int channelIndex) {
    if (deviceId.size() != DeviceIdSize) return false;
    if (channelIndex < 0) return channelKeyFingerprint.isEmpty();
    return channelKeyFingerprint.size() == ChannelFingerprintSize;
}

QSqlDatabase History::databaseFor(const QByteArray& deviceId) const {
    if (deviceId.size() != DeviceIdSize) return {};

    const auto existing = connectionNames_.constFind(deviceId);
    if (existing != connectionNames_.constEnd()) {
        QSqlDatabase db = QSqlDatabase::database(*existing, false);
        if (db.isValid() && (db.isOpen() || db.open())) return db;
        return {};
    }

    if (!QDir().mkpath(directory_)) return {};

    const QString deviceName = QString::fromLatin1(deviceId.toHex());
    const QString connectionName =
        QStringLiteral("corelet-history-%1-%2")
            .arg(quintptr(this), 0, 16)
            .arg(deviceName);
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(QDir(directory_).filePath(deviceName + QStringLiteral(".sqlite3")));
    db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!db.open()) {
        db = {};
        QSqlDatabase::removeDatabase(connectionName);
        return {};
    }

    bool initialized = false;
    {
        QSqlQuery query(db);
        // WAL keeps appends short and leaves readers unblocked if a future UI
        // reads history off the main thread.
        initialized = query.exec(QStringLiteral("PRAGMA journal_mode = WAL")) &&
                      query.exec(QStringLiteral("PRAGMA synchronous = NORMAL")) &&
                      query.exec(QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS messages ("
                          "id INTEGER PRIMARY KEY, "
                          // NULL is the device's direct-message conversation;
                          // channel conversations use a 32-byte fingerprint.
                          "channel BLOB, "
                          "timestamp INTEGER NOT NULL, "
                          "text TEXT NOT NULL, "
                          "sender TEXT NOT NULL DEFAULT '', "
                          "outgoing INTEGER NOT NULL DEFAULT 0, "
                          "snr REAL, "
                          "path_len INTEGER)")) &&
                      query.exec(QStringLiteral(
                          "CREATE INDEX IF NOT EXISTS messages_by_channel "
                          "ON messages(channel, id)"));
    }
    if (!initialized) {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(connectionName);
        return {};
    }

    connectionNames_.insert(deviceId, connectionName);
    return db;
}

}  // namespace model
