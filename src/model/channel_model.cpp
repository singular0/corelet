#include "model/channel_model.h"

namespace model {

ChannelModel::ChannelModel(QObject* parent) : QAbstractListModel(parent) {}

int ChannelModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(channels_.size());
}

QVariant ChannelModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= channels_.size()) return {};
    const Channel& ch = channels_.at(index.row());

    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return ch.displayName();
        case IndexRole: return ch.index;
        case UnreadRole: return unread_.value(ch.keyFingerprint(), 0);
        case TypeRole: return int(ch.type);
        case LastActivityRole: return last_.value(ch.keyFingerprint()).when;
        case PreviewRole: return last_.value(ch.keyFingerprint()).preview;
        case KeyFingerprintRole: return ch.keyFingerprint();
        default: return {};
    }
}

void ChannelModel::setChannels(const QVector<Channel>& channels) {
    beginResetModel();
    channels_ = channels;
    // Unread counts and activity times are keyed by the channel key, so they
    // survive both a reconnect and a channel moving to another device slot.
    endResetModel();
}

void ChannelModel::clearTransientState() {
    unread_.clear();
    last_.clear();
}

int ChannelModel::rowForIndex(int channelIndex) const {
    for (int i = 0; i < channels_.size(); i++)
        if (channels_.at(i).index == channelIndex) return i;
    return -1;
}

int ChannelModel::rowForKey(const QByteArray& keyFingerprint) const {
    if (keyFingerprint.isEmpty()) return -1;
    for (int i = 0; i < channels_.size(); i++)
        if (channels_.at(i).keyFingerprint() == keyFingerprint) return i;
    return -1;
}

int ChannelModel::channelIndexForRow(int row) const {
    if (row < 0 || row >= channels_.size()) return -1;
    return channels_.at(row).index;
}

QByteArray ChannelModel::keyForIndex(int channelIndex) const {
    const int row = rowForIndex(channelIndex);
    return row < 0 ? QByteArray() : channels_.at(row).keyFingerprint();
}

void ChannelModel::setLastMessage(int channelIndex, const Message& msg) {
    const QByteArray key = keyForIndex(channelIndex);
    if (key.isEmpty()) return;
    // Channel text is free-form and may carry newlines; the preview is one line
    // of a two-line row, so it is flattened here rather than left to elide into
    // a blank second half.
    QString text = msg.text.simplified();
    // Our own name comes from the device and would read as just another speaker;
    // "You" is what the row means.
    const QString who = msg.outgoing ? QStringLiteral("You") : msg.sender;
    last_[key] = {msg.timestamp, who.isEmpty()
                                     ? text
                                     : QStringLiteral("%1: %2").arg(who, text)};

    const int row = rowForIndex(channelIndex);
    if (row >= 0)
        Q_EMIT dataChanged(index(row), index(row), {LastActivityRole, PreviewRole});
}

void ChannelModel::bumpUnread(int channelIndex) {
    const QByteArray key = keyForIndex(channelIndex);
    if (key.isEmpty()) return;
    unread_[key]++;
    const int row = rowForIndex(channelIndex);
    if (row >= 0) Q_EMIT dataChanged(index(row), index(row), {UnreadRole});
}

int ChannelModel::unreadCount(int channelIndex) const {
    const QByteArray key = keyForIndex(channelIndex);
    return key.isEmpty() ? 0 : unread_.value(key, 0);
}

void ChannelModel::forget(const QByteArray& keyFingerprint) {
    // No dataChanged: this is called after the channel has left the list.
    unread_.remove(keyFingerprint);
    last_.remove(keyFingerprint);
}

void ChannelModel::clearUnread(int channelIndex) {
    const QByteArray key = keyForIndex(channelIndex);
    if (key.isEmpty() || unread_.value(key, 0) == 0) return;
    unread_[key] = 0;
    const int row = rowForIndex(channelIndex);
    if (row >= 0) Q_EMIT dataChanged(index(row), index(row), {UnreadRole});
}

}  // namespace model
