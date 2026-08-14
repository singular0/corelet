#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>

#include "model/history.h"

namespace {

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
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
        history.append(deviceA, channelA, message(3, 1));
        model::Message nameless = message(4, 2);
        nameless.sender = QString();
        history.append(deviceA, channelB, nameless);
        history.append(deviceB, channelA, message(7, 3));
        history.append(deviceA, {}, message(-1, 4));

        const QString pathA = directory.filePath(
            QString::fromLatin1(deviceA.toHex()) + QStringLiteral(".sqlite3"));
        const QString pathB = directory.filePath(
            QString::fromLatin1(deviceB.toHex()) + QStringLiteral(".sqlite3"));
        if (!check(QFileInfo::exists(pathA), "device A database exists") ||
            !check(QFileInfo::exists(pathB), "device B database exists") ||
            !check(pathA != pathB, "devices use distinct databases"))
            return 1;

        const QVector<model::Message> a = history.messages(deviceA, channelA, 9);
        if (!check(a.size() == 1, "device A channel A has one message") ||
            !check(a.at(0).channelIndex == 9, "stored message is rebound to current slot") ||
            !check(a.at(0).text == QStringLiteral("message-1"), "message text round trips") ||
            !check(!a.at(0).hasSignal, "absent signal remains absent"))
            return 1;

        const QVector<model::Message> b = history.messages(deviceB, channelA, 7);
        if (!check(b.size() == 1, "device B history is isolated") ||
            !check(b.at(0).text == QStringLiteral("message-3"),
                   "device B message round trips") ||
            !check(b.at(0).hasSignal && b.at(0).pathLen == 2,
                   "signal metadata round trips") ||
            !check(history.messages(deviceA, QByteArray(""), -1).size() == 1,
                   "direct messages use their own scope"))
            return 1;

        for (int i = 0; i <= model::History::MaxPerChannel; ++i)
            history.append(deviceA, channelA, message(3, 1000 + i));

        const QVector<model::Message> retained = history.messages(deviceA, channelA, 3);
        const std::optional<model::Message> latest =
            history.latestMessage(deviceA, channelA, 12);
        if (!check(retained.size() == model::History::MaxPerChannel,
                   "channel history is capped") ||
            !check(retained.first().text == QStringLiteral("message-1001"),
                   "retention drops the oldest messages") ||
            !check(retained.last().text == QStringLiteral("message-1500"),
                   "retention keeps the newest message") ||
            !check(latest.has_value(), "latest message query finds a row") ||
            !check(latest->channelIndex == 12,
                   "latest message is rebound to the current slot") ||
            !check(latest->text == QStringLiteral("message-1500"),
                   "latest message query returns the newest row"))
            return 1;
    }

    {
        model::History history(directory.path());
        if (!check(history.messages(deviceA, channelA, 3).size() ==
                       model::History::MaxPerChannel,
                   "history survives reopening"))
            return 1;

        history.remove(deviceA, channelA);
        if (!check(history.messages(deviceA, channelA, 3).isEmpty(),
                   "channel removal deletes its messages") ||
            !check(history.messages(deviceA, channelB, 4).size() == 1,
                   "channel removal leaves sibling channels intact") ||
            !check(history.messages(deviceB, channelA, 7).size() == 1,
                   "channel removal leaves other devices intact") ||
            !check(history.messages(deviceA, {}, -1).size() == 1,
                   "channel removal leaves direct messages intact"))
            return 1;
    }

    return 0;
}
