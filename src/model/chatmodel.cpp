#include "model/chatmodel.h"

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
        case DayBreakRole:
            if (index.row() == 0) return true;
            return messages_.at(index.row() - 1).timestamp.date() != m.timestamp.date();
        default: return {};
    }
}

void ChatModel::setMessages(const QVector<Message>& messages) {
    beginResetModel();
    messages_ = messages;
    endResetModel();
}

void ChatModel::append(const Message& msg) {
    const int row = int(messages_.size());
    beginInsertRows({}, row, row);
    messages_.append(msg);
    endInsertRows();
}

}  // namespace model
