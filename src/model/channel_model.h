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
    };

    explicit ChannelModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setChannels(const QVector<Channel>& channels);
    // Row for a daemon slot number, or -1 if that slot is not configured.
    int rowForIndex(int channelIndex) const;
    int channelIndexForRow(int row) const;

    // The newest message in a channel, reduced to the two things the sidebar
    // draws. Kept here rather than read from History on demand so a row repaint
    // costs no lookup, and stored already-flattened so it costs no formatting
    // either — the delegate runs on every paint, this runs once per message.
    void setLastMessage(int channelIndex, const Message& msg);
    void bumpUnread(int channelIndex);
    void clearUnread(int channelIndex);

private:
    struct LastMessage {
        QDateTime when;
        QString preview;
    };

    QString tooltipFor(const Channel& ch) const;

    QVector<Channel> channels_;
    QHash<int, int> unread_;
    QHash<int, LastMessage> last_;
};

}  // namespace model
