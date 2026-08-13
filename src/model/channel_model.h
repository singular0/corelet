#pragma once

#include <QAbstractListModel>
#include <QHash>

#include "model/types.h"

namespace model {

// The channel list on the left. Unread counts live here rather than in the
// window so the sidebar repaints one row when a message lands in a channel the
// user is not looking at.
class ChannelModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        IndexRole,  // the daemon's slot number, not the row
        UnreadRole,
        TypeRole,          // model::ChannelType
        LastActivityRole,  // QDateTime of the newest message, invalid if none
        PreviewRole,       // "sender: text" for that message, empty if none
        KeyFingerprintRole,
    };

    explicit ChannelModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setChannels(const QVector<Channel>& channels);
    // Called when the window leaves one device identity. Public and hashtag
    // keys can exist on many devices, so even key-based UI state must not cross
    // the outer device boundary.
    void clearTransientState();
    // Row for a daemon slot number, or -1 if that slot is not configured.
    int rowForIndex(int channelIndex) const;
    // Row for the key-derived persistent identity, independent of its slot.
    int rowForKey(const QByteArray& keyFingerprint) const;
    int channelIndexForRow(int row) const;
    QByteArray keyForIndex(int channelIndex) const;

    // The newest message in a channel, reduced to the two things the sidebar
    // draws. Kept here rather than read from History on demand so a row repaint
    // costs no lookup, and stored already-flattened so it costs no formatting
    // either — the delegate runs on every paint, this runs once per message.
    void setLastMessage(int channelIndex, const Message& msg);
    void bumpUnread(int channelIndex);
    void clearUnread(int channelIndex);
    // Drops the transient state held for a channel that has been removed.
    void forget(const QByteArray& keyFingerprint);

private:
    struct LastMessage {
        QDateTime when;
        QString preview;
    };

    QVector<Channel> channels_;
    QHash<QByteArray, int> unread_;
    QHash<QByteArray, LastMessage> last_;
};

}  // namespace model
