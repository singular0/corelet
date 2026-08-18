#include "ui/main_window.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <memory>

#include "model/chat_model.h"
#include "model/conversation_model.h"
#include "protocol/text_limits.h"
#include "ui/add_channel_dialogs.h"
#include "ui/byte_limit.h"
#include "ui/connect_dialog.h"
#include "ui/conversation_delegate.h"
#include "ui/contacts_dialog.h"
#include "ui/dialog_settings.h"
#include "ui/elided_label.h"
#include "ui/icons.h"
#include "ui/message_delegate.h"
#include "ui/node_pane.h"
#include "ui/share_channel_dialog.h"
#include "ui/theme.h"

namespace {

QString historyDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/history");
}

QString deviceSettingsGroup(const QByteArray& deviceId) {
    return QStringLiteral("devices/%1").arg(QString::fromLatin1(deviceId.toHex()));
}

// The title bar is the only place the version is visible: an ad-hoc signed
// build has no About box worth opening and the Linux package is installed by
// hand, so a bug report needs the number somewhere permanently on screen.
QString windowTitleFor(const QString& deviceName) {
    const QString base = QStringLiteral("Corelet %1").arg(QCoreApplication::applicationVersion());
    return deviceName.isEmpty() ? base : QStringLiteral("%1 — %2").arg(base, deviceName);
}

// Every node ships with the Public channel and it is how a stranger is reached
// at all, so it stays where it is. It also costs nothing to keep: its key is a
// constant, not something the user could lose.
bool isRemovable(const model::Channel& ch) {
    return ch.type != model::ChannelType::Public;
}

// A channel action in the sidebar header. One SVG rendered in three colours:
// QIcon picks the mode itself, which is cheaper than restyling the button on
// hover and enable changes. `hover` is what the action means -- the accent for
// adding, the error colour for anything that takes something away.
QToolButton* headerButton(const QString& icon, const QColor& hover, int iconSize, qreal dpr) {
    QIcon set;
    set.addPixmap(icons::tinted(icon, iconSize, theme::TextMuted, dpr), QIcon::Normal);
    set.addPixmap(icons::tinted(icon, iconSize, hover, dpr), QIcon::Active);
    set.addPixmap(icons::tinted(icon, iconSize, theme::Border, dpr), QIcon::Disabled);

    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("iconButton"));
    button->setIcon(set);
    button->setIconSize(QSize(iconSize, iconSize));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

// A chat reads from the bottom, so a conversation shorter than the window has
// to sit on the input box rather than floating at the top of an empty pane.
// QListView cannot anchor content to the bottom, so the slack is pushed into
// the viewport's top margin.
class ChatListView : public QListView {
public:
    using QListView::QListView;

    // The view keeps its own anchor current, so nothing outside has to
    // remember to call it after every append.
    void setModel(QAbstractItemModel* model) override {
        QListView::setModel(model);
        if (!model) return;
        connect(model, &QAbstractItemModel::rowsInserted, this,
                [this] { scheduleAnchor(); });
        // A row can go away again: a send the daemon refuses is taken back off
        // the screen, and the conversation has to settle back onto the input box.
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { scheduleAnchor(); });
        connect(model, &QAbstractItemModel::modelReset, this, [this] { scheduleAnchor(); });
    }

    void updateBottomAnchor() {
        const int rows = model() ? model()->rowCount() : 0;
        // The unpadded height available to items: viewport() already excludes
        // whatever margin is currently applied.
        const int available = viewport()->height() + topMargin_;

        int content = 0;
        for (int i = 0; i < rows; i++) {
            content += sizeHintForRow(i);
            // Once the conversation is taller than the window no padding is
            // needed, and stopping here keeps this bounded to a screenful of
            // text layouts however long the history gets.
            if (content >= available) {
                setTopMargin(0);
                return;
            }
        }
        setTopMargin(available - content);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QListView::resizeEvent(event);
        scheduleAnchor();
    }

private:
    // Deferred: row heights depend on the delegate's viewport width, which the
    // window sets from its own resizeEvent, and on rows the view has not laid
    // out yet at the moment the model announces them.
    void scheduleAnchor() {
        if (anchorQueued_) return;
        anchorQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            anchorQueued_ = false;
            updateBottomAnchor();
        });
    }

    void setTopMargin(int margin) {
        if (margin == topMargin_) return;
        topMargin_ = margin;
        setViewportMargins(0, margin, 0, 0);
    }

    int topMargin_ = 0;
    bool anchorQueued_ = false;
};

}  // namespace

MainWindow::MainWindow(const proto::ConnectTarget& target, QWidget* parent)
    : QMainWindow(parent), history_(historyDirectory()) {
    setWindowTitle(windowTitleFor({}));

    client_ = new proto::CompanionClient(this);
    buildUi();

    connect(client_, &proto::CompanionClient::stateChanged, this, &MainWindow::onStateChanged);
    connect(client_, &proto::CompanionClient::deviceInfoChanged, this, &MainWindow::onDeviceInfo);
    connect(client_, &proto::CompanionClient::channelsChanged, this,
            &MainWindow::onChannelsChanged);
    connect(client_, &proto::CompanionClient::contactsChanged, this,
            [this](const QVector<model::Contact>&) {
                contactsSyncing_ = false;
                resolveDirectPeers();
                updateReadyStatus();
            });
    // An advert can be the first thing to put a name to a peer somebody is
    // already talking to, which turns a row of hex into a person.
    connect(client_, &proto::CompanionClient::contactChanged, this,
            [this](const model::Contact& contact) {
                const model::Conversation peer =
                    model::Conversation::direct(contact.pubkey);
                if (conversationModel_->rowFor(peer) < 0) return;
                conversationModel_->upsertDirect({peer, contact.displayName()});
                rememberDirectConversation(peer);
                if (peer == current_) updateHeader();
            });
    connect(client_, &proto::CompanionClient::messageSyncChanged, this,
            [this](bool syncing) {
                messagesSyncing_ = syncing;
                updateReadyStatus();
            });
    connect(client_, &proto::CompanionClient::messageReceived, this,
            &MainWindow::onMessageReceived);
    connect(client_, &proto::CompanionClient::directMessageReceived, this,
            &MainWindow::onDirectMessageReceived);
    connect(client_, &proto::CompanionClient::sendResult, this, &MainWindow::onSendResult);
    connect(client_, &proto::CompanionClient::channelSaveResult, this,
            &MainWindow::onChannelSaveResult);
    connect(client_, &proto::CompanionClient::channelRemoveResult, this,
            &MainWindow::onChannelRemoveResult);

    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    splitter_->restoreState(settings.value(QStringLiteral("splitter")).toByteArray());
    // These values have no device or channel-key scope and cannot be assigned
    // safely.
    settings.remove(QStringLiteral("channelCache"));
    settings.remove(QStringLiteral("channel"));

    connectTo(target);
}

