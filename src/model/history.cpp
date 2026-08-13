#include "model/history.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

namespace model {

namespace {

// Compact once the file has grown to a few times what we would keep in memory.
// Doing it on every launch would be pointless work; never doing it lets the
// file grow without bound on a device with a small SD card.
constexpr int CompactThreshold = History::MaxPerChannel * 8;

QJsonObject toJson(const Message& m) {
    QJsonObject o;
    o["ch"] = m.channelIndex;
    o["ts"] = qint64(m.timestamp.toSecsSinceEpoch());
    o["text"] = m.text;
    if (!m.sender.isEmpty()) o["from"] = m.sender;
    if (m.outgoing) o["out"] = true;
    if (m.hasSignal) {
        o["snr"] = double(m.snr);
        o["path"] = m.pathLen;
    }
    return o;
}

bool fromJson(const QJsonObject& o, Message& out) {
    if (!o.contains("ch") || !o.contains("text")) return false;
    out.channelIndex = o["ch"].toInt();
    out.text = o["text"].toString();
    out.sender = o["from"].toString();
    out.outgoing = o["out"].toBool(false);
    out.timestamp = QDateTime::fromSecsSinceEpoch(qint64(o["ts"].toDouble()));
    if (o.contains("snr")) {
        out.hasSignal = true;
        out.snr = float(o["snr"].toDouble());
        out.pathLen = o["path"].toInt(0xFF);
    }
    return true;
}

}  // namespace

History::History(QString path) : path_(std::move(path)) {}

void History::load() {
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;  // first run

    QTextStream in(&f);
    while (!in.atEnd()) {
        const QByteArray line = in.readLine().toUtf8();
        if (line.isEmpty()) continue;
        linesOnDisk_++;

        QJsonParseError err {};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;

        Message m;
        if (!fromJson(doc.object(), m)) continue;
        byChannel_[m.channelIndex].append(m);
    }
    f.close();

    bool trimmed = false;
    for (auto it = byChannel_.begin(); it != byChannel_.end(); ++it) {
        if (it->size() > MaxPerChannel) {
            it->remove(0, it->size() - MaxPerChannel);
            trimmed = true;
        }
    }
    if (trimmed || linesOnDisk_ > CompactThreshold) compact();
}

const QVector<Message>& History::messages(int channelIndex) const {
    static const QVector<Message> empty;
    auto it = byChannel_.constFind(channelIndex);
    return it == byChannel_.constEnd() ? empty : *it;
}

void History::append(const Message& msg) {
    QVector<Message>& msgs = byChannel_[msg.channelIndex];
    msgs.append(msg);
    if (msgs.size() > MaxPerChannel) msgs.remove(0, msgs.size() - MaxPerChannel);

    appendLine(msg);
    if (linesOnDisk_ > CompactThreshold) compact();
}

void History::appendLine(const Message& msg) {
    QDir().mkpath(QFileInfo(path_).absolutePath());

    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    f.write(QJsonDocument(toJson(msg)).toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
    linesOnDisk_++;
}

void History::compact() {
    // Flat chronological order across channels is not required by the reader,
    // which buckets by "ch" anyway, so grouping per channel is fine and avoids
    // a merge sort over the whole log.
    QDir().mkpath(QFileInfo(path_).absolutePath());

    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) return;

    int written = 0;
    for (auto it = byChannel_.constBegin(); it != byChannel_.constEnd(); ++it) {
        for (const Message& m : *it) {
            f.write(QJsonDocument(toJson(m)).toJson(QJsonDocument::Compact));
            f.write("\n");
            written++;
        }
    }
    // QSaveFile replaces atomically, so a crash mid-compaction leaves the old
    // history intact rather than a half-written file.
    if (f.commit()) linesOnDisk_ = written;
}

}  // namespace model
