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

// One of our own, still on its way: the only kind of row whose state moves
// after it is inserted.
model::Message pendingSend(const model::Conversation& conversation, int token) {
    model::Message msg = message(14, token);
    msg.conversation = conversation;
    msg.outgoing = true;
    msg.sendState = model::Message::SendState::Pending;
    msg.sendToken = token;
    return msg;
}

int sendState(const model::ChatModel& model, int row) {
    return model.data(model.index(row), model::ChatModel::SendStateRole).toInt();
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

    // Send state. A channel message is finished the moment the daemon takes it;
    // a direct one is answered for by its peer, or is not, and either answer
    // arrives long after the row went up.
    const model::Conversation channel =
        model::Conversation::channel(QByteArray(model::Conversation::IdSize, 'c'));
    const model::Conversation peer =
        model::Conversation::direct(QByteArray::fromHex("010203040506"));

    model.setMessages({});
    model.append(pendingSend(channel, 1));
    model.append(pendingSend(peer, 2));

    if (!check(model.setSendState(1, model::Message::SendState::Sent),
               "the daemon taking a channel send finds its row") ||
        !check(sendState(model, 0) == int(model::Message::SendState::Sent),
               "the channel row shows the send as taken") ||
        !check(!model.setSendState(1, model::Message::SendState::Delivered),
               "a channel send is finished by that and answers to nothing more"))
        return 1;

    if (!check(model.setSendState(2, model::Message::SendState::Sent),
               "the daemon taking a direct send finds its row") ||
        !check(model.setSendState(2, model::Message::SendState::Unconfirmed),
               "a direct send is still addressable once its window has passed") ||
        !check(sendState(model, 1) == int(model::Message::SendState::Unconfirmed),
               "the direct row shows the wait as run out") ||
        !check(model.setSendState(2, model::Message::SendState::Delivered),
               "a late ack still reaches the row it lapsed on") ||
        !check(sendState(model, 1) == int(model::Message::SendState::Delivered),
               "the direct row shows the peer's confirmation") ||
        !check(!model.setSendState(2, model::Message::SendState::Unconfirmed),
               "a confirmed send answers to nothing more"))
        return 1;

    // A send the daemon refused never happened, and neither did its row.
    model.append(pendingSend(peer, 3));
    model.removePending(3);
    if (!check(model.rowCount() == 2, "a refused send takes its row with it") ||
        !check(!model.setSendState(3, model::Message::SendState::Sent),
               "a removed row is no longer addressable"))
        return 1;

    return 0;
}
