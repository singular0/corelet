#include "model/chat_model.h"

namespace model {

ChatModel::ChatModel(QObject* parent) : QAbstractListModel(parent) {}

int ChatModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(messages_.size());
}

QVariant ChatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= messages_.size()) return {};
    const Message& m = messages_.at(index.row());

    switch (role) {
        case SenderRole: return m.sender;
        case TextRole: return m.text;
        case TimestampRole: return m.timestamp;
        case OutgoingRole: return m.outgoing;
        case SnrRole: return m.snr;
        case HasSignalRole: return m.hasSignal;
        case PathLenRole: return m.pathLen;
        case SendStateRole: return int(m.sendState);
        case DayBreakRole:
            if (index.row() == 0) return true;
            return messages_.at(index.row() - 1).timestamp.date() != m.timestamp.date();
        case UnseenBreakRole: return index.row() == unseenBreakRow_;
        default: return {};
    }
}

void ChatModel::setMessages(const QVector<Message>& messages, int unseenCount) {
    beginResetModel();
    messages_ = messages;
    const int boundedCount = qBound(0, unseenCount, int(messages_.size()));
    unseenBreakRow_ = boundedCount == 0 ? -1 : int(messages_.size()) - boundedCount;
    endResetModel();
}

void ChatModel::append(const Message& msg, bool unseen) {
    const int row = int(messages_.size());
    beginInsertRows({}, row, row);
    messages_.append(msg);
    if (unseen && unseenBreakRow_ < 0) unseenBreakRow_ = row;
    endInsertRows();
}

// Searched from the end: a send in flight is one of the newest rows, and on the
// common path the first row looked at is the one wanted.
int ChatModel::rowForToken(int sendToken) const {
    if (sendToken == 0) return -1;
    for (int row = int(messages_.size()) - 1; row >= 0; row--)
        if (messages_.at(row).sendToken == sendToken) return row;
    return -1;
}

bool ChatModel::markSent(int sendToken) {
    const int row = rowForToken(sendToken);
    if (row < 0) return false;

    messages_[row].sendState = Message::SendState::Sent;
    messages_[row].sendToken = 0;
    // The mark is the same width in either state, so this repaints the row
    // without asking the view to lay it out again.
    Q_EMIT dataChanged(index(row), index(row), {SendStateRole});
    return true;
}

void ChatModel::removePending(int sendToken) {
    const int row = rowForToken(sendToken);
    if (row < 0) return;

    beginRemoveRows({}, row, row);
    messages_.remove(row);
    endRemoveRows();
}

}  // namespace model
