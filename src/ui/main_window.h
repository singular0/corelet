#pragma once

#include <QHash>
#include <QMainWindow>
#include <optional>

#include "model/history.h"
#include "protocol/client.h"

class QAction;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class QTimer;
class QToolButton;
class ByteLimit;
class ElidedLabel;
class MessageDelegate;
class NodePane;

namespace model {
class ChatModel;
class ConversationModel;
}  // namespace model

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const proto::ConnectTarget& target, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void onConversationSelected(const QModelIndex& current, const QModelIndex& previous);
    void onSendClicked();

    void onStateChanged(proto::CompanionClient::State state, const QString& detail);
    void onDeviceInfo(const proto::CompanionClient::DeviceInfo& info);
    void onChannelsChanged(const QVector<model::Channel>& channels);
    void onMessageReceived(const model::Message& msg);
    void onDirectMessageReceived(const model::Message& msg);
    void onSendResult(int token, bool ok, const QString& error);
    void onSendConfirmed(int token);
    void onSendUnconfirmed(int token);
    void onChannelSaveResult(int channelIndex, bool ok, const QString& error);
    void onChannelRemoveResult(int channelIndex, bool ok, const QString& error);

private:
    void buildUi();
    // Points the client at a target, replacing whatever it was talking to.
    void connectTo(const proto::ConnectTarget& target);
    void openConnectDialog();
    // The address book, which lives on the node: nothing to show without a link.
    void showContacts();
    // The add button's menu: a direct conversation, then one item per kind of
    // channel, since which kind is being added is settled before any of them
    // has a field to fill in.
    void showAddMenu();
    // Writes a channel the menu produced -- from a dialog, or straight from the
    // public channel's constant key -- to the slot it carries.
    void addChannel(const model::Channel& channel);
    // Picks a peer out of the address book and opens the conversation with
    // them, which for somebody nobody has spoken to yet is what creates it.
    void startDirectConversation();
    // Shows the selected channel's key, to pass on to whoever is being invited.
    void shareCurrentChannel();
    // Asks first, and what it warns about depends on the kind: removing a
    // channel deletes its key from the device and a private one cannot be got
    // back, while a direct conversation is only ever local.
    void removeCurrentConversation();
    // Asks before something goes away for good, and says what removing it will
    // actually cost -- which differs enough between a channel whose key lives
    // only on the device and a conversation that is only ever local to be worth
    // spelling out each time.
    bool confirmRemoval(const QString& title, const QString& name,
                        const QString& impactItems);

    // --- the sidebar --------------------------------------------------------
    // Shows a channel list, from the active device or its identity-scoped
    // cache, and rebuilds the peer rows beside it.
    void showChannels(const QVector<model::Channel>& channels);
    void loadCachedChannels();
    void saveCachedChannels(const QVector<model::Channel>& channels);
    // The peer rows: everyone this device has stored messages with, plus any
    // conversation somebody opened and has not spoken in yet, which exists only
    // in settings.
    void loadDirectConversations();
    // Remembers a peer under this device, so an empty conversation survives a
    // restart and so the sidebar has a name to draw while offline -- the same
    // reason channel names are cached.
    void rememberDirectConversation(const model::Conversation& conversation);
    void forgetDirectConversation(const model::Conversation& conversation);
    // What to call a peer: what the address book says now, what it said when
    // this device was last connected, or the key itself when nothing ever has.
    QString peerName(const model::Conversation& conversation) const;
    // Fills in each row's newest message, which is what the sidebar draws under
    // the name.
    void hydratePreviews();
    // Puts the selection back on `wanted`, or on the first row when that
    // conversation is no longer there. Every rebuild of the rows ends here: a
    // model reset drops the view's current index, and the pane would be left
    // showing a conversation the list no longer has.
    // Taken by value: it is normally the open conversation, which this clears.
    void restoreSelection(model::Conversation wanted);
    void selectConversation(const model::Conversation& conversation);
    void showConversation(const model::Conversation& conversation);
    // The wire slot of the open conversation, or -1 when it is a direct one --
    // which has no slot, being addressed by key.
    int currentChannelIndex() const;
    // The selected channel as the device holds it -- with its key, and so with
    // its real type, which the offline cache cannot give. Empty when the open
    // conversation is not a channel or the link is down, which is also when
    // nothing can be done to it.
    std::optional<model::Channel> currentChannelOnDevice() const;
    // The conversation the window was last left on, for this device.
    model::Conversation rememberedConversation() const;
    void rememberConversation(const model::Conversation& conversation);

    void appendToView(const model::Message& msg);
    // Hands the message box's byte counter the budget the open conversation
    // leaves for a message body. Driven by the selection and by the node name
    // arriving or changing; ordinary editing needs nothing, the counter follows
    // the field itself.
    void updateMessageBudget();
    void updateInputState();
    void updateReadyStatus();
    // Adding and removing a channel are both writes to the device, so they need
    // a live link -- and a free slot to write into, or a removable channel
    // selected. A direct conversation is local either way.
    void updateConversationActions();
    void updateHeader();
    // A passing word above the message box -- a failed send, a channel being
    // added. It costs no vertical space while there is nothing to say, which is
    // why this is not a permanent strip.
    void showNotice(const QString& text, int ms, bool error = false);
    void hideNotice();
    // Storage failure is not a passing word: history is the only copy of a
    // collected message, so this holds the notice line until storage answers
    // again. A transient notice only borrows the line and gives it back.
    void setStorageFault(const QString& text);
    void clearStorageFault();

    // A direct message can be collected before the address book can name the
    // peer it came from, in which case it is stored under the six bytes the
    // wire gave. This is where those messages join the conversation with that
    // peer, and it is why the address book arriving is worth acting on.
    void resolveDirectPeers();
    // Opens this device's database and tells the client whether collecting
    // messages is safe. Runs before the handshake completes, which is before
    // the client sends its first SYNC_NEXT_MESSAGE.
    void preflightStorage();
    // A history failure is data loss in progress -- collection has already
    // taken the message off the node -- so it stops the drain as well as
    // saying so. `action` completes "Could not ...".
    void onStorageFailure(const QString& action, const model::HistoryResult& result);
    // Records how a direct send ended up on the row it was written to, and lets
    // go of that row once nothing can answer for it any more.
    void settleStoredSend(int token, model::Message::SendState state);

    proto::CompanionClient* client_ = nullptr;
    proto::ConnectTarget target_;
    model::History history_;
    // The MeshCore public key learned from SELF_INFO. No conversation or
    // message is read or written before this is known.
    QByteArray activeDeviceId_;

    model::ConversationModel* conversationModel_ = nullptr;
    model::ChatModel* chatModel_ = nullptr;
    MessageDelegate* messageDelegate_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QListView* conversationList_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* shareChannelButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    // Foot of the sidebar: the node, the link, and the way to the connect
    // dialog. There is no status bar and no menu bar to put any of that in.
    NodePane* nodePane_ = nullptr;
    QListView* chatView_ = nullptr;
    QLabel* header_ = nullptr;
    QLineEdit* input_ = nullptr;
    QAction* sendAction_ = nullptr;
    // Caps the message box in encoded bytes. Held on to because the budget moves
    // with the open conversation and with the node's name.
    ByteLimit* messageLimit_ = nullptr;
    ElidedLabel* notice_ = nullptr;
    QTimer* noticeTimer_ = nullptr;
    // Non-empty while storage is unusable, which is also while the inbox drain
    // is stopped. Cleared by a preflight that succeeds.
    QString storageFault_;
    // One preflight per handshake. Reset whenever the link leaves Ready, which
    // is the only thing that gives storage another chance.
    bool storagePreflighted_ = false;
    bool contactsSyncing_ = false;
    bool messagesSyncing_ = false;

    // The open conversation, or an invalid one when nothing is selected. This
    // is an identity rather than a wire address: a channel that moves slot
    // stays open, and a peer has no slot at all.
    model::Conversation current_;

    // Sends the daemon has not answered yet, by the tag the app gave them. They
    // are on screen but not in history: a message is written down only once it
    // is known to have got somewhere.
    struct PendingSend {
        model::Message message;
        QByteArray deviceId;
        model::Conversation conversation;
    };
    QHash<int, PendingSend> pendingSends_;
    // Direct sends the daemon has taken and the peer has not confirmed, by the
    // same tag: the device database and the row each message was written to, so
    // an answer arriving a minute later can be recorded rather than only drawn.
    // The row number is what survives -- the tag means nothing next session, and
    // the on-screen row may have been replaced by a reload.
    //
    // Emptied when the link leaves Ready, which is exactly when the client
    // abandons the acks it was waiting on: nothing can answer for these
    // afterwards, since a push cannot reach a session that is gone.
    struct OpenSend {
        QByteArray deviceId;
        qint64 rowId = 0;
    };
    QHash<int, OpenSend> openSends_;
    // clearChannel() removes the channel from the client's list before its
    // result reaches the window, so retain the key-derived identity here.
    QHash<int, model::Conversation> pendingChannelRemovals_;
    int lastSendToken_ = 0;
};
