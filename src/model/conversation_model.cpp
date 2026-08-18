#include "model/conversation_model.h"

#include <algorithm>

namespace model {

ConversationModel::ConversationModel(QObject* parent) : QAbstractListModel(parent) {}

int ConversationModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(rows_.size());
}

QVariant ConversationModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const ConversationEntry& row = rows_.at(index.row());

    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return row.name;
        case KindRole: return int(row.conversation.kind);
        case ChannelTypeRole: return int(row.channelType);
        case IndexRole: return row.channelIndex;
        case UnreadRole: return unread_.value(row.conversation, 0);
        case LastActivityRole: return last_.value(row.conversation).when;
        case PreviewRole: return last_.value(row.conversation).preview;
        case ConversationRole: return QVariant::fromValue(row.conversation);
        default: return {};
    }
}

bool ConversationModel::sortsBefore(const ConversationEntry& a, const ConversationEntry& b) {
    // Two peers may well advertise the same name -- nothing stops them, and an
    // unnamed one is named after its key -- so the identity breaks the tie and
    // the order stays put from one enumeration to the next.
    const QString ka = a.name.toCaseFolded();
    const QString kb = b.name.toCaseFolded();
    return ka == kb ? a.conversation.id < b.conversation.id : ka < kb;
}

void ConversationModel::rebuild() {
    beginResetModel();
    // Channels first and in slot order, because that list is fixed and short
    // and the reader learns where each one is. Peers follow by name rather than
    // by when they last said something: a list that reorders itself under the
    // pointer is worse to use than one that is a row out of date, especially on
    // a trackball.
    std::sort(directs_.begin(), directs_.end(), sortsBefore);
    rows_ = channels_;
    rows_.append(directs_);
    // Unread counts and activity times are keyed by the conversation, so they
    // survive a reconnect and a channel moving to another device slot.
    endResetModel();
}

void ConversationModel::setChannels(const QVector<Channel>& channels) {
    channels_.clear();
    channels_.reserve(channels.size());
    for (const Channel& channel : channels) {
        const QByteArray fingerprint = channel.keyFingerprint();
        if (fingerprint.isEmpty()) continue;
        channels_.append({Conversation::channel(fingerprint), channel.displayName(),
                          channel.index, channel.type});
    }
    rebuild();
}

void ConversationModel::setDirectConversations(const QVector<ConversationEntry>& peers) {
    directs_.clear();
    directs_.reserve(peers.size());
    for (const ConversationEntry& peer : peers)
        if (peer.conversation.isDirect() && peer.conversation.isValid())
            directs_.append(peer);
    rebuild();
}

void ConversationModel::upsertDirect(const ConversationEntry& peer) {
    if (!peer.conversation.isDirect() || !peer.conversation.isValid()) return;

    const auto existing = std::find_if(
        directs_.begin(), directs_.end(), [&](const ConversationEntry& entry) {
            return entry.conversation == peer.conversation;
        });
    if (existing != directs_.end()) {
        if (existing->name == peer.name) return;
        const int at = int(existing - directs_.begin());
        *existing = peer;
        // A rename can move the row. Only one that keeps its place is a
        // repaint; anything else has to go back through the ordering.
        const bool inPlace =
            (at == 0 || sortsBefore(directs_.at(at - 1), peer)) &&
            (at + 1 >= int(directs_.size()) || sortsBefore(peer, directs_.at(at + 1)));
        if (!inPlace) {
            rebuild();
            return;
        }
        const int row = int(channels_.size()) + at;
        rows_[row] = peer;
        Q_EMIT dataChanged(index(row), index(row), {Qt::DisplayRole, NameRole});
        return;
    }

    // A peer nobody has heard from before. Inserting it where it belongs keeps
    // the reader's place in the list and whatever is selected selected, which a
    // reset would not.
    const auto at = std::lower_bound(directs_.begin(), directs_.end(), peer, sortsBefore);
    const int row = int(channels_.size() + (at - directs_.begin()));
    beginInsertRows({}, row, row);
    directs_.insert(at, peer);
    rows_.insert(row, peer);
    endInsertRows();
}

void ConversationModel::clearTransientState() {
    unread_.clear();
    last_.clear();
}

int ConversationModel::rowFor(const Conversation& conversation) const {
    if (!conversation.isValid()) return -1;
    for (int i = 0; i < rows_.size(); i++)
        if (rows_.at(i).conversation == conversation) return i;
    return -1;
}

Conversation ConversationModel::conversationAt(int row) const {
    if (row < 0 || row >= rows_.size()) return {};
    return rows_.at(row).conversation;
}

const ConversationEntry* ConversationModel::entry(const Conversation& conversation) const {
    const int row = rowFor(conversation);
    return row < 0 ? nullptr : &rows_.at(row);
}

Conversation ConversationModel::channelAt(int channelIndex) const {
    if (channelIndex < 0) return {};
    for (const ConversationEntry& row : channels_)
        if (row.channelIndex == channelIndex) return row.conversation;
    return {};
}

void ConversationModel::rowChanged(const Conversation& conversation, const QList<int>& roles) {
    const int row = rowFor(conversation);
    if (row >= 0) Q_EMIT dataChanged(index(row), index(row), roles);
}

void ConversationModel::setLastMessage(const Conversation& conversation, const Message& msg) {
    if (!conversation.isValid()) return;
    // Message text is free-form and may carry newlines; the preview is one line
    // of a two-line row, so it is flattened here rather than left to elide into
    // a blank second half.
    QString text = msg.text.simplified();
    // Our own name comes from the device and would read as just another
    // speaker; "You" is what the row means. A peer's name is the row's own
    // title, so a direct message does not repeat it.
    const QString who = msg.outgoing            ? QStringLiteral("You")
                        : conversation.isDirect() ? QString()
                                                : msg.sender;
    last_[conversation] = {msg.timestamp, who.isEmpty()
                                              ? text
                                              : QStringLiteral("%1: %2").arg(who, text)};
    rowChanged(conversation, {LastActivityRole, PreviewRole});
}

void ConversationModel::bumpUnread(const Conversation& conversation) {
    if (!conversation.isValid()) return;
    unread_[conversation]++;
    rowChanged(conversation, {UnreadRole});
}

int ConversationModel::unreadCount(const Conversation& conversation) const {
    return conversation.isValid() ? unread_.value(conversation, 0) : 0;
}

void ConversationModel::clearUnread(const Conversation& conversation) {
    if (!conversation.isValid() || unread_.value(conversation, 0) == 0) return;
    unread_[conversation] = 0;
    rowChanged(conversation, {UnreadRole});
}

void ConversationModel::forget(const Conversation& conversation) {
    unread_.remove(conversation);
    last_.remove(conversation);
    // A channel has already left the list by the time this runs -- the device
    // re-enumerated without it. A peer has not: nothing but this list says the
    // conversation existed, so removing it is removing the row.
    const auto at = std::find_if(directs_.begin(), directs_.end(),
                                 [&](const ConversationEntry& entry) {
                                     return entry.conversation == conversation;
                                 });
    if (at == directs_.end()) return;
    const int row = rowFor(conversation);
    beginRemoveRows({}, row, row);
    directs_.erase(at);
    rows_.remove(row);
    endRemoveRows();
}

}  // namespace model