void MainWindow::connectTo(const proto::ConnectTarget& target) {
    if (!target.isValid()) return;

    // A target address is not a device identity: a daemon endpoint can later
    // serve another radio. Hide the old device immediately and do not load any
    // persisted state until SELF_INFO supplies the new public key.
    activeDeviceId_.clear();
    pendingChannelRemovals_.clear();
    current_ = {};
    // The new session preflights its own database, and that is what decides
    // whether there is still a fault to report.
    clearStorageFault();
    hideNotice();
    conversationModel_->clearTransientState();
    conversationModel_->setChannels({});
    conversationModel_->setDirectConversations({});
    chatModel_->setMessages({});
    setWindowTitle(windowTitleFor({}));
    nodePane_->setDevice({});
    updateHeader();
    updateInputState();
    updateConversationActions();

    target_ = target;
    nodePane_->setTarget(target);
    client_->start(proto::createTransport(target));
}

void MainWindow::openConnectDialog() {
    ConnectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    connectTo(dialog.target());
}

void MainWindow::showContacts() {
    // The node pane keeps its button disabled while the link is down; this is
    // what makes that a guard rather than a coat of paint.
    if (client_->state() != proto::CompanionClient::State::Ready) return;
    ContactsDialog(client_, this).exec();
}

void MainWindow::showAddMenu() {
    // Everything in this menu needs the node: the channel items write to it and
    // need its keys to recognise a channel already joined, and a direct
    // conversation is with somebody out of its address book. None of it works
    // from the offline cache.
    if (client_->state() != proto::CompanionClient::State::Ready) return;

    // Taken by value: each dialog runs an event loop, and a reconnect
    // re-enumerating the channels underneath it would leave the reference it
    // was built from dangling.
    const QVector<model::Channel> existing = client_->channels();

    // The kind of conversation is chosen before anything is typed, so it is a
    // menu rather than a control inside a dialog. The icons are the ones the
    // sidebar paints for each kind, which is what the new row will look like.
    const int iconSize = theme::scaled(font(), 14);
    const qreal dpr = devicePixelRatioF();
    QMenu menu(this);
    auto item = [&](const QString& icon, const QString& text) {
        QIcon set;
        set.addPixmap(icons::tinted(icon, iconSize, theme::TextMuted, dpr), QIcon::Normal);
        // The highlighted row goes accent under its icon, and that is the row a
        // menu draws in Active mode: a muted glyph left there would be the one
        // thing on it that did not come forward.
        set.addPixmap(icons::tinted(icon, iconSize, theme::Background, dpr), QIcon::Active);
        // Not the Border grey the header buttons ghost themselves with: those
        // sit on the darker sidebar, and on a popup's lifted surface the same
        // colour is not a dim icon, it is no icon.
        set.addPixmap(icons::tinted(icon, iconSize, theme::IconDisabled, dpr), QIcon::Disabled);
        return menu.addAction(set, text);
    };

    // A person rather than a channel, so it is first and set apart: it takes no
    // slot, needs nothing typed, and is the only item here that opens a list.
    QAction* directMessage = item(QStringLiteral("users"),
                                  QStringLiteral("New Direct Message"));
    menu.addSeparator();

    QAction* createPrivate = item(QStringLiteral("lock"),
                                  QStringLiteral("Create a Private Channel"));
    QAction* joinPrivate = item(QStringLiteral("lock"),
                                QStringLiteral("Join a Private Channel"));
    QAction* joinPublic = item(QStringLiteral("globe"),
                               QStringLiteral("Join the Public Channel"));
    QAction* joinHashtag = item(QStringLiteral("hash"),
                                QStringLiteral("Join a Hashtag Channel"));

    // There is exactly one public channel and its key is a constant, so joining
    // it twice is not a thing to do. The item stays in place rather than
    // disappearing, so the menu keeps the same items in the same order whatever
    // the device holds -- which is also why a device with all eight slots in use
    // greys the channel items rather than losing them.
    const bool room = AddChannelDialog::hasFreeSlot(existing);
    joinPublic->setEnabled(room && !AddChannelDialog::publicChannelJoined(existing));
    createPrivate->setEnabled(room);
    joinPrivate->setEnabled(room);
    joinHashtag->setEnabled(room);
    if (!room) {
        // The '+' used to go dead when the slots filled and the reason lived in
        // its tooltip. Now that it still has a direct conversation to offer, the
        // reason has to travel with the items that went grey instead -- and a
        // menu shows an action's tooltip only when asked to.
        menu.setToolTipsVisible(true);
        const QString full =
            QStringLiteral("All %1 channel slots are in use").arg(proto::MaxChannels);
        for (QAction* action : {createPrivate, joinPrivate, joinPublic, joinHashtag})
            action->setToolTip(full);
    }

    QAction* chosen = menu.exec(addButton_->mapToGlobal(QPoint(0, addButton_->height())));

    // A popup holds the mouse grab for as long as it is up, so the button that
    // opened it never sees the pointer leave: dismissed with a click somewhere
    // else, the '+' would stay lit as though the cursor were still on it. Only
    // when the pointer really has moved off -- clearing the flag under it would
    // unlight a button the cursor is still on and leave it that way, since no
    // second enter is coming.
    if (!addButton_->rect().contains(addButton_->mapFromGlobal(QCursor::pos()))) {
        addButton_->setAttribute(Qt::WA_UnderMouse, false);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(addButton_, &leave);
    }

    if (chosen == directMessage) {
        startDirectConversation();
        return;
    }

    // The public channel is the one kind with nothing to fill in.
    if (chosen == joinPublic) {
        addChannel(AddChannelDialog::publicChannel(existing));
        return;
    }

    std::unique_ptr<AddChannelDialog> dialog;
    if (chosen == createPrivate)
        dialog = std::make_unique<CreatePrivateChannelDialog>(existing, this);
    else if (chosen == joinPrivate)
        dialog = std::make_unique<JoinPrivateChannelDialog>(existing, this);
    else if (chosen == joinHashtag)
        dialog = std::make_unique<JoinHashtagChannelDialog>(existing, this);
    if (!dialog || dialog->exec() != QDialog::Accepted) return;

    addChannel(dialog->channel());
}

void MainWindow::addChannel(const model::Channel& ch) {
    if (!ch.configured()) return;
    showNotice(QStringLiteral("Adding %1...").arg(ch.displayName()), 10000);
    client_->setChannel(ch.index, ch.name, ch.secret);
}

void MainWindow::startDirectConversation() {
    if (client_->state() != proto::CompanionClient::State::Ready) return;

    const QByteArray peerKey = ContactsDialog::pickContact(client_, this);
    if (peerKey.size() != model::Conversation::IdSize) return;

    // A conversation with somebody already spoken to is simply opened again.
    // A new one has to exist before it can be selected, and nothing has been
    // said in it to make it exist, so the row and its settings entry are what
    // hold it until something is.
    const model::Conversation peer = model::Conversation::direct(peerKey);
    conversationModel_->upsertDirect({peer, peerName(peer)});
    rememberDirectConversation(peer);
    selectConversation(peer);
    input_->setFocus(Qt::OtherFocusReason);
}

void MainWindow::shareCurrentChannel() {
    // Only the device's list carries keys, and a key is the entire invitation:
    // the cached list the sidebar falls back on while offline has nothing to
    // share.
    const std::optional<model::Channel> ch = currentChannelOnDevice();
    if (!ch) return;

    ShareChannelDialog(*ch, this).exec();
}

