#include "model/channel_model.h"

#include <QLocale>

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
        case UnreadRole: return unread_.value(ch.index, 0);
        case TypeRole: return int(ch.type);
        case LastActivityRole: return last_.value(ch.index).when;
        case PreviewRole: return last_.value(ch.index).preview;
        case Qt::ToolTipRole: return tooltipFor(ch);
        default: return {};
    }
}

QString ChannelModel::tooltipFor(const Channel& ch) const {
    // The row is icon, name and a time; the tooltip is where what the icon
    // means and the full date live, since neither fits a 210px sidebar.
    QString kind;
    switch (ch.type) {
        case ChannelType::Public: kind = QStringLiteral("public channel"); break;
        case ChannelType::Hashtag: kind = QStringLiteral("hashtag channel"); break;
        case ChannelType::Private: kind = QStringLiteral("private channel"); break;
    }

    QString text = QStringLiteral("%1 — %2 (slot %3)").arg(ch.displayName(), kind).arg(ch.index);
    const QDateTime when = last_.value(ch.index).when;
    if (when.isValid())
        text += QStringLiteral("\nLast message %1")
                    .arg(QLocale().toString(when, QLocale::ShortFormat));
    return text;
}

void ChannelModel::setChannels(const QVector<Channel>& channels) {
    beginResetModel();
    channels_ = channels;
    // Unread counts and activity times are keyed by slot number, so they
    // survive a re-enumeration after a reconnect.
    endResetModel();
}

int ChannelModel::rowForIndex(int channelIndex) const {
    for (int i = 0; i < channels_.size(); i++)
        if (channels_.at(i).index == channelIndex) return i;
    return -1;
}

int ChannelModel::channelIndexForRow(int row) const {
    if (row < 0 || row >= channels_.size()) return -1;
    return channels_.at(row).index;
}

void ChannelModel::setLastMessage(int channelIndex, const Message& msg) {
    // Channel text is free-form and may carry newlines; the preview is one line
    // of a two-line row, so it is flattened here rather than left to elide into
    // a blank second half.
    QString text = msg.text.simplified();
    // Our own name comes from the device and would read as just another speaker;
    // "You" is what the row means.
    const QString who = msg.outgoing ? QStringLiteral("You") : msg.sender;
    last_[channelIndex] = {msg.timestamp, who.isEmpty()
                                              ? text
                                              : QStringLiteral("%1: %2").arg(who, text)};

    const int row = rowForIndex(channelIndex);
    if (row >= 0)
        Q_EMIT dataChanged(index(row), index(row),
                           {LastActivityRole, PreviewRole, Qt::ToolTipRole});
}

void ChannelModel::bumpUnread(int channelIndex) {
    unread_[channelIndex]++;
    const int row = rowForIndex(channelIndex);
    if (row >= 0) Q_EMIT dataChanged(index(row), index(row), {UnreadRole});
}

void ChannelModel::clearUnread(int channelIndex) {
    if (unread_.value(channelIndex, 0) == 0) return;
    unread_[channelIndex] = 0;
    const int row = rowForIndex(channelIndex);
    if (row >= 0) Q_EMIT dataChanged(index(row), index(row), {UnreadRole});
}

}  // namespace model
