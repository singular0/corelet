#include <QCoreApplication>

#include "model/conversation_model.h"

namespace {

// Counts one of the model's signals for as long as it is alive. QSignalSpy
// would do this, but it lives in Qt6::Test and what these checks are about is
// which signal a change costs -- a reset repaints the whole sidebar and drops
// the reader's place in it -- rather than how that is measured.
class Counter : public QObject {
public:
    template <typename Signal>
    Counter(QAbstractItemModel* model, Signal signal) {
        connect(model, signal, this, [this] { count_++; });
    }

    int count() const { return count_; }

private:
    int count_ = 0;
};

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

model::Channel channel(int slot, const QString& name, char key) {
    model::Channel ch;
    ch.index = slot;
    ch.name = name;
    ch.secret = QByteArray(model::ChannelSecretSize, key);
    ch.type = model::ChannelType::Private;
    return ch;
}

model::Conversation peer(char key) {
    return model::Conversation::direct(QByteArray(model::Conversation::IdSize, key));
}

model::Message message(const QString& sender, const QString& text) {
    model::Message msg;
    msg.sender = sender;
    msg.text = text;
    msg.timestamp = QDateTime::fromSecsSinceEpoch(1'700'000'000);
    return msg;
}

QString nameAt(const model::ConversationModel& model, int row) {
    return model.data(model.index(row), model::ConversationModel::NameRole).toString();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    model::ConversationModel model;

    const model::Channel general = channel(0, QStringLiteral("General"), '\x11');
    const model::Channel ops = channel(3, QStringLiteral("Ops"), '\x22');
    const model::Conversation zoe = peer('\x01');
    const model::Conversation adam = peer('\x02');

    model.setChannels({general, ops});
    model.setDirectConversations({{zoe, QStringLiteral("Zoe")}, {adam, QStringLiteral("Adam")}});

    // Channels keep the order the device gave them; peers follow, by name, so
    // the list a reader has learned does not reshuffle itself.
    if (!check(model.rowCount() == 4, "every conversation is a row") ||
        !check(nameAt(model, 0) == QStringLiteral("General"), "channels come first") ||
        !check(nameAt(model, 1) == QStringLiteral("Ops"), "channels keep their slot order") ||
        !check(nameAt(model, 2) == QStringLiteral("Adam"), "peers are sorted by name") ||
        !check(nameAt(model, 3) == QStringLiteral("Zoe"), "and follow the channels"))
        return 1;

    const model::Conversation generalConversation =
        model.channelAt(general.index);
    if (!check(generalConversation == model.conversationAt(0),
               "a slot number finds the conversation currently in it") ||
        !check(!model.channelAt(7).isValid(), "an empty slot names no conversation") ||
        !check(model.rowFor(adam) == 2, "a peer is found by identity") ||
        !check(model.entry(model.conversationAt(1))->channelIndex == ops.index,
               "a channel row carries the wire slot a send is addressed to") ||
        !check(model.entry(adam)->channelIndex == -1, "a peer has no slot"))
        return 1;

    // A message from somebody new must not reset the list: the reader may be
    // scrolled down it, or reading the conversation that would lose its
    // selection.
    const model::Conversation mia = peer('\x03');
    Counter resets(&model, &QAbstractItemModel::modelReset);
    Counter inserts(&model, &QAbstractItemModel::rowsInserted);
    model.upsertDirect({mia, QStringLiteral("Mia")});
    if (!check(resets.count() == 0, "a new peer costs no reset") ||
        !check(inserts.count() == 1, "a new peer is one row insert") ||
        !check(nameAt(model, 3) == QStringLiteral("Mia"), "and lands where its name belongs"))
        return 1;

    // A rename that keeps the row's place is a repaint; one that moves it goes
    // back through the ordering.
    Counter changes(&model, &QAbstractItemModel::dataChanged);
    model.upsertDirect({mia, QStringLiteral("Mica")});
    if (!check(resets.count() == 0, "a rename that stays put costs no reset") ||
        !check(changes.count() == 1, "and is one repaint") ||
        !check(nameAt(model, 3) == QStringLiteral("Mica"), "with the new name on it"))
        return 1;

    model.upsertDirect({mia, QStringLiteral("Aaron")});
    if (!check(resets.count() == 1, "a rename that moves the row reorders the list") ||
        !check(nameAt(model, 2) == QStringLiteral("Aaron"), "to where the new name belongs") ||
        !check(nameAt(model, 3) == QStringLiteral("Adam"), "pushing the rest down"))
        return 1;

    // Unread counts and previews are keyed by the conversation, so a channel
    // moving to another slot -- or a peer moving up the list -- keeps them.
    model.bumpUnread(adam);
    model.bumpUnread(adam);
    model.setLastMessage(adam, message(QStringLiteral("Adam"), QStringLiteral("on my way")));
    model.setLastMessage(generalConversation,
                         message(QStringLiteral("Ben"), QStringLiteral("hello")));
    model.setChannels({channel(6, QStringLiteral("General"), '\x11')});
    if (!check(model.unreadCount(adam) == 2, "a peer keeps its unread count across a rebuild") ||
        !check(model.channelAt(6) == generalConversation,
               "a channel that moved slot is the same conversation") ||
        !check(model.data(model.index(0), model::ConversationModel::PreviewRole).toString() ==
                   QStringLiteral("Ben: hello"),
               "a channel preview names who spoke") ||
        !check(model.data(model.index(model.rowFor(adam)),
                          model::ConversationModel::PreviewRole)
                       .toString() == QStringLiteral("on my way"),
               "a direct preview does not repeat the name the row already carries"))
        return 1;

    model.clearUnread(adam);
    if (!check(model.unreadCount(adam) == 0, "opening a conversation clears its count"))
        return 1;

    // Nothing but this list says a direct conversation exists, so forgetting one
    // is removing the row. A channel has already left the list by then.
    const int before = model.rowCount();
    model.forget(adam);
    if (!check(model.rowCount() == before - 1, "forgetting a peer removes its row") ||
        !check(model.rowFor(adam) < 0, "and it is not found again"))
        return 1;

    model.clearTransientState();
    if (!check(model.unreadCount(peer('\x03')) == 0,
               "leaving a device drops even key-based state"))
        return 1;

    return 0;
}