// Asks before something goes away for good. `impactItems` is the <li> list of
// what removing this actually costs, which differs enough between a channel
// whose key lives only on the device and a conversation that is only ever local
// to be worth spelling out each time.
bool MainWindow::confirmRemoval(const QString& title, const QString& name,
                                const QString& impactItems) {
    QDialog dialog(this);
    ui::configureDialogWindow(dialog);
    dialog.setWindowTitle(title);

    const int removeIconSize = theme::scaled(dialog.font(), 40);
    auto* icon = new QLabel;
    icon->setPixmap(icons::tinted(QStringLiteral("trash-2"), removeIconSize, theme::Error,
                                  devicePixelRatioF()));
    icon->setFixedSize(removeIconSize, removeIconSize);
    icon->setAlignment(Qt::AlignTop);

    auto* content = new QLabel(
        QStringLiteral("<p style=\"margin: 0;\">Remove <b>%1</b>?</p>"
                       "<ul style=\"margin-top: 8px; margin-bottom: 0; margin-left: 16px; "
                       "margin-right: 0; -qt-list-indent: 0; color: %2;\">%3</ul>")
            .arg(name.toHtmlEscaped(), theme::TextMuted.name(), impactItems));
    content->setTextFormat(Qt::RichText);
    content->setWordWrap(true);
    content->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto* bodyLayout = new QHBoxLayout;
    bodyLayout->addWidget(icon, 0, Qt::AlignTop);
    bodyLayout->addWidget(content, 1, Qt::AlignTop);

    auto* cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setDefault(true);
    auto* confirm = new QPushButton(QStringLiteral("Remove"));

    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(6);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(cancel);
    buttonLayout->addWidget(confirm);

    auto* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(20, 16, 20, 16);
    dialogLayout->setSpacing(12);
    dialogLayout->addLayout(bodyLayout);
    dialogLayout->addLayout(buttonLayout);
    ui::lockDialogSize(dialog, *dialogLayout, 500);

    connect(confirm, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted;
}

void MainWindow::removeCurrentConversation() {
    if (current_.isDirect()) {
        const model::Conversation peer = current_;
        const QString name = peerName(peer);
        // Nothing on the node knows this conversation exists: it is a row and a
        // pile of messages in this app's own database, and both go together.
        if (!confirmRemoval(
                QStringLiteral("Remove conversation"), name,
                QStringLiteral("<li>The conversation's messages will be deleted from "
                               "this app.</li>"
                               "<li>%1 stays in the node's contacts.</li>"
                               "<li>A new message from them starts it again.</li>")
                    .arg(name.toHtmlEscaped())))
            return;

        const model::HistoryResult forgotten = history_.remove(activeDeviceId_, peer);
        if (!forgotten) {
            showNotice(QStringLiteral("Could not delete the conversation: %1")
                           .arg(forgotten.error),
                       8000, true);
            return;
        }
        forgetDirectConversation(peer);
        // forget() takes the row out and the view moves its selection to a
        // neighbour, so this is what settles on whatever survived rather than
        // leaving the pane on a conversation that is gone.
        conversationModel_->forget(peer);
        restoreSelection(current_);
        return;
    }

    // Taken by value: the dialog below runs an event loop, and a reconnect
    // re-enumerating the channels underneath it would leave a reference to the
    // client's vector dangling.
    const std::optional<model::Channel> ch = currentChannelOnDevice();
    if (!ch || !isRemovable(*ch)) return;

    // The key lives on the device and nowhere else -- the app caches names only
    // -- so this is the last chance to say so.
    const QString impactItems =
        ch->type == model::ChannelType::Private
            ? QStringLiteral("<li>Channel's key will be deleted the device.</li>"
                             "<li>Channel's messages history will be deleted from this app.</li>"
                             "<li>You need a copy of the channel key to rejoin.</li>")
            : QStringLiteral("<li>Channel's key will be deleted the device.</li>"
                             "<li>Channel's messages history will be deleted from this app.</li>"
                             "<li>You can rejoin the channel any time.</li>");
    if (!confirmRemoval(QStringLiteral("Remove channel"), ch->displayName(), impactItems))
        return;

    showNotice(QStringLiteral("Removing %1...").arg(ch->displayName()), 10000);
    pendingChannelRemovals_.insert(ch->index,
                                   model::Conversation::channel(ch->keyFingerprint()));
    client_->clearChannel(ch->index);
}

void MainWindow::buildUi() {
    // --- channel list -------------------------------------------------------
    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    // The title and the channel actions share the header strip: at 480 rows
    // there is no menu bar and no room for a toolbar, so they live beside the
    // label they belong to.
    auto* sidebarHeader = new QWidget;
    sidebarHeader->setObjectName(QStringLiteral("sidebarHeader"));
    auto* sidebarHeaderLayout = new QHBoxLayout(sidebarHeader);
    sidebarHeaderLayout->setContentsMargins(10, 4, 6, 4);
    sidebarHeaderLayout->setSpacing(4);

    auto* sidebarTitle = new QLabel(QStringLiteral("CONVERSATIONS"));
    QFont headerFont = theme::secondaryFont(sidebarTitle->font());
    headerFont.setBold(true);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    sidebarTitle->setFont(headerFont);
    sidebarTitle->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    const qreal dpr = devicePixelRatioF();
    const int headerIconSize = theme::scaled(font(), 14);
    addButton_ = headerButton(QStringLiteral("plus"), theme::Accent, headerIconSize, dpr);
    // Share and remove both act on the selected row rather than carrying one of
    // their own: per-row buttons would cost sidebar width the uConsole has not
    // got, and hover affordances are no use on a trackball.
    shareChannelButton_ =
        headerButton(QStringLiteral("share"), theme::Accent, headerIconSize, dpr);
    removeButton_ = headerButton(QStringLiteral("minus"), theme::Error, headerIconSize, dpr);

    sidebarHeaderLayout->addWidget(sidebarTitle, 1);
    sidebarHeaderLayout->addWidget(shareChannelButton_);
    sidebarHeaderLayout->addWidget(removeButton_);
    sidebarHeaderLayout->addWidget(addButton_);

    conversationModel_ = new model::ConversationModel(this);
    conversationList_ = new QListView;
    conversationList_->setObjectName(QStringLiteral("conversationList"));
    conversationList_->setModel(conversationModel_);
    conversationList_->setItemDelegate(new ConversationDelegate(conversationList_));
    conversationList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    conversationList_->setSelectionMode(QAbstractItemView::SingleSelection);
    conversationList_->setFocusPolicy(Qt::NoFocus);

    nodePane_ = new NodePane;

    leftLayout->addWidget(sidebarHeader);
    leftLayout->addWidget(conversationList_, 1);
    leftLayout->addWidget(nodePane_);

    // --- chat ---------------------------------------------------------------
    auto* right = new QWidget;
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    header_ = new QLabel;
    header_->setObjectName(QStringLiteral("header"));

    chatModel_ = new model::ChatModel(this);
    auto* chatView = new ChatListView;
    chatView_ = chatView;
    chatView_->setModel(chatModel_);
    messageDelegate_ = new MessageDelegate(chatView_);
    chatView_->setItemDelegate(messageDelegate_);
    chatView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chatView_->setSelectionMode(QAbstractItemView::NoSelection);
    chatView_->setFocusPolicy(Qt::NoFocus);
    chatView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    chatView_->setUniformItemSizes(false);
    chatView_->setResizeMode(QListView::Adjust);

    auto* inputRow = new QWidget;
    auto* inputLayout = new QVBoxLayout(inputRow);
    inputLayout->setContentsMargins(8, 6, 8, 6);
    inputLayout->setSpacing(2);

    input_ = new QLineEdit;
    input_->setPlaceholderText(QStringLiteral("Message"));
    // Not setMaxLength: the limit is a budget of encoded bytes rather than a
    // character count. What fits depends on the node's name, which is not known
    // yet, so this starts on the budget of a nameless node and is corrected
    // when SELF_INFO answers.
    messageLimit_ = new ByteLimit(input_, proto::maxMessageBytes(client_->device().name));

    const int sendIconSize = theme::scaled(input_->font(), 16);
    QIcon sendIcon;
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), sendIconSize, theme::Accent, dpr),
                       QIcon::Normal);
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), sendIconSize, theme::Text, dpr),
                       QIcon::Active);
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), sendIconSize, theme::Border, dpr),
                       QIcon::Disabled);
    sendAction_ = input_->addAction(sendIcon, QLineEdit::TrailingPosition);
    sendAction_->setObjectName(QStringLiteral("sendAction"));
    sendAction_->setText(QStringLiteral("Send message"));
    sendAction_->setToolTip(QStringLiteral("Send message"));

    inputLayout->addWidget(input_);

    // Sits above the message box, where the eye already is when a send fails,
    // and takes no room at all the rest of the time.
    notice_ = new ElidedLabel;
    notice_->setObjectName(QStringLiteral("notice"));
    notice_->setFont(theme::secondaryFont(font()));
    notice_->hide();

    noticeTimer_ = new QTimer(this);
    noticeTimer_->setSingleShot(true);
    connect(noticeTimer_, &QTimer::timeout, this, [this] {
        // A transient notice had the line for its few seconds; a storage fault
        // is still true afterwards and takes it back.
        if (storageFault_.isEmpty())
            notice_->hide();
        else
            setStorageFault(storageFault_);
    });

    rightLayout->addWidget(header_);
    rightLayout->addWidget(chatView_, 1);
    rightLayout->addWidget(notice_);
    rightLayout->addWidget(inputRow);

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->addWidget(left);
    splitter_->addWidget(right);
    splitter_->setStretchFactor(0, 0);
    splitter_->setStretchFactor(1, 1);
    splitter_->setCollapsible(0, false);
    splitter_->setSizes({210, 800});
    setCentralWidget(splitter_);

    connect(nodePane_, &NodePane::connectRequested, this, &MainWindow::openConnectDialog);
    connect(nodePane_, &NodePane::disconnectRequested, client_, &proto::CompanionClient::stop);
    connect(nodePane_, &NodePane::contactsRequested, this, &MainWindow::showContacts);
    connect(addButton_, &QToolButton::clicked, this, &MainWindow::showAddMenu);
    connect(shareChannelButton_, &QToolButton::clicked, this, &MainWindow::shareCurrentChannel);
    connect(removeButton_, &QToolButton::clicked, this, &MainWindow::removeCurrentConversation);
    connect(conversationList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onConversationSelected);
    connect(sendAction_, &QAction::triggered, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);
    // Connected after the counter's own, so this reads a field already held
    // inside its budget. All it decides is whether there is anything to send.
    connect(input_, &QLineEdit::textChanged, this, [this] { updateInputState(); });

    // The uConsole panel is 1280x480; this is a sane default anywhere else.
    resize(1024, 480);
    updateInputState();
    updateConversationActions();
    updateHeader();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings settings;
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("splitter"), splitter_->saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    // Bubble widths are a fraction of the viewport, so the delegate has to be
    // told when that changes or rows keep their old heights.
    messageDelegate_->setViewportWidth(chatView_->viewport()->width());
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void MainWindow::onChannelsChanged(const QVector<model::Channel>& channels) {
    // SELF_INFO precedes channel enumeration. Refusing an unscoped write here
    // is what prevents a malformed handshake from recreating the old bug.
    if (activeDeviceId_.size() != 32) return;
    saveCachedChannels(channels);
    showChannels(channels);
}

