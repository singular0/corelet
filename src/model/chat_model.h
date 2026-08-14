#pragma once

#include <QAbstractListModel>

#include "model/types.h"

namespace model {

// Messages of the currently selected channel, as a list model so the view
// recycles delegates and only repaints what changed. Appending is the common
// case and costs one row insert, not a full reset.
class ChatModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        SenderRole = Qt::UserRole + 1,
        TextRole,
        TimestampRole,
        OutgoingRole,
        SnrRole,
        HasSignalRole,
        PathLenRole,
        SendStateRole,
        // True when this row starts a new calendar day, so the delegate can
        // draw a date separator without re-deriving it from its neighbours.
        DayBreakRole,
        // True for the first message that was unseen when this conversation
        // opened. The marker remains in this view after the sidebar count is
        // cleared, but is not persisted as message data.
        UnseenBreakRole,
    };

    explicit ChatModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setMessages(const QVector<Message>& messages, int unseenCount = 0);
    // `unseen` starts a boundary when a received message lands while the open
    // view is scrolled back. Later rows remain below that same boundary.
    void append(const Message& msg, bool unseen = false);
    const Message& at(int row) const { return messages_.at(row); }
    int firstUnseenRow() const { return unseenBreakRow_; }

    // A message echoed optimistically is the only row that changes after it is
    // inserted: the daemon either takes it, or it never happened and the row
    // goes away again. Anything that reloads the conversation -- a channel
    // switch, a reconnect re-enumerating the channels -- drops that row, since
    // a send still in flight is not in history yet, so markSent() reports
    // whether it found one and the caller can put the message back.
    bool markSent(int sendToken);
    void removePending(int sendToken);

private:
    int rowForToken(int sendToken) const;

    QVector<Message> messages_;
    int unseenBreakRow_ = -1;
};

}  // namespace model
