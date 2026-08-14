#include "ui/main_window.h"

#include <QAction>
#include <QCloseEvent>
#include <QDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "model/channel_model.h"
#include "model/chat_model.h"
#include "ui/add_channel_dialog.h"
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

QString historyPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/history-v2.jsonl");
}

QString deviceSettingsGroup(const QByteArray& deviceId) {
    return QStringLiteral("devices/%1").arg(QString::fromLatin1(deviceId.toHex()));
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
QToolButton* headerButton(const QString& icon, const QColor& hover, qreal dpr) {
    constexpr int IconSize = 14;
    QIcon set;
    set.addPixmap(icons::tinted(icon, IconSize, theme::TextMuted, dpr), QIcon::Normal);
    set.addPixmap(icons::tinted(icon, IconSize, hover, dpr), QIcon::Active);
    set.addPixmap(icons::tinted(icon, IconSize, theme::Border, dpr), QIcon::Disabled);

    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("iconButton"));
    button->setIcon(set);
    button->setIconSize(QSize(IconSize, IconSize));
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
    : QMainWindow(parent), history_(historyPath()) {
    setWindowTitle(QStringLiteral("MeshCore"));
    history_.load();

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
    // These legacy values had no device or channel-key scope and cannot be
    // assigned safely. Stop carrying them forward; history.jsonl itself stays
    // untouched as a recoverable legacy file.
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
    channelModel_->clearTransientState();
    channelModel_->setChannels({});
    chatModel_->setMessages({});
    setWindowTitle(QStringLiteral("MeshCore"));
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

void MainWindow::openAddChannelDialog() {
    // The device holds the channel list and the keys, so adding one is a write
    // to it — and the dialog needs those keys to recognise a channel that is
    // already joined. Neither works from the offline cache.
    if (client_->state() != proto::CompanionClient::State::Ready) return;

    AddChannelDialog dialog(client_->channels(), this);
    if (dialog.exec() != QDialog::Accepted) return;

    const model::Channel ch = dialog.channel();
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

    constexpr int RemoveIconSize = 40;
    auto* icon = new QLabel;
    icon->setPixmap(icons::tinted(QStringLiteral("trash-2"), RemoveIconSize, theme::Error,
                                  devicePixelRatioF()));
    icon->setFixedSize(RemoveIconSize, RemoveIconSize);
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
    QFont headerFont = channelsTitle->font();
    headerFont.setPointSizeF(qMax(6.5, headerFont.pointSizeF() - 1.5));
    headerFont.setBold(true);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    channelsTitle->setFont(headerFont);
    channelsTitle->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    const qreal dpr = devicePixelRatioF();
    addChannelButton_ = headerButton(QStringLiteral("plus"), theme::Accent, dpr);
    // Share and remove both act on the selected row rather than carrying one of
    // their own: per-row buttons would cost sidebar width the uConsole has not
    // got, and hover affordances are no use on a trackball.
    shareChannelButton_ = headerButton(QStringLiteral("share"), theme::Accent, dpr);
    removeChannelButton_ = headerButton(QStringLiteral("minus"), theme::Error, dpr);

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
    // A mesh payload is 184 bytes; the daemon would reject anything longer, so
    // stop it at the keyboard instead of after a failed transmit.
    input_->setMaxLength(proto::MaxMessageChars);

    constexpr int SendIconSize = 16;
    QIcon sendIcon;
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), SendIconSize, theme::Accent, dpr),
                       QIcon::Normal);
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), SendIconSize, theme::Text, dpr),
                       QIcon::Active);
    sendIcon.addPixmap(icons::tinted(QStringLiteral("send"), SendIconSize, theme::Border, dpr),
                       QIcon::Disabled);
    sendAction_ = input_->addAction(sendIcon, QLineEdit::TrailingPosition);
    sendAction_->setObjectName(QStringLiteral("sendAction"));
    sendAction_->setText(QStringLiteral("Send message"));
    sendAction_->setToolTip(QStringLiteral("Send message"));

    charCount_ = new QLabel;
    QFont countFont = charCount_->font();
    countFont.setPointSizeF(qMax(6.5, countFont.pointSizeF() - 1.5));
    charCount_->setFont(countFont);
    charCount_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));
    charCount_->setMinimumWidth(28);
    charCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    inputLayout->addWidget(input_);
    inputLayout->addWidget(charCount_, 0, Qt::AlignRight);

    // Sits above the message box, where the eye already is when a send fails,
    // and takes no room at all the rest of the time.
    notice_ = new ElidedLabel;
    notice_->setObjectName(QStringLiteral("notice"));
    notice_->setFont(countFont);
    notice_->hide();

    noticeTimer_ = new QTimer(this);
    noticeTimer_->setSingleShot(true);
    connect(noticeTimer_, &QTimer::timeout, notice_, &QWidget::hide);

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
    connect(addChannelButton_, &QToolButton::clicked, this, &MainWindow::openAddChannelDialog);
    connect(shareChannelButton_, &QToolButton::clicked, this, &MainWindow::shareCurrentChannel);
    connect(removeChannelButton_, &QToolButton::clicked, this, &MainWindow::removeCurrentChannel);
    connect(channelList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onChannelSelected);
    connect(sendAction_, &QAction::triggered, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::returnPressed, this, &MainWindow::onSendClicked);
    connect(input_, &QLineEdit::textChanged, this, &MainWindow::onTextChanged);

    // The uConsole panel is 1280x480; this is a sane default anywhere else.
    resize(1024, 480);
    onTextChanged({});
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
        const QVector<model::Message> msgs =
            history_.messages(activeDeviceId_, ch.keyFingerprint(), ch.index);
        if (!msgs.isEmpty()) channelModel_->setLastMessage(ch.index, msgs.last());
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
    history_.remove(activeDeviceId_, channelKey);
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
    chatModel_->setMessages(channelIndex >= 0 && activeDeviceId_.size() == 32
                                ? history_.messages(activeDeviceId_, channelKey, channelIndex)
                                : QVector<model::Message>());
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

    // Lay out first, then jump to the newest message: scrolling before the
    // delegate has sized the rows lands in the wrong place.
    messageDelegate_->setViewportWidth(chatView_->viewport()->width());
    QTimer::singleShot(0, this, [this] { chatView_->scrollToBottom(); });
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
    header_->setText(QStringLiteral("<b>%1</b> <span style=\"color: %2;\">slot %3</span>")
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
    notice_->hide();
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
    if (activeDeviceId_.size() != 32 || channelKey.size() != 32) return;

    history_.append(activeDeviceId_, channelKey, msg);
    channelModel_->setLastMessage(msg.channelIndex, msg);

    if (msg.channelIndex == currentChannel_)
        appendToView(msg);
    else
        channelModel_->bumpUnread(msg.channelIndex);
}