void MainWindow::loadCachedChannels() {
    if (activeDeviceId_.size() != 32) return;

    QVector<model::Channel> channels;
    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("channels"));
    for (const QString& key : settings.childGroups()) {
        const QByteArray fingerprint = QByteArray::fromHex(key.toLatin1());
        if (fingerprint.size() != 32) continue;

        settings.beginGroup(key);
        model::Channel ch;
        ch.index = settings.value(QStringLiteral("slot"), -1).toInt();
        ch.name = settings.value(QStringLiteral("name")).toString();
        ch.type = model::channelTypeFromInt(
            settings.value(QStringLiteral("type"), int(model::ChannelType::Private)).toInt());
        ch.cachedKeyFingerprint = fingerprint;
        settings.endGroup();
        if (ch.index < 0) continue;
        channels.append(ch);
    }
    settings.endGroup();
    settings.endGroup();

    std::sort(channels.begin(), channels.end(), [](const model::Channel& a,
                                                   const model::Channel& b) {
        return a.index < b.index;
    });
    // Unconditionally, unlike the channels themselves: a device with nothing
    // cached still has direct conversations to show, and showChannels() is what
    // builds them.
    showChannels(channels);
}

void MainWindow::saveCachedChannels(const QVector<model::Channel>& channels) {
    if (activeDeviceId_.size() != 32) return;

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    // Re-enumeration is authoritative. Removing the group first prevents a
    // deleted channel from lingering in this device's offline view.
    settings.remove(QStringLiteral("channels"));
    settings.beginGroup(QStringLiteral("channels"));
    for (const model::Channel& ch : channels) {
        const QByteArray fingerprint = ch.keyFingerprint();
        if (fingerprint.size() != 32) continue;
        settings.beginGroup(QString::fromLatin1(fingerprint.toHex()));
        settings.setValue(QStringLiteral("slot"), ch.index);
        settings.setValue(QStringLiteral("name"), ch.name);
        settings.setValue(QStringLiteral("type"), int(ch.type));
        settings.endGroup();
    }
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::showChannels(const QVector<model::Channel>& channels) {
    const model::Conversation wanted =
        current_.isValid() ? current_ : rememberedConversation();

    conversationModel_->setChannels(channels);
    // The channel enumeration says nothing about peers, but it does reset the
    // rows, and the direct conversations belong to the same device.
    loadDirectConversations();
    hydratePreviews();
    restoreSelection(wanted);
}

void MainWindow::loadDirectConversations() {
    if (activeDeviceId_.size() != 32) {
        conversationModel_->setDirectConversations({});
        return;
    }

    // Nothing on the node enumerates these: a conversation exists because
    // somebody said something in it, or because somebody opened it and has not
    // yet -- which lives in settings and nowhere else.
    QVector<model::Conversation> peers;
    const model::HistoryConversations stored = history_.directConversations(activeDeviceId_);
    if (!stored.result)
        onStorageFailure(QStringLiteral("read the direct conversations"), stored.result);
    else
        peers = stored.conversations;

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("directs"));
    for (const QString& key : settings.childGroups()) {
        const model::Conversation peer =
            model::Conversation::direct(QByteArray::fromHex(key.toLatin1()));
        if (peer.isValid() && !peers.contains(peer)) peers.append(peer);
    }
    settings.endGroup();
    settings.endGroup();

    QVector<model::ConversationEntry> rows;
    rows.reserve(peers.size());
    for (const model::Conversation& peer : peers) rows.append({peer, peerName(peer)});
    conversationModel_->setDirectConversations(rows);
}

