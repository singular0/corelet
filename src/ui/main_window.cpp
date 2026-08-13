#include "ui/main_window.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
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
#include "ui/elided_label.h"
#include "ui/icons.h"
#include "ui/message_delegate.h"
#include "ui/node_pane.h"
#include "ui/theme.h"

namespace {

QString historyPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/history.jsonl");
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

    // Paint the sidebar and history straight away rather than after the
    // handshake; the device's list replaces this as soon as it arrives.
    loadCachedChannels();

    connectTo(target);
}

void MainWindow::connectTo(const proto::ConnectTarget& target) {
    if (!target.isValid()) return;

    // History and the channel cache are not keyed by device: pointing the app
    // at a second node mixes its channels into the same history file. v1
    // assumes one radio, which is what a companion app usually is.
    target_ = target;
    nodePane_->setTarget(target.label());
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

void MainWindow::removeCurrentChannel() {
    // Taken by value: the dialog below runs an event loop, and a reconnect
    // re-enumerating the channels underneath it would leave a reference to the
    // client's vector dangling.
    const std::optional<model::Channel> ch = currentChannelOnDevice();
    if (!ch || !isRemovable(*ch)) return;

    // The key lives on the device and nowhere else -- the app caches names only
    // -- so this is the last chance to say so.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Remove channel"));
    box.setText(QStringLiteral("Remove \"%1\"?").arg(ch->displayName()));
    box.setInformativeText(
        ch->type == model::ChannelType::Private
            ? QStringLiteral("The key is deleted from the device and its messages are removed "
                             "from this app. Without a copy of the key the channel cannot be "
                             "joined again.")
            : QStringLiteral("The channel is deleted from the device and its messages are "
                             "removed from this app. It can be added again at any time."));
    QPushButton* confirm = box.addButton(QStringLiteral("Remove"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != confirm) return;

    showNotice(QStringLiteral("Removing %1...").arg(ch->displayName()), 10000);
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
    // The minus acts on the selected row rather than carrying a row of its own:
    // a per-row delete button would cost sidebar width the uConsole has not got,
    // and hover affordances are no use on a trackball.
    removeChannelButton_ = headerButton(QStringLiteral("minus"), theme::Error, dpr);

    channelsHeaderLayout->addWidget(channelsTitle, 1);
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
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(8, 6, 8, 6);
    inputLayout->setSpacing(6);

    input_ = new QLineEdit;
    input_->setPlaceholderText(QStringLiteral("Message"));
    // A mesh payload is 184 bytes; the daemon would reject anything longer, so
    // stop it at the keyboard instead of after a failed transmit.
    input_->setMaxLength(proto::MaxMessageChars);

    charCount_ = new QLabel;
    QFont countFont = charCount_->font();
    countFont.setPointSizeF(qMax(6.5, countFont.pointSizeF() - 1.5));
    charCount_->setFont(countFont);
    charCount_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));
    charCount_->setMinimumWidth(28);
    charCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    sendButton_ = new QPushButton(QStringLiteral("Send"));
    sendButton_->setDefault(true);

    inputLayout->addWidget(input_, 1);
    inputLayout->addWidget(charCount_);
    inputLayout->addWidget(sendButton_);

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
    connect(addChannelButton_, &QToolButton::clicked, this, &MainWindow::openAddChannelDialog);
    connect(removeChannelButton_, &QToolButton::clicked, this, &MainWindow::removeCurrentChannel);
    connect(channelList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            &MainWindow::onChannelSelected);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::onSendClicked);
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
    settings.setValue(QStringLiteral("channel"), currentChannel_);
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
    // Remember the list so the next launch can show the sidebar and history
    // immediately, and keep showing them while the daemon is restarting. Only
    // names are cached: the channel keys stay in the daemon's state directory
    // rather than being copied into a settings file. The type goes with them
    // because it is derived from a key the cache will not have.
    QStringList cached;
    for (const model::Channel& ch : channels)
        cached << QStringLiteral("%1\x1f%2\x1f%3").arg(ch.index).arg(ch.name).arg(int(ch.type));
    QSettings().setValue(QStringLiteral("channelCache"), cached);

    showChannels(channels);
}

void MainWindow::loadCachedChannels() {
    QVector<model::Channel> channels;
    const QStringList cached = QSettings().value(QStringLiteral("channelCache")).toStringList();
    for (const QString& entry : cached) {
        const QStringList parts = entry.split(QLatin1Char('\x1f'));
        if (parts.size() < 2) continue;
        model::Channel ch;
        ch.index = parts[0].toInt();
        ch.name = parts[1];
        // Entries written before the cache carried a type are read for what the
        // name gives away, which the device's answer corrects a moment later.
        ch.type = parts.size() > 2 ? model::channelTypeFromInt(parts[2].toInt())
                                   : model::Channel::classifyByName(ch.name);
        channels.append(ch);
    }
    if (!channels.isEmpty()) showChannels(channels);
}