void MainWindow::onDirectMessageReceived(const model::Message& msg) {
    // v1 has no DM view, but SYNC_NEXT_MESSAGE already popped this from the
    // daemon's inbox: not writing it down would destroy it. It goes to the same
    // history file under channel -1, ready for whenever DMs are built out.
    if (activeDeviceId_.size() != 32) return;
    history_.append(activeDeviceId_, {}, msg);
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

    chatModel_->append(msg);
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
    history_.append(pending.deviceId, pending.channelKey, msg);

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

void MainWindow::onTextChanged(const QString& text) {
    constexpr int WarningThreshold = 10;
    const int left = proto::MaxMessageChars - int(text.size());
    charCount_->setText(QStringLiteral("%1/%2").arg(left).arg(proto::MaxMessageChars));
    charCount_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg((left <= WarningThreshold ? theme::Warning : theme::TextMuted).name()));
    updateInputState();
}

void MainWindow::updateInputState() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const bool canType = ready && currentChannel_ >= 0;
    input_->setEnabled(canType);
    sendAction_->setEnabled(canType && !input_->text().trimmed().isEmpty());
    input_->setPlaceholderText(canType ? QStringLiteral("Message")
                               : ready ? QStringLiteral("Select a channel")
                                       : QStringLiteral("Waiting for a connection..."));
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
    setWindowTitle(info.name.isEmpty() ? QStringLiteral("MeshCore")
                                       : QStringLiteral("MeshCore — %1").arg(info.name));
    nodePane_->setDevice(info);
}