void MainWindow::rememberDirectConversation(const model::Conversation& conversation) {
    if (activeDeviceId_.size() != 32 || !conversation.isDirect() || !conversation.isValid())
        return;

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("directs"));
    settings.beginGroup(QString::fromLatin1(conversation.id.toHex()));
    // The name only, and only as a fallback for when the address book cannot be
    // reached: the node's contact store is the authority on who a key is.
    // Written even when there is no name to write, because the entry existing
    // is what holds a conversation nothing has been said in yet.
    const model::Contact* peer = client_->contactFor(conversation);
    settings.setValue(QStringLiteral("name"), peer ? peer->displayName() : QString());
    settings.endGroup();
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::forgetDirectConversation(const model::Conversation& conversation) {
    if (activeDeviceId_.size() != 32 || !conversation.isValid()) return;

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("directs"));
    settings.remove(QString::fromLatin1(conversation.id.toHex()));
    settings.endGroup();
    settings.endGroup();
}

QString MainWindow::peerName(const model::Conversation& conversation) const {
    if (!conversation.isDirect() || conversation.id.isEmpty()) return {};

    if (const model::Contact* peer = client_->contactFor(conversation))
        return peer->displayName();

    if (activeDeviceId_.size() == 32) {
        QSettings settings;
        settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
        settings.beginGroup(QStringLiteral("directs"));
        settings.beginGroup(QString::fromLatin1(conversation.id.toHex()));
        const QString cached = settings.value(QStringLiteral("name")).toString();
        if (!cached.isEmpty()) return cached;
    }

    // Nothing has ever named this key. The leading bytes are how the daemon's
    // logs refer to such a node, and they are at least unique -- and they are
    // all a peer heard from before the address book caught up has to offer.
    return QString::fromLatin1(conversation.id.left(3).toHex());
}

void MainWindow::hydratePreviews() {
    if (activeDeviceId_.size() != 32) return;

    for (int row = 0; row < conversationModel_->rowCount(); row++) {
        const model::Conversation conversation = conversationModel_->conversationAt(row);
        const model::ConversationEntry* entry = conversationModel_->entry(conversation);
        const model::HistoryLatest latest = history_.latestMessage(
            activeDeviceId_, conversation, entry ? entry->channelIndex : -1);
        // One broken database breaks every lookup; say so once and stop asking
        // rather than paint the sidebar as a screenful of empty conversations.
        if (!latest.result) {
            onStorageFailure(QStringLiteral("read the message history"), latest.result);
            return;
        }
        if (latest.message) conversationModel_->setLastMessage(conversation, *latest.message);
    }
}

void MainWindow::restoreSelection(model::Conversation wanted) {
    // Selection follows the conversation, never a former wire slot. If it is
    // gone -- a channel removed or dropped by the device, a conversation
    // deleted -- fall back to the top of the list.
    if (conversationModel_->rowFor(wanted) < 0) wanted = conversationModel_->conversationAt(0);

    // Force showConversation() to reload: the rows are new, and the open
    // conversation may be gone from the list altogether, in which case the pane
    // would be left showing something that belongs to nothing.
    current_ = {};
    chatModel_->setMessages({});
    selectConversation(wanted);
    updateHeader();
    updateInputState();
    updateConversationActions();
}

void MainWindow::onChannelSaveResult(int channelIndex, bool ok, const QString& error) {
    if (!ok) {
        showNotice(QStringLiteral("Could not add the channel: %1").arg(error), 8000, true);
        return;
    }

    // The sidebar was rebuilt before this arrived, so the slot is there to open.
    // The user asked for it a moment ago; showing it is what they meant.
    hideNotice();
    selectConversation(conversationModel_->channelAt(channelIndex));
    updateConversationActions();
}

void MainWindow::onChannelRemoveResult(int channelIndex, bool ok, const QString& error) {
    const model::Conversation channel = pendingChannelRemovals_.take(channelIndex);
    if (!ok) {
        showNotice(QStringLiteral("Could not remove the channel: %1").arg(error), 8000, true);
        return;
    }

    // The sidebar was rebuilt before this arrived and has already moved off the
    // channel. Its key fingerprint was retained before the slot was cleared so
    // only this device's copy of that channel history is removed.
    hideNotice();
    const model::HistoryResult forgotten = history_.remove(activeDeviceId_, channel);
    // The channel is gone from the device either way, so this is not a failed
    // removal -- it is messages left behind for a channel that can no longer be
    // opened, which the user is owed the chance to clear up.
    if (!forgotten)
        showNotice(QStringLiteral("Channel removed, but its messages could not be deleted: %1")
                       .arg(forgotten.error),
                   8000, true);
    conversationModel_->forget(channel);
    updateConversationActions();
}

void MainWindow::selectConversation(const model::Conversation& conversation) {
    const int row = conversationModel_->rowFor(conversation);
    if (row < 0) {
        conversationList_->setCurrentIndex({});
        showConversation({});
        return;
    }
    conversationList_->setCurrentIndex(conversationModel_->index(row));
    // A reconnect resets the model, but the conversation may land on the same
    // row as before. Do not depend on QItemSelectionModel deciding that this is
    // a current-index change: the refreshed list must always reload from the
    // updated history.
    showConversation(conversation);
}

void MainWindow::onConversationSelected(const QModelIndex& current, const QModelIndex&) {
    showConversation(current.isValid() ? conversationModel_->conversationAt(current.row())
                                       : model::Conversation());
}

