#pragma once

#include <QAbstractListModel>
#include <QHash>

#include "model/types.h"

namespace model {

// One row of the sidebar. A channel and a conversation with one peer are drawn
// the same way and behave the same way once open; what differs is where the
// name comes from and what stands in the row's disc.
struct ConversationEntry {
    Conversation conversation;
    QString name;
    // Channels only: the wire slot a send is addressed to, and which of the
    // three kinds of key the row's icon should say this is.
    int channelIndex = -1;
    ChannelType channelType = ChannelType::Private;
};

// The conversation list on the left: the device's channels, then the peers
// somebody has direct messages with. Unread counts live here rather than in the
// window so the sidebar repaints one row when a message lands in a conversation
// the user is not looking at.
class ConversationModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        KindRole,         // model::ConversationKind
        ChannelTypeRole,  // model::ChannelType; meaningless for a direct row
        IndexRole,        // the daemon's slot number, not the row; -1 for a peer
        UnreadRole,
        LastActivityRole,  // QDateTime of the newest message, invalid if none
        PreviewRole,       // "sender: text" for that message, empty if none
        ConversationRole,  // the identity, as a QVariant of model::Conversation
    };

    explicit ConversationModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    // The channels, which the device enumerates in one answer, so they are
    // replaced whole. Direct conversations are left where they are: they belong
    // to the same device and nothing about the channel list says anything about
    // them.
    void setChannels(const QVector<Channel>& channels);
    // The peers, likewise replaced whole -- from what history holds for this
    // device, plus whatever names the address book or the offline cache can put
    // to them.
    void setDirectConversations(const QVector<ConversationEntry>& peers);
    // One peer, added or renamed in place. A message from somebody new must not
    // cost the list a reset: the reader may be scrolled down it, or reading the
    // conversation that would lose its selection.
    void upsertDirect(const ConversationEntry& peer);
    // Called when the window leaves one device identity. Public and hashtag
    // keys can exist on many devices, so even key-based UI state must not cross
    // the outer device boundary.
    void clearTransientState();

    int rowFor(const Conversation& conversation) const;
    Conversation conversationAt(int row) const;
    // The whole row, or null when this conversation is not in the list. Valid
    // only until the list next changes.
    const ConversationEntry* entry(const Conversation& conversation) const;
    // The conversation a daemon slot number currently names, or an invalid one
    // when nothing configured is in that slot.
    Conversation channelAt(int channelIndex) const;

    // The newest message in a conversation, reduced to the two things the
    // sidebar draws. Kept here rather than read from History on demand so a row
    // repaint costs no lookup, and stored already-flattened so it costs no
    // formatting either — the delegate runs on every paint, this runs once per
    // message.
    void setLastMessage(const Conversation& conversation, const Message& msg);
    void bumpUnread(const Conversation& conversation);
    int unreadCount(const Conversation& conversation) const;
    void clearUnread(const Conversation& conversation);
    // Drops the transient state held for a conversation that has been removed,
    // and the row itself when it is a peer -- unlike a channel, nothing else
    // enumerates those.
    void forget(const Conversation& conversation);

private:
    struct LastMessage {
        QDateTime when;
        QString preview;
    };

    // Rebuilds the visible order from the two groups. Channels keep the order
    // the device gave them, which is by slot; peers follow, by name.
    void rebuild();
    static bool sortsBefore(const ConversationEntry& a, const ConversationEntry& b);
    void rowChanged(const Conversation& conversation, const QList<int>& roles);

    QVector<ConversationEntry> channels_;
    QVector<ConversationEntry> directs_;
    QVector<ConversationEntry> rows_;
    QHash<Conversation, int> unread_;
    QHash<Conversation, LastMessage> last_;
};

}  // namespace model
