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
        // True when this row starts a new calendar day, so the delegate can
        // draw a date separator without re-deriving it from its neighbours.
        DayBreakRole,
    };

    explicit ChatModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setMessages(const QVector<Message>& messages);
    void append(const Message& msg);
    const Message& at(int row) const { return messages_.at(row); }

private:
    QVector<Message> messages_;
};

}  // namespace model
