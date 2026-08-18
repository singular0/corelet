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

#include "model/channel_model.h"
#include "model/chat_model.h"
#include "protocol/text_limits.h"
#include "ui/add_channel_dialogs.h"
#include "ui/byte_limit.h"
#include "ui/channel_delegate.h"
#include "ui/connect_dialog.h"
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
    currentChannel_ = -1;
    // The new session preflights its own database, and that is what decides
    // whether there is still a fault to report.
    clearStorageFault();
    hideNotice();
    channelModel_->clearTransientState();
    channelModel_->setChannels({});
    chatModel_->setMessages({});
    setWindowTitle(windowTitleFor({}));
    nodePane_->setDevice({});
    updateHeader();
    updateInputState();
    updateChannelActions();

    target_ = target;
    nodePane_->setTarget(target);
    client_->start(proto::createTransport(target));
}

void MainWindow::openConnectDialog() {
    ConnectDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    connectTo(dialog.target());
}

void MainWindow::showAddChannelMenu() {
    // The device holds the channel list and the keys, so adding one is a write
    // to it — and every item below needs those keys, to recognise a channel that
    // is already joined. Neither works from the offline cache.
    if (client_->state() != proto::CompanionClient::State::Ready) return;

    // Taken by value: each dialog runs an event loop, and a reconnect
    // re-enumerating the channels underneath it would leave the reference it
    // was built from dangling.
    const QVector<model::Channel> existing = client_->channels();
    if (!AddChannelDialog::hasFreeSlot(existing)) return;

    // The kind of channel is chosen before anything is typed, so it is a menu
    // rather than a control inside a dialog. The icons are the ones the sidebar
    // paints for each kind, which is what the new row will look like.
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
    // disappearing, so the menu keeps the same four items in the same order
    // whatever the device holds.
    joinPublic->setEnabled(!AddChannelDialog::publicChannelJoined(existing));

    QAction* chosen =
        menu.exec(addChannelButton_->mapToGlobal(QPoint(0, addChannelButton_->height())));

    // A popup holds the mouse grab for as long as it is up, so the button that
    // opened it never sees the pointer leave: dismissed with a click somewhere
    // else, the '+' would stay lit as though the cursor were still on it. Only
    // when the pointer really has moved off -- clearing the flag under it would
    // unlight a button the cursor is still on and leave it that way, since no
    // second enter is coming.
    if (!addChannelButton_->rect().contains(
            addChannelButton_->mapFromGlobal(QCursor::pos()))) {
        addChannelButton_->setAttribute(Qt::WA_UnderMouse, false);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(addChannelButton_, &leave);
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

void MainWindow::shareCurrentChannel() {
    // Only the device's list carries keys, and a key is the entire invitation:
    // the cached list the sidebar falls back on while offline has nothing to
    // share.
    const std::optional<model::Channel> ch = currentChannelOnDevice();
    if (!ch) return;

    ShareChannelDialog(*ch, this).exec();
}

void MainWindow::removeCurrentChannel() {
    // Taken by value: the dialog below runs an event loop, and a reconnect
    // re-enumerating the channels underneath it would leave a reference to the
    // client's vector dangling.
    const std::optional<model::Channel> ch = currentChannelOnDevice();
    if (!ch || !isRemovable(*ch)) return;

    // The key lives on the device and nowhere else -- the app caches names only
    // -- so this is the last chance to say so.
    QDialog dialog(this);
    ui::configureDialogWindow(dialog);
    dialog.setWindowTitle(QStringLiteral("Remove channel"));

    const int removeIconSize = theme::scaled(dialog.font(), 40);
    auto* icon = new QLabel;
    icon->setPixmap(icons::tinted(QStringLiteral("trash-2"), removeIconSize, theme::Error,
                                  devicePixelRatioF()));
    icon->setFixedSize(removeIconSize, removeIconSize);
    icon->setAlignment(Qt::AlignTop);

    const QString impactItems =
        ch->type == model::ChannelType::Private
            ? QStringLiteral("<li>Channel's key will be deleted the device.</li>"
                             "<li>Channel's messages history will be deleted from this app.</li>"
                             "<li>You need a copy of the channel key to rejoin.</li>")
            : QStringLiteral("<li>Channel's key will be deleted the device.</li>"
                             "<li>Channel's messages history will be deleted from this app.</li>"
                             "<li>You can rejoin the channel any time.</li>");
    auto* content = new QLabel(
        QStringLiteral("<p style=\"margin: 0;\">Remove <b>%1</b>?</p>"
                       "<ul style=\"margin-top: 8px; margin-bottom: 0; margin-left: 16px; "
                       "margin-right: 0; -qt-list-indent: 0; color: %2;\">%3</ul>")
            .arg(ch->displayName().toHtmlEscaped(), theme::TextMuted.name(), impactItems));
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
    if (dialog.exec() != QDialog::Accepted) return;

    showNotice(QStringLiteral("Removing %1...").arg(ch->displayName()), 10000);
    pendingChannelRemovals_.insert(ch->index, ch->keyFingerprint());
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
    auto* channelsHeader = new QWidget;
    channelsHeader->setObjectName(QStringLiteral("sidebarHeader"));
    auto* channelsHeaderLayout = new QHBoxLayout(channelsHeader);
    channelsHeaderLayout->setContentsMargins(10, 4, 6, 4);
    channelsHeaderLayout->setSpacing(4);

    auto* channelsTitle = new QLabel(QStringLiteral("CHANNELS"));
    QFont headerFont = theme::secondaryFont(channelsTitle->font());
    headerFont.setBold(true);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    channelsTitle->setFont(headerFont);
    channelsTitle->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    const qreal dpr = devicePixelRatioF();
    const int headerIconSize = theme::scaled(font(), 14);
    addChannelButton_ =
        headerButton(QStringLiteral("plus"), theme::Accent, headerIconSize, dpr);
    // Share and remove both act on the selected row rather than carrying one of
    // their own: per-row buttons would cost sidebar width the uConsole has not
    // got, and hover affordances are no use on a trackball.
    shareChannelButton_ =
        headerButton(QStringLiteral("share"), theme::Accent, headerIconSize, dpr);
    removeChannelButton_ =
        headerButton(QStringLiteral("minus"), theme::Error, headerIconSize, dpr);

    channelsHeaderLayout->addWidget(channelsTitle, 1);
    channelsHeaderLayout->addWidget(shareChannelButton_);
    channelsHeaderLayout->addWidget(removeChannelButton_);
    channelsHeaderLayout->addWidget(addChannelButton_);

    channelModel_ = new model::ChannelModel(this);
    channelList_ = new QListView;
    channelList_->setObjectName(QStringLiteral("channelList"));
    channelList_->setModel(channelModel_);
    channelList_->setItemDelegate(new ChannelDelegate(channelList_));
    channelList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    channelList_->setSelectionMode(QAbstractItemView::SingleSelection);
    channelList_->setFocusPolicy(Qt::NoFocus);

    nodePane_ = new NodePane;

    leftLayout->addWidget(channelsHeader);
    leftLayout->addWidget(channelList_, 1);
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
    connect(addChannelButton_, &QToolButton::clicked, this, &MainWindow::showAddChannelMenu);
    connect(shareChannelButton_, &QToolButton::clicked, this, &MainWindow::shareCurrentChannel);
    connect(removeChannelButton_, &QToolButton::clicked, this, &MainWindow::removeCurrentChannel);
    connect(channelList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onChannelSelected);
    connect(sendAction_, &QAction::triggered, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);
    // Connected after the counter's own, so this reads a field already held
    // inside its budget. All it decides is whether there is anything to send.
    connect(input_, &QLineEdit::textChanged, this, [this] { updateInputState(); });

    // The uConsole panel is 1280x480; this is a sane default anywhere else.
    resize(1024, 480);
    updateInputState();
    updateChannelActions();
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
    if (!channels.isEmpty()) showChannels(channels);
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
    QByteArray wantedKey = currentChannelKey();
    if (wantedKey.isEmpty() && activeDeviceId_.size() == 32) {
        QSettings settings;
        settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
        wantedKey = QByteArray::fromHex(
            settings.value(QStringLiteral("selectedChannel")).toString().toLatin1());
        settings.endGroup();
    }

    channelModel_->setChannels(channels);

    for (const model::Channel& ch : channels) {
        const model::HistoryLatest latest =
            history_.latestMessage(activeDeviceId_, ch.keyFingerprint(), ch.index);
        // One broken database breaks all eight lookups; say so once and stop
        // asking rather than paint the sidebar as eight empty conversations.
        if (!latest.result) {
            onStorageFailure(QStringLiteral("read the message history"), latest.result);
            break;
        }
        if (latest.message) channelModel_->setLastMessage(ch.index, *latest.message);
    }

    // Selection follows the channel key, not its former wire slot. If that
    // channel is gone, fall back to the first channel on this device.
    int wantedRow = channelModel_->rowForKey(wantedKey);
    if (wantedRow < 0 && !channels.isEmpty()) wantedRow = 0;
    const int wanted = wantedRow < 0 ? -1 : channelModel_->channelIndexForRow(wantedRow);

    // Force showChannel() to reload: the rows are new, and the open channel may
    // be gone from the list altogether -- removed, or dropped by the device --
    // in which case selectChannel() has nothing to open and the pane is left
    // showing a conversation that no longer belongs to anything.
    currentChannel_ = -1;
    chatModel_->setMessages({});
    selectChannel(wanted);
    updateHeader();
    updateInputState();
    updateChannelActions();
}

void MainWindow::onChannelSaveResult(int channelIndex, bool ok, const QString& error) {
    if (!ok) {
        showNotice(QStringLiteral("Could not add the channel: %1").arg(error), 8000, true);
        return;
    }

    // The sidebar was rebuilt before this arrived, so the slot is there to open.
    // The user asked for it a moment ago; showing it is what they meant.
    hideNotice();
    selectChannel(channelIndex);
    updateChannelActions();
}

void MainWindow::onChannelRemoveResult(int channelIndex, bool ok, const QString& error) {
    const QByteArray channelKey = pendingChannelRemovals_.take(channelIndex);
    if (!ok) {
        showNotice(QStringLiteral("Could not remove the channel: %1").arg(error), 8000, true);
        return;
    }

    // The sidebar was rebuilt before this arrived and has already moved off the
    // channel. Its key fingerprint was retained before the slot was cleared so
    // only this device's copy of that channel history is removed.
    hideNotice();
    const model::HistoryResult forgotten = history_.remove(activeDeviceId_, channelKey);
    // The channel is gone from the device either way, so this is not a failed
    // removal -- it is messages left behind for a channel that can no longer be
    // opened, which the user is owed the chance to clear up.
    if (!forgotten)
        showNotice(QStringLiteral("Channel removed, but its messages could not be deleted: %1")
                       .arg(forgotten.error),
                   8000, true);
    channelModel_->forget(channelKey);
    updateChannelActions();
}

void MainWindow::selectChannel(int channelIndex) {
    const int row = channelModel_->rowForIndex(channelIndex);
    if (row < 0) {
        showChannel(-1);
        return;
    }
    channelList_->setCurrentIndex(channelModel_->index(row));
    // A reconnect resets the channel model, but the selected slot may land on
    // the same row as before. Do not depend on QItemSelectionModel deciding
    // that this is a current-index change: the refreshed channel list must
    // always reload its conversation from the updated history.
    showChannel(channelIndex);
}

void MainWindow::onChannelSelected(const QModelIndex& current, const QModelIndex&) {
    showChannel(current.isValid() ? channelModel_->channelIndexForRow(current.row()) : -1);
}

void MainWindow::showChannel(int channelIndex) {
    if (channelIndex == currentChannel_) return;
    currentChannel_ = channelIndex;

    const QByteArray channelKey = currentChannelKey();
    const int unseenCount = channelModel_->unreadCount(channelIndex);
    QVector<model::Message> conversation;
    if (channelIndex >= 0 && activeDeviceId_.size() == 32) {
        model::HistoryMessages stored =
            history_.messages(activeDeviceId_, channelKey, channelIndex);
        // An unreadable conversation must not be shown as an empty one: a
        // database that stopped opening would otherwise look exactly like a
        // channel nobody has said anything on.
        if (stored.result)
            conversation = std::move(stored.messages);
        else
            onStorageFailure(QStringLiteral("read the conversation"), stored.result);
    }
    chatModel_->setMessages(conversation, unseenCount);
    channelModel_->clearUnread(channelIndex);
    if (channelIndex >= 0 && !channelKey.isEmpty() && activeDeviceId_.size() == 32) {
        QSettings settings;
        settings.beginGroup(deviceSettingsGroup(activeDeviceId_));
        settings.setValue(QStringLiteral("selectedChannel"),
                          QString::fromLatin1(channelKey.toHex()));
        settings.endGroup();
    }
    updateHeader();
    updateInputState();
    // Removing acts on the selection, so the button follows it.
    updateChannelActions();

    // Lay out first, then show the boundary of anything that arrived while the
    // channel was closed. With no unseen messages, retain the usual newest-row
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

QByteArray MainWindow::currentChannelKey() const {
    return currentChannel_ < 0 ? QByteArray() : channelModel_->keyForIndex(currentChannel_);
}

std::optional<model::Channel> MainWindow::currentChannelOnDevice() const {
    // Only the device's own list carries keys, and the type is derived from the
    // key: the cached list the sidebar shows while offline cannot answer this.
    if (client_->state() != proto::CompanionClient::State::Ready || currentChannel_ < 0)
        return std::nullopt;
    for (const model::Channel& ch : client_->channels())
        if (ch.index == currentChannel_) return ch;
    return std::nullopt;
}

void MainWindow::updateHeader() {
    if (currentChannel_ < 0) {
        header_->setText(channelModel_->rowCount() == 0
                             ? QStringLiteral("<b>No channels</b>")
                             : QStringLiteral("<b>Select a channel</b>"));
        return;
    }

    // Only the conversation's own name and its slot: who we are and what the
    // radio is doing belong to the node pane, which says it once instead of on
    // every channel. The slot is useful when cross-checking against the daemon,
    // though persistence follows the channel key rather than this wire address.
    const int row = channelModel_->rowForIndex(currentChannel_);
    const QString name = channelModel_->data(channelModel_->index(row),
                                             model::ChannelModel::NameRole).toString();
    header_->setText(QStringLiteral("<b>%1</b> <span style=\"color: %2;\">[%3]</span>")
                         .arg(name.toHtmlEscaped(), theme::TextMuted.name())
                         .arg(currentChannel_));
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
    QByteArray channelKey = channelModel_->keyForIndex(msg.channelIndex);
    // The live list is authoritative if a message races a UI reset. The
    // protocol has already popped the message, so losing it merely because its
    // row is not painted yet would be data loss.
    if (channelKey.isEmpty()) {
        for (const model::Channel& channel : client_->channels()) {
            if (channel.index == msg.channelIndex) {
                channelKey = channel.keyFingerprint();
                break;
            }
        }
    }

    // Whatever the app could not work out about where this belongs, the node no
    // longer has a copy of it. An unplaceable message is stored in the reserved
    // conversation for its slot instead of being thrown away, and the retry is
    // whoever reads it back later, not the mesh.
    const bool knownDevice = activeDeviceId_.size() == 32;
    const bool knownChannel = channelKey.size() == 32;
    const QByteArray deviceId =
        knownDevice ? activeDeviceId_ : model::History::orphanDeviceId();
    const QByteArray conversation =
        knownChannel ? channelKey : model::History::orphanChannel(msg.channelIndex);

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

    channelModel_->setLastMessage(msg.channelIndex, msg);

    if (msg.channelIndex == currentChannel_)
        appendToView(msg);
    else
        channelModel_->bumpUnread(msg.channelIndex);
}

void MainWindow::onDirectMessageReceived(const model::Message& msg) {
    // v1 has no DM view, but SYNC_NEXT_MESSAGE already popped this from the
    // daemon's inbox: not writing it down would destroy it. It goes to the same
    // device database under channel -1, ready for whenever DMs are built out.
    const bool knownDevice = activeDeviceId_.size() == 32;
    const model::HistoryResult stored = history_.append(
        knownDevice ? activeDeviceId_ : model::History::orphanDeviceId(), {}, msg);
    if (!stored) {
        onStorageFailure(QStringLiteral("save a received direct message"), stored);
        return;
    }

    // Counted only once it is on disk, so the number on screen is the number
    // that can still be read back.
    directMessageCount_++;
    showNotice(QStringLiteral("%1 direct message(s) received and saved — no DM view yet")
                   .arg(directMessageCount_),
               8000);
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
    if (text.isEmpty() || currentChannel_ < 0) return;
    if (client_->state() != proto::CompanionClient::State::Ready) return;
    const QByteArray channelKey = currentChannelKey();
    if (activeDeviceId_.size() != 32 || channelKey.size() != 32) return;

    input_->clear();

    // Our own transmission never comes back to us over the air, so the message
    // is echoed locally or it would never appear at all. It goes up marked as
    // pending straight away -- the daemon answering is what a reader is waiting
    // to know about, and an empty pane while it does says nothing.
    model::Message msg;
    msg.channelIndex = currentChannel_;
    msg.sender = client_->device().name;
    msg.text = text;
    msg.timestamp = QDateTime::currentDateTime();
    msg.outgoing = true;
    msg.sendState = model::Message::SendState::Pending;
    msg.sendToken = ++lastSendToken_;

    // Registered before the command goes out: a send refused on the spot answers
    // from inside the call below.
    pendingSends_.insert(msg.sendToken, {msg, activeDeviceId_, channelKey});
    appendToView(msg);
    client_->sendChannelMessage(msg.channelIndex, msg.text, msg.sendToken);
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
        if (pending.deviceId == activeDeviceId_ && pending.channelKey == currentChannelKey() &&
            input_->text().isEmpty())
            input_->setText(msg.text);
        return;
    }

    // Only now is it worth writing down. History is what the app has instead of
    // the daemon's inbox, and a message that never left has no business in it.
    msg.sendState = model::Message::SendState::Sent;
    msg.sendToken = 0;
    const model::HistoryResult stored = history_.append(pending.deviceId, pending.channelKey, msg);
    // The message did go out, so it stays on screen; what it says about storage
    // is the same as any other failed write, and collection stops on it.
    if (!stored) onStorageFailure(QStringLiteral("save a sent message"), stored);

    const bool stillShowingDevice = pending.deviceId == activeDeviceId_;
    const int row = stillShowingDevice ? channelModel_->rowForKey(pending.channelKey) : -1;
    if (row >= 0) {
        msg.channelIndex = channelModel_->channelIndexForRow(row);
        channelModel_->setLastMessage(msg.channelIndex, msg);
    }
    // The row it went up as is gone if the conversation was reloaded meanwhile.
    // Put the message back when that happened to the channel being looked at;
    // any other channel reads it from history when it is opened.
    if (!chatModel_->markSent(token) && stillShowingDevice &&
        pending.channelKey == currentChannelKey())
        appendToView(msg);
}

// The node prepends its own name to every channel message it sends, so what is
// left for the body moves with that name: connecting, or moving to a node called
// something else, is what makes this a budget rather than a constant.
void MainWindow::updateMessageBudget() {
    messageLimit_->setBudget(proto::maxMessageBytes(client_->device().name));
    updateInputState();
}

void MainWindow::updateInputState() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const bool canType = ready && currentChannel_ >= 0;
    const bool becameAvailable = canType && !input_->isEnabled();
    input_->setEnabled(canType);
    // No length test: the counter holds the box inside the budget, so whatever
    // is in it fits.
    sendAction_->setEnabled(canType && !input_->text().trimmed().isEmpty());
    input_->setPlaceholderText(canType ? QStringLiteral("Message")
                               : ready ? QStringLiteral("Select a channel")
                                       : QStringLiteral("Waiting for a connection..."));
    if (becameAvailable) input_->setFocus(Qt::OtherFocusReason);
}

void MainWindow::updateChannelActions() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const bool room = AddChannelDialog::hasFreeSlot(client_->channels());
    addChannelButton_->setEnabled(ready && room);
    addChannelButton_->setToolTip(!room ? QStringLiteral("All %1 channel slots are in use")
                                              .arg(proto::MaxChannels)
                                 : ready ? QStringLiteral("Add a channel")
                                         : QStringLiteral("Connect to add a channel"));

    const std::optional<model::Channel> current = currentChannelOnDevice();
    // Any channel can be shared, including the public ones nobody needs an
    // invitation to: what stops it is not having the key, which is the case
    // whenever the list on screen came from the offline cache.
    shareChannelButton_->setEnabled(current.has_value());
    shareChannelButton_->setToolTip(
        !ready     ? QStringLiteral("Connect to share a channel")
        : !current ? QStringLiteral("Select a channel to share")
                   : QStringLiteral("Share %1").arg(current->displayName()));

    const bool removable = current && isRemovable(*current);
    removeChannelButton_->setEnabled(removable);
    removeChannelButton_->setToolTip(
        !ready     ? QStringLiteral("Connect to remove a channel")
        : !current ? QStringLiteral("Select a channel to remove")
        : removable
            ? QStringLiteral("Remove %1").arg(current->displayName())
            : QStringLiteral("The Public channel cannot be removed"));
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void MainWindow::onStateChanged(proto::CompanionClient::State state, const QString& detail) {
    using State = proto::CompanionClient::State;

    // One preflight per handshake, and no retry inside a live session: opening
    // a database successfully does not promise that inserting into it will
    // work, so a retry that guesses wrong costs another message to find out --
    // asking for one is what destroys the node's copy. Rebuilding the link is
    // the deliberate act that gets storage another chance.
    if (state != State::Ready) storagePreflighted_ = false;

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
            label = QStringLiteral("connected");
            color = theme::Accent;
            break;
    }
    nodePane_->setConnection(label, color, client_->isRunning(), state == State::Ready);
    updateInputState();
    updateChannelActions();
}

void MainWindow::onDeviceInfo(const proto::CompanionClient::DeviceInfo& info) {
    if (info.pubkey.size() == 32 && info.pubkey != activeDeviceId_) {
        // The endpoint may now serve a different radio. Switch storage scopes
        // on the cryptographic identity, then show only that device's cache
        // while its live channel enumeration completes.
        activeDeviceId_ = info.pubkey;
        currentChannel_ = -1;
        channelModel_->clearTransientState();
        channelModel_->setChannels({});
        chatModel_->setMessages({});
        loadCachedChannels();
        updateHeader();
        updateInputState();
        updateChannelActions();
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