void MainWindow::showConversation(const model::Conversation& conversation) {
    if (conversation == current_) return;
    current_ = conversation;

    const int unseenCount = conversationModel_->unreadCount(conversation);
    QVector<model::Message> messages;
    if (conversation.isValid() && activeDeviceId_.size() == 32) {
        model::HistoryMessages stored =
            history_.messages(activeDeviceId_, conversation, currentChannelIndex());
        // An unreadable conversation must not be shown as an empty one: a
        // database that stopped opening would otherwise look exactly like a
        // channel nobody has said anything on.
        if (stored.result)
            messages = std::move(stored.messages);
        else
            onStorageFailure(QStringLiteral("read the conversation"), stored.result);
    }
    // Everything incoming here came from the one peer, so it is drawn under the
    // name that peer goes by now rather than the one it had when each message
    // arrived -- a rename should not leave a conversation full of old names.
    if (conversation.isDirect()) {
        const QString name = peerName(conversation);
        for (model::Message& msg : messages)
            if (!msg.outgoing) msg.sender = name;
    }
    chatModel_->setMessages(messages, unseenCount);
    conversationModel_->clearUnread(conversation);
    if (conversation.isValid() && activeDeviceId_.size() == 32)
        rememberConversation(conversation);
    updateHeader();
    updateInputState();
    // A direct message's budget is a constant and a channel's moves with the
    // node's name, so the counter follows the selection too.
    updateMessageBudget();
    // Sharing and removing act on the selection, so the buttons follow it.
    updateConversationActions();

    // Lay out first, then show the boundary of anything that arrived while the
    // conversation was closed. With no unseen messages, retain the usual newest-row
    // anchor. Scrolling before the delegate sizes the rows lands incorrectly.
    messageDelegate_->setViewportWidth(chatView_->viewport()->width());
    QTimer::singleShot(0, this, [this] {
        const int unseenRow = chatModel_->firstUnseenRow();
        if (unseenRow >= 0)
            chatView_->scrollTo(chatModel_->index(unseenRow), QAbstractItemView::PositionAtTop);
        else
            chatView_->scrollToBottom();
    });
}

int MainWindow::currentChannelIndex() const {
    const model::ConversationEntry* entry = conversationModel_->entry(current_);
    return entry ? entry->channelIndex : -1;
}

std::optional<model::Channel> MainWindow::currentChannelOnDevice() const {
    // Only the device's own list carries keys, and the type is derived from the
    // key: the cached list the sidebar shows while offline cannot answer this.
    if (client_->state() != proto::CompanionClient::State::Ready || !current_.isChannel())
        return std::nullopt;
    for (const model::Channel& ch : client_->channels())
        if (model::Conversation::channel(ch.keyFingerprint()) == current_) return ch;
    return std::nullopt;
}

model::Conversation MainWindow::rememberedConversation() const {
    if (activeDeviceId_.size() != 32) return {};

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("selectedConversation"));
    const model::Conversation remembered {
        model::conversationKindFromInt(settings.value(QStringLiteral("kind")).toInt()),
        QByteArray::fromHex(settings.value(QStringLiteral("id")).toString().toLatin1())};
    settings.endGroup();
    if (remembered.isValid()) return remembered;

    // What versions before direct conversations wrote: a channel fingerprint
    // under its own key, and nothing to say which kind it was because there was
    // only the one.
    const model::Conversation channel = model::Conversation::channel(QByteArray::fromHex(
        settings.value(QStringLiteral("selectedChannel")).toString().toLatin1()));
    settings.endGroup();
    return channel.isValid() ? channel : model::Conversation();
}

void MainWindow::rememberConversation(const model::Conversation& conversation) {
    if (activeDeviceId_.size() != 32 || !conversation.isValid()) return;

    QSettings settings;
    settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
    settings.beginGroup(QStringLiteral("selectedConversation"));
    settings.setValue(QStringLiteral("kind"), int(conversation.kind));
    settings.setValue(QStringLiteral("id"), QString::fromLatin1(conversation.id.toHex()));
    settings.endGroup();
    settings.endGroup();
}

void MainWindow::updateHeader() {
    const model::ConversationEntry* entry = conversationModel_->entry(current_);
    if (!entry) {
        header_->setText(conversationModel_->rowCount() == 0
                             ? QStringLiteral("<b>No conversations</b>")
                             : QStringLiteral("<b>Select a conversation</b>"));
        return;
    }

    // Only the conversation's own name and what addresses it: who we are and
    // what the radio is doing belong to the node pane, which says it once
    // instead of on every row. For a channel that address is the wire slot,
    // useful when cross-checking against the daemon though persistence follows
    // the key; for a peer it is the leading bytes of the key itself, which is
    // the only thing that tells two nodes of the same name apart.
    const QString address = current_.isDirect()
                                ? QString::fromLatin1(current_.id.left(3).toHex())
                                : QString::number(entry->channelIndex);
    header_->setText(QStringLiteral("<b>%1</b> <span style=\"color: %2;\">[%3]</span>")
                         .arg(entry->name.toHtmlEscaped(), theme::TextMuted.name(), address));
}

void MainWindow::showNotice(const QString& text, int ms, bool error) {
    notice_->setStyleSheet(
        QStringLiteral("color: %1;").arg((error ? theme::Error : theme::TextMuted).name()));
    notice_->setFullText(text);
    notice_->show();
    noticeTimer_->start(ms);
}

void MainWindow::hideNotice() {
    noticeTimer_->stop();
    if (!storageFault_.isEmpty()) {
        setStorageFault(storageFault_);
        return;
    }
    notice_->hide();
}

void MainWindow::setStorageFault(const QString& text) {
    storageFault_ = text;
    noticeTimer_->stop();
    notice_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::Error.name()));
    notice_->setFullText(storageFault_);
    // The line elides at 1280x480 and this is the one notice worth reading in
    // full, so the whole sentence is also available on hover.
    notice_->setToolTip(storageFault_);
    notice_->show();
}

void MainWindow::clearStorageFault() {
    if (storageFault_.isEmpty()) return;
    storageFault_.clear();
    notice_->setToolTip({});
    // Whatever transient notice is up has its own timer and keeps its seconds.
    if (!noticeTimer_->isActive()) notice_->hide();
}

void MainWindow::resolveDirectPeers() {
    if (activeDeviceId_.size() != 32) return;

    const model::HistoryConversations stored = history_.directConversations(activeDeviceId_);
    if (!stored.result) {
        onStorageFailure(QStringLiteral("read the direct conversations"), stored.result);
        return;
    }

    model::Conversation wanted = current_;
    bool moved = false;
    for (const model::Conversation& conversation : stored.conversations) {
        if (conversation.resolved()) continue;
        const model::Contact* peer = client_->contactFor(conversation);
        if (!peer) continue;  // still nobody this end can name; it keeps
        const model::HistoryResult folded =
            history_.resolvePeer(activeDeviceId_, peer->pubkey);
        if (!folded) {
            onStorageFailure(QStringLiteral("place a collected direct message"), folded);
            return;
        }
        const model::Conversation resolved = model::Conversation::direct(peer->pubkey);
        forgetDirectConversation(conversation);
        rememberDirectConversation(resolved);
        // The open conversation follows its peer rather than emptying out under
        // the reader.
        if (current_ == conversation) wanted = resolved;
        moved = true;
    }

    // The rows have to be rebuilt whether or not anything moved: the address
    // book arriving is also what puts names to peers the sidebar was drawing as
    // hex, and either way the previews follow.
    loadDirectConversations();
    hydratePreviews();
    if (moved || conversationModel_->rowFor(current_) < 0) restoreSelection(wanted);
    else selectConversation(wanted);
}

