#include <QCoreApplication>

#include "model/chat_model.h"

namespace {

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

model::Message message(int day, int sequence) {
    model::Message msg;
    msg.text = QStringLiteral("message-%1").arg(sequence);
    msg.timestamp = QDateTime(QDate(2026, 8, day), QTime(12, sequence));
    return msg;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    model::ChatModel model;

    const QVector<model::Message> messages {
        message(13, 0), message(13, 1), message(14, 2), message(14, 3)};
    model.setMessages(messages, 2);

    if (!check(model.firstUnseenRow() == 2, "unseen boundary precedes unseen suffix") ||
        !check(!model.data(model.index(1), model::ChatModel::UnseenBreakRole).toBool(),
               "seen row has no unseen boundary") ||
        !check(model.data(model.index(2), model::ChatModel::UnseenBreakRole).toBool(),
               "first unseen row carries boundary") ||
        !check(model.data(model.index(2), model::ChatModel::DayBreakRole).toBool(),
               "day and unseen boundaries can coincide") ||
        !check(!model.data(model.index(3), model::ChatModel::UnseenBreakRole).toBool(),
               "later unseen row has no duplicate boundary"))
        return 1;

    model.append(message(14, 4));
    if (!check(model.firstUnseenRow() == 2, "append preserves existing unseen boundary") ||
        !check(!model.data(model.index(4), model::ChatModel::UnseenBreakRole).toBool(),
               "new append does not move the boundary"))
        return 1;

    model.setMessages(messages, 99);
    if (!check(model.firstUnseenRow() == 0, "unseen count is bounded by loaded history") ||
        !check(model.data(model.index(0), model::ChatModel::UnseenBreakRole).toBool(),
               "bounded boundary appears before first retained row"))
        return 1;

    model.setMessages(messages);
    if (!check(model.firstUnseenRow() == -1, "reload without unseen messages clears boundary"))
        return 1;

    model.append(message(14, 4), true);
    model.append(message(14, 5), true);
    if (!check(model.firstUnseenRow() == 4,
               "first off-screen append starts one unseen boundary") ||
        !check(model.data(model.index(4), model::ChatModel::UnseenBreakRole).toBool(),
               "off-screen append carries unseen boundary") ||
        !check(!model.data(model.index(5), model::ChatModel::UnseenBreakRole).toBool(),
               "later off-screen append does not duplicate boundary"))
        return 1;

    return 0;
}
