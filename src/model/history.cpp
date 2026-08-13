#include "model/history.h"

#include <QCryptographicHash>
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

QJsonObject toJson(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                   const Message& m) {
    QJsonObject o;
    o["device"] = QString::fromLatin1(deviceId.toHex());
    if (channelKeyFingerprint.isEmpty())
        o["direct"] = true;
    else
        o["channel"] = QString::fromLatin1(channelKeyFingerprint.toHex());
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

bool fromJson(const QJsonObject& o, QByteArray& deviceId, QByteArray& channelKeyFingerprint,
              Message& out) {
    if (!o.contains("device") || !o.contains("text")) return false;
    deviceId = QByteArray::fromHex(o["device"].toString().toLatin1());
    if (deviceId.size() != 32) return false;

    const bool direct = o["direct"].toBool(false);
    channelKeyFingerprint = QByteArray::fromHex(o["channel"].toString().toLatin1());
    if (!direct && channelKeyFingerprint.size() != QCryptographicHash::hashLength(
                                                    QCryptographicHash::Sha256))
        return false;
    if (direct) channelKeyFingerprint.clear();

    // A stored channel has no slot. The caller binds it to the slot used by the
    // current connection; -1 remains the runtime address for direct messages.
    out.channelIndex = direct ? -1 : 0;
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

        QByteArray deviceId;
        QByteArray channelKeyFingerprint;
        Message m;
        if (!fromJson(doc.object(), deviceId, channelKeyFingerprint, m)) continue;
        byDevice_[deviceId][channelKeyFingerprint].append(m);
    }
    f.close();

    bool trimmed = false;
    for (auto device = byDevice_.begin(); device != byDevice_.end(); ++device) {
        for (auto channel = device->begin(); channel != device->end(); ++channel) {
            if (channel->size() > MaxPerChannel) {
                channel->remove(0, channel->size() - MaxPerChannel);
                trimmed = true;
            }
        }
    }
    if (trimmed || linesOnDisk_ > CompactThreshold) compact();
}

QVector<Message> History::messages(const QByteArray& deviceId,
                                   const QByteArray& channelKeyFingerprint,
                                   int channelIndex) const {
    if (deviceId.size() != 32) return {};
    if (channelIndex >= 0 && channelKeyFingerprint.size() != 32) return {};
    if (channelIndex < 0 && !channelKeyFingerprint.isEmpty()) return {};

    const auto device = byDevice_.constFind(deviceId);
    if (device == byDevice_.constEnd()) return {};
    const auto channel = device->constFind(channelKeyFingerprint);
    if (channel == device->constEnd()) return {};

    QVector<Message> result = *channel;
    for (Message& msg : result) msg.channelIndex = channelIndex;
    return result;
}

void History::append(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint,
                     const Message& msg) {
    if (deviceId.size() != 32) return;
    if (msg.channelIndex >= 0 &&
        channelKeyFingerprint.size() !=
            QCryptographicHash::hashLength(QCryptographicHash::Sha256))
        return;
    if (msg.channelIndex < 0 && !channelKeyFingerprint.isEmpty()) return;

    Message stored = msg;
    stored.channelIndex = channelKeyFingerprint.isEmpty() ? -1 : 0;
    QVector<Message>& msgs = byDevice_[deviceId][channelKeyFingerprint];
    msgs.append(stored);
    if (msgs.size() > MaxPerChannel) msgs.remove(0, msgs.size() - MaxPerChannel);

    appendLine(deviceId, channelKeyFingerprint, stored);
    if (linesOnDisk_ > CompactThreshold) compact();
}

void History::remove(const QByteArray& deviceId, const QByteArray& channelKeyFingerprint) {
    if (deviceId.size() != 32 || channelKeyFingerprint.size() != 32) return;
    auto device = byDevice_.find(deviceId);
    if (device == byDevice_.end() || device->remove(channelKeyFingerprint) == 0) return;
    if (device->isEmpty()) byDevice_.erase(device);
    // Dropping lines is a rewrite whatever way it is done, and compact() already
    // writes the file from what is in memory.
    compact();
}

void History::appendLine(const QByteArray& deviceId,
                         const QByteArray& channelKeyFingerprint, const Message& msg) {
    QDir().mkpath(QFileInfo(path_).absolutePath());

    QFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    f.write(QJsonDocument(toJson(deviceId, channelKeyFingerprint, msg))
                .toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
    linesOnDisk_++;
}

void History::compact() {
    // Flat chronological order across devices and channels is not required by
    // the reader, which buckets by both identities anyway.
    QDir().mkpath(QFileInfo(path_).absolutePath());

    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) return;

    int written = 0;
    for (auto device = byDevice_.constBegin(); device != byDevice_.constEnd(); ++device) {
        for (auto channel = device->constBegin(); channel != device->constEnd(); ++channel) {
            for (const Message& m : *channel) {
                f.write(QJsonDocument(toJson(device.key(), channel.key(), m))
                            .toJson(QJsonDocument::Compact));
                f.write("\n");
                written++;
            }
        }
    }
    // QSaveFile replaces atomically, so a crash mid-compaction leaves the old
    // history intact rather than a half-written file.
    if (f.commit()) linesOnDisk_ = written;
}

}  // namespace model