void MainWindow::preflightStorage() {
    // No identity, no database. The client is told so rather than left to
    // collect messages into nowhere.
    if (activeDeviceId_.size() != 32) {
        client_->setStorageAvailable(false);
        return;
    }

    storagePreflighted_ = true;
    const model::HistoryResult ready = history_.preflight(activeDeviceId_);
    if (!ready) {
        onStorageFailure(QStringLiteral("open the message history"), ready);
        return;
    }
    clearStorageFault();
    client_->setStorageAvailable(true);
}

void MainWindow::onStorageFailure(const QString& action, const model::HistoryResult& result) {
    // Stop first: SYNC_NEXT_MESSAGE destroys the daemon's copy of whatever it
    // returns, so every further collection with storage broken is another
    // message gone. The backlog keeps until the app can write it down.
    client_->setStorageAvailable(false);
    setStorageFault(QStringLiteral("Could not %1: %2. Messages are not being collected; "
                                   "reconnect once storage is writable.")
                        .arg(action, result.error));
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

void MainWindow::onMessageReceived(const model::Message& msg) {
    // Whatever the app could not work out about where this belongs, the node no
    // longer has a copy of it. An unplaceable message is stored in the reserved
    // conversation for its slot instead of being thrown away, and the retry is
    // whoever reads it back later, not the mesh. The client resolves the slot
    // against its own live channel list, so a message racing a UI reset is
    // placed by what the device says rather than by what is painted.
    const bool knownDevice = activeDeviceId_.size() == 32;
    const bool knownChannel = msg.conversation.isValid();
    const QByteArray deviceId =
        knownDevice ? activeDeviceId_ : model::History::orphanDeviceId();
    const model::Conversation conversation =
        knownChannel ? msg.conversation : model::History::orphanChannel(msg.channelIndex);

    const model::HistoryResult stored = history_.append(deviceId, conversation, msg);
    if (!stored) {
        onStorageFailure(QStringLiteral("save a received message"), stored);
        return;
    }
    if (!knownDevice || !knownChannel) {
        // There is no row to hang it on and no view that would show it, so the
        // only honest thing left is to say it happened.
        showNotice(QStringLiteral("A message arrived that could not be matched to a channel; "
                                  "it was saved out of the way."),
                   8000, true);
        return;
    }

    conversationModel_->setLastMessage(conversation, msg);

    if (conversation == current_)
        appendToView(msg);
    else
        conversationModel_->bumpUnread(conversation);
}

void MainWindow::onDirectMessageReceived(const model::Message& msg) {
    // Written down before anything is drawn: SYNC_NEXT_MESSAGE has already
    // popped this from the daemon's inbox, so a row on screen that never
    // reached the database is a message that exists nowhere.
    const bool knownDevice = activeDeviceId_.size() == 32;
    const model::HistoryResult stored =
        history_.append(knownDevice ? activeDeviceId_ : model::History::orphanDeviceId(),
                        msg.conversation, msg);
    if (!stored) {
        onStorageFailure(QStringLiteral("save a received direct message"), stored);
        return;
    }
    if (!knownDevice) {
        // No device identity yet means no database of its own to file this
        // under and no sidebar to hang it on, so the only honest thing left is
        // to say it happened.
        showNotice(QStringLiteral("A direct message arrived before the node identified "
                                  "itself; it was saved out of the way."),
                   8000, true);
        return;
    }

    // Nothing enumerates direct conversations, so hearing from somebody is what
    // creates the row -- and it is remembered so the conversation is still
    // there, with a name on it, the next time the app starts offline.
    const model::Conversation conversation = msg.conversation;
    const QString name = peerName(conversation);
    conversationModel_->upsertDirect({conversation, name});
    rememberDirectConversation(conversation);
    conversationModel_->setLastMessage(conversation, msg);

    if (conversation == current_) {
        // Drawn under the name the peer goes by now, which is what reopening
        // the conversation would show: the name stored with the message is only
        // the fallback for when nothing can be asked.
        model::Message shown = msg;
        shown.sender = name;
        appendToView(shown);
    } else {
        conversationModel_->bumpUnread(conversation);
    }
}

void MainWindow::appendToView(const model::Message& msg) {
    QScrollBar* bar = chatView_->verticalScrollBar();
    // Only follow the conversation if the user is already at the bottom;
    // yanking the view while they are reading back is worse than a missed jump.
    const bool atBottom = bar->value() >= bar->maximum() - 8;

    // An incoming row below the viewport is just as unseen as one received in
    // another channel. Give it a boundary without treating our own send as new.
    chatModel_->append(msg, !atBottom && !msg.outgoing);
    if (atBottom) QTimer::singleShot(0, this, [this] { chatView_->scrollToBottom(); });
}

void MainWindow::onSendClicked() {
    const QString text = input_->text().trimmed();
    if (text.isEmpty() || !current_.isValid()) return;
    if (client_->state() != proto::CompanionClient::State::Ready) return;
    if (activeDeviceId_.size() != 32) return;

    const int channelIndex = currentChannelIndex();
    if (current_.isChannel() && channelIndex < 0) return;

    input_->clear();

    // Our own transmission never comes back to us over the air, so the message
    // is echoed locally or it would never appear at all. It goes up marked as
    // pending straight away -- the daemon answering is what a reader is waiting
    // to know about, and an empty pane while it does says nothing.
    model::Message msg;
    msg.conversation = current_;
    msg.channelIndex = channelIndex;
    // The node prepends its own name to a channel message because nothing else
    // in one says who spoke. A direct message needs none: only the recipient's
    // key opens it, so who it is from is not in question.
    msg.sender = current_.isChannel() ? client_->device().name : QString();
    msg.text = text;
    msg.timestamp = QDateTime::currentDateTime();
    msg.outgoing = true;
    msg.sendState = model::Message::SendState::Pending;
    msg.sendToken = ++lastSendToken_;

    // Registered before the command goes out: a send refused on the spot answers
    // from inside the call below.
    pendingSends_.insert(msg.sendToken, {msg, activeDeviceId_, current_});
    appendToView(msg);
    if (current_.isDirect())
        client_->sendDirectMessage(current_, msg.text, msg.sendToken);
    else
        client_->sendChannelMessage(channelIndex, msg.text, msg.sendToken);
}

void MainWindow::onSendResult(int token, bool ok, const QString& error) {
    const auto it = pendingSends_.constFind(token);
    if (it == pendingSends_.constEnd()) return;
    const PendingSend pending = *it;
    model::Message msg = pending.message;
    pendingSends_.erase(it);

    if (!ok) {
        // Nothing went out, so nothing stays on screen claiming it did.
        chatModel_->removePending(token);
        showNotice(QStringLiteral("Could not send: %1").arg(error), 6000, true);
        // Hand the text back rather than losing it to a failed send.
        if (pending.deviceId == activeDeviceId_ && pending.conversation == current_ &&
            input_->text().isEmpty())
            input_->setText(msg.text);
        return;
    }

    // Only now is it worth writing down. History is what the app has instead of
    // the daemon's inbox, and a message that never left has no business in it.
    msg.sendState = model::Message::SendState::Sent;
    msg.sendToken = 0;
    const model::HistoryResult stored =
        history_.append(pending.deviceId, pending.conversation, msg);
    // The message did go out, so it stays on screen; what it says about storage
    // is the same as any other failed write, and collection stops on it.
    if (!stored) onStorageFailure(QStringLiteral("save a sent message"), stored);

    const bool stillShowingDevice = pending.deviceId == activeDeviceId_;
    if (stillShowingDevice) conversationModel_->setLastMessage(pending.conversation, msg);
    // The row it went up as is gone if the conversation was reloaded meanwhile.
    // Put the message back when that happened to the conversation being looked
    // at; any other one reads it from history when it is opened.
    if (!chatModel_->markSent(token) && stillShowingDevice &&
        pending.conversation == current_)
        appendToView(msg);
}

// The node prepends its own name to every channel message it sends, so what is
// left for the body moves with that name: connecting, or moving to a node called
// something else, is what makes this a budget rather than a constant. A direct
// message carries no such prefix and gets the whole payload.
void MainWindow::updateMessageBudget() {
    messageLimit_->setBudget(current_.isDirect()
                                 ? proto::MaxDirectTextBytes
                                 : proto::maxMessageBytes(client_->device().name));
    updateInputState();
}

void MainWindow::updateInputState() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const bool canType = ready && current_.isValid();
    const bool becameAvailable = canType && !input_->isEnabled();
    input_->setEnabled(canType);
    // No length test: the counter holds the box inside the budget, so whatever
    // is in it fits.
    sendAction_->setEnabled(canType && !input_->text().trimmed().isEmpty());
    input_->setPlaceholderText(canType ? QStringLiteral("Message")
                               : ready ? QStringLiteral("Select a conversation")
                                       : QStringLiteral("Waiting for a connection..."));
    if (becameAvailable) input_->setFocus(Qt::OtherFocusReason);
}

