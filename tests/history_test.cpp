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
    const QByteArray channelA(32, '\x33');
    const QByteArray channelB(32, '\x44');

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
        history.append(deviceA, {}, message(-1, 4));

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
            !check(history.messages(deviceA, QByteArray(""), -1).messages.size() == 1,
                   "direct messages use their own scope"))
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

        for (int i = 0; i <= model::History::MaxPerChannel; ++i)
            history.append(deviceA, channelA, message(3, 1000 + i));

        const QVector<model::Message> retained =
            history.messages(deviceA, channelA, 3).messages;
        const model::HistoryLatest latest = history.latestMessage(deviceA, channelA, 12);
        if (!check(retained.size() == model::History::MaxPerChannel,
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
                       model::History::MaxPerChannel,
                   "history survives reopening"))
            return 1;

        if (!check(bool(history.remove(deviceA, channelA)), "removal reports success") ||
            !check(history.messages(deviceA, channelA, 3).messages.isEmpty(),
                   "channel removal deletes its messages") ||
            !check(history.messages(deviceA, channelB, 4).messages.size() == 1,
                   "channel removal leaves sibling channels intact") ||
            !check(history.messages(deviceB, channelA, 7).messages.size() == 1,
                   "channel removal leaves other devices intact") ||
            !check(history.messages(deviceA, {}, -1).messages.size() == 1,
                   "channel removal leaves direct messages intact"))
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
        if (!check(!opened && !opened.error.isEmpty(), "preflight reports why it failed") ||
            !check(!appended && !appended.error.isEmpty(), "append reports why it failed") ||
            !check(!read.result && read.messages.isEmpty(), "a failed read is not an empty one") ||
            !check(!newest.result && !newest.message,
                   "a failed latest lookup is not an absent row") ||
            !check(!removed && !removed.error.isEmpty(), "removal reports why it failed"))
            return 1;
    }

    return 0;
}