void MainWindow::showChannels(const QVector<model::Channel>& channels) {
    channelModel_->setChannels(channels);

    for (const model::Channel& ch : channels) {
        const QVector<model::Message>& msgs = history_.messages(ch.index);
        if (!msgs.isEmpty()) channelModel_->setLastMessage(ch.index, msgs.last());
    }

    // Prefer the channel that was open before, then the one from last session,
    // then the first slot — a reconnect should not move the user.
    int wanted = currentChannel_;
    if (wanted < 0) wanted = QSettings().value(QStringLiteral("channel"), -1).toInt();
    if (channelModel_->rowForIndex(wanted) < 0)
        wanted = channels.isEmpty() ? -1 : channels.first().index;

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
    if (!ok) {
        showNotice(QStringLiteral("Could not remove the channel: %1").arg(error), 8000, true);
        return;
    }

    // The sidebar was rebuilt before this arrived and has already moved off the
    // slot. What is left is everything else keyed by that slot number, which the
    // next channel written into it would otherwise inherit: the conversation,
    // its unread count, and the preview line in the row.
    hideNotice();
    history_.remove(channelIndex);
    channelModel_->forget(channelIndex);
    updateChannelActions();
}

void MainWindow::selectChannel(int channelIndex) {
    const int row = channelModel_->rowForIndex(channelIndex);
    if (row < 0) {
        showChannel(-1);
        return;
    }
    channelList_->setCurrentIndex(channelModel_->index(row));
}

void MainWindow::onChannelSelected(const QModelIndex& current, const QModelIndex&) {
    showChannel(current.isValid() ? channelModel_->channelIndexForRow(current.row()) : -1);
}

void MainWindow::showChannel(int channelIndex) {
    if (channelIndex == currentChannel_) return;
    currentChannel_ = channelIndex;

    chatModel_->setMessages(channelIndex >= 0 ? history_.messages(channelIndex)
                                              : QVector<model::Message>());
    channelModel_->clearUnread(channelIndex);
    updateHeader();
    updateInputState();
    // Removing acts on the selection, so the button follows it.
    updateChannelActions();

    // Lay out first, then jump to the newest message: scrolling before the
    // delegate has sized the rows lands in the wrong place.
    messageDelegate_->setViewportWidth(chatView_->viewport()->width());
    QTimer::singleShot(0, this, [this] { chatView_->scrollToBottom(); });
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

    // Only the conversation's own name: who we are and what the radio is doing
    // belong to the node pane, which says it once instead of on every channel.
    const int row = channelModel_->rowForIndex(currentChannel_);
    const QString name = channelModel_->data(channelModel_->index(row),
                                             model::ChannelModel::NameRole).toString();
    header_->setText(QStringLiteral("<b>%1</b>").arg(name.toHtmlEscaped()));
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
    history_.append(msg);
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
    history_.append(msg);
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
    pendingSends_.insert(msg.sendToken, msg);
    appendToView(msg);
    client_->sendChannelMessage(msg.channelIndex, msg.text, msg.sendToken);
}

void MainWindow::onSendResult(int token, bool ok, const QString& error) {
    const auto it = pendingSends_.constFind(token);
    if (it == pendingSends_.constEnd()) return;
    model::Message msg = *it;
    pendingSends_.erase(it);

    if (!ok) {
        // Nothing went out, so nothing stays on screen claiming it did.
        chatModel_->removePending(token);
        showNotice(QStringLiteral("Could not send: %1").arg(error), 6000, true);
        // Hand the text back rather than losing it to a failed send.
        if (input_->text().isEmpty()) input_->setText(msg.text);
        return;
    }

    // Only now is it worth writing down. History is what the app has instead of
    // the daemon's inbox, and a message that never left has no business in it.
    msg.sendState = model::Message::SendState::Sent;
    msg.sendToken = 0;
    history_.append(msg);
    channelModel_->setLastMessage(msg.channelIndex, msg);
    // The row it went up as is gone if the conversation was reloaded meanwhile.
    // Put the message back when that happened to the channel being looked at;
    // any other channel reads it from history when it is opened.
    if (!chatModel_->markSent(token) && msg.channelIndex == currentChannel_)
        appendToView(msg);
}

void MainWindow::onTextChanged(const QString& text) {
    const int left = proto::MaxMessageChars - int(text.size());
    charCount_->setText(left <= 30 ? QString::number(left) : QString());
    charCount_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg((left <= 10 ? theme::Warning : theme::TextMuted).name()));
    updateInputState();
}

void MainWindow::updateInputState() {
    const bool ready = client_->state() == proto::CompanionClient::State::Ready;
    const bool canType = ready && currentChannel_ >= 0;
    input_->setEnabled(canType);
    sendButton_->setEnabled(canType && !input_->text().trimmed().isEmpty());
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
    nodePane_->setConnection(label, color);
    updateInputState();
    updateChannelActions();
}

void MainWindow::onDeviceInfo(const proto::CompanionClient::DeviceInfo& info) {
    setWindowTitle(info.name.isEmpty() ? QStringLiteral("MeshCore")
                                       : QStringLiteral("MeshCore — %1").arg(info.name));
    nodePane_->setDevice(info);
}