void MainWindow::updateConversationActions() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    // A direct conversation takes no slot, so a device with all eight channels
    // in use still has something to offer here; it is the channel items inside
    // the menu that go grey.
    addButton_->setEnabled(ready);
    addButton_->setToolTip(ready ? QStringLiteral("Start a conversation")
                                 : QStringLiteral("Connect to start a conversation"));

    const std::optional<model::Channel> channel = currentChannelOnDevice();
    // Any channel can be shared, including the public ones nobody needs an
    // invitation to: what stops it is not having the key, which is the case
    // whenever the list on screen came from the offline cache. A peer has no
    // key of ours to pass on -- an invitation to a conversation with somebody
    // is their contact card, which is #22.
    shareChannelButton_->setEnabled(channel.has_value());
    shareChannelButton_->setToolTip(
        current_.isDirect() ? QStringLiteral("Only a channel can be shared")
        : !ready            ? QStringLiteral("Connect to share a channel")
        : !channel          ? QStringLiteral("Select a channel to share")
                            : QStringLiteral("Share %1").arg(channel->displayName()));

    // Removing a channel is a write to the device; removing a direct
    // conversation only deletes this app's copy of it, which works offline.
    const model::ConversationEntry* entry = conversationModel_->entry(current_);
    const bool removable =
        current_.isDirect() ? entry != nullptr : (channel && isRemovable(*channel));
    removeButton_->setEnabled(removable);
    removeButton_->setToolTip(
        current_.isDirect() && entry
            ? QStringLiteral("Delete the conversation with %1").arg(entry->name)
        : !ready    ? QStringLiteral("Connect to remove a channel")
        : !channel  ? QStringLiteral("Select a conversation to remove")
        : removable ? QStringLiteral("Remove %1").arg(channel->displayName())
                    : QStringLiteral("The Public channel cannot be removed"));
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void MainWindow::updateReadyStatus() {
    if (client_->state() != proto::CompanionClient::State::Ready) return;

    const QString label = contactsSyncing_
                              ? QStringLiteral("syncing contacts")
                          : messagesSyncing_ ? QStringLiteral("syncing messages")
                                             : QStringLiteral("connected");
    const QColor color = contactsSyncing_ || messagesSyncing_ ? theme::Warning
                                                               : theme::Accent;
    nodePane_->setConnection(label, color, client_->isRunning(), true);
}

void MainWindow::onStateChanged(proto::CompanionClient::State state, const QString& detail) {
    using State = proto::CompanionClient::State;

    // One preflight per handshake, and no retry inside a live session: opening
    // a database successfully does not promise that inserting into it will
    // work, so a retry that guesses wrong costs another message to find out --
    // asking for one is what destroys the node's copy. Rebuilding the link is
    // the deliberate act that gets storage another chance.
    if (state != State::Ready) {
        storagePreflighted_ = false;
        contactsSyncing_ = false;
        messagesSyncing_ = false;
    }

    QString label;
    QColor color = theme::TextMuted;
    switch (state) {
        case State::Disconnected:
            label = detail.isEmpty() ? QStringLiteral("disconnected") : detail;
            color = theme::Error;
            break;
        case State::Connecting:
            // Whatever the transport is up to — a BLE scan reports its own
            // progress, and it is worth showing rather than a spinner-in-words.
            label = detail.isEmpty() ? QStringLiteral("connecting") : detail;
            color = theme::Warning;
            break;
        case State::Handshaking:
            label = QStringLiteral("syncing");
            color = theme::Warning;
            break;
        case State::Ready:
            // The client deliberately makes channels usable before the full
            // contact stream is published. Keep the link active, but say what
            // the remaining startup work is until contactsChanged arrives.
            contactsSyncing_ = true;
            label = QStringLiteral("syncing contacts");
            color = theme::Warning;
            break;
    }
    nodePane_->setConnection(label, color, client_->isRunning(), state == State::Ready);
    updateInputState();
    updateConversationActions();
}

void MainWindow::onDeviceInfo(const proto::CompanionClient::DeviceInfo& info) {
    if (info.pubkey.size() == 32 && info.pubkey != activeDeviceId_) {
        // The endpoint may now serve a different radio. Switch storage scopes
        // on the cryptographic identity, then show only that device's cache
        // while its live channel enumeration completes.
        activeDeviceId_ = info.pubkey;
        current_ = {};
        conversationModel_->clearTransientState();
        conversationModel_->setChannels({});
        conversationModel_->setDirectConversations({});
        chatModel_->setMessages({});
        loadCachedChannels();
        updateHeader();
        updateInputState();
        updateConversationActions();
    }
    // SELF_INFO is the first thing a handshake answers and channel enumeration
    // -- which ends by starting the inbox drain -- is queued behind it, so this
    // is the last moment before the app can be handed a message, and the
    // earliest at which it knows which database that message belongs in. It is
    // also reached again on every battery poll, which is not a reason to
    // preflight again.
    if (!storagePreflighted_) preflightStorage();
    setWindowTitle(windowTitleFor(info.name));
    nodePane_->setDevice(info);
    // The node prepends its name to every channel message it sends, so the name
    // is part of the message budget: learning it, or moving to a node called
    // something else, changes how much room the counter should be promising.
    updateMessageBudget();
}
