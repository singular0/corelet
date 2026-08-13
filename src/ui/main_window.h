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
class ElidedLabel;
class MessageDelegate;
class NodePane;

namespace model {
class ChannelModel;
class ChatModel;
}  // namespace model

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const proto::ConnectTarget& target, QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private Q_SLOTS:
    void onChannelSelected(const QModelIndex& current, const QModelIndex& previous);
    void onSendClicked();
    void onTextChanged(const QString& text);

    void onStateChanged(proto::CompanionClient::State state, const QString& detail);
    void onDeviceInfo(const proto::CompanionClient::DeviceInfo& info);
    void onChannelsChanged(const QVector<model::Channel>& channels);
    void onMessageReceived(const model::Message& msg);
    void onDirectMessageReceived(const model::Message& msg);
    void onSendResult(int token, bool ok, const QString& error);
    void onChannelSaveResult(int channelIndex, bool ok, const QString& error);
    void onChannelRemoveResult(int channelIndex, bool ok, const QString& error);

private:
    void buildUi();
    // Points the client at a target, replacing whatever it was talking to.
    void connectTo(const proto::ConnectTarget& target);
    void openConnectDialog();
    void openAddChannelDialog();
    // Shows the selected channel's key, to pass on to whoever is being invited.
    void shareCurrentChannel();
    // Asks first: this deletes the key from the device, and a private channel
    // cannot be got back without a copy of it.
    void removeCurrentChannel();
    // Shows a channel list, from the active device or its identity-scoped cache.
    void showChannels(const QVector<model::Channel>& channels);
    void loadCachedChannels();
    void saveCachedChannels(const QVector<model::Channel>& channels);
    void selectChannel(int channelIndex);
    void showChannel(int channelIndex);
    QByteArray currentChannelKey() const;
    // The selected channel as the device holds it -- with its key, and so with
    // its real type, which the offline cache cannot give. Empty when nothing is
    // selected or the link is down, which is also when nothing can be done to it.
    std::optional<model::Channel> currentChannelOnDevice() const;
    void appendToView(const model::Message& msg);
    void updateInputState();
    // Adding and removing a channel are both writes to the device, so they need
    // a live link -- and a free slot to write into, or a removable channel
    // selected.
    void updateChannelActions();
    void updateHeader();
    // A passing word above the message box -- a failed send, a channel being
    // added. It costs no vertical space while there is nothing to say, which is
    // why this is not a permanent strip.
    void showNotice(const QString& text, int ms, bool error = false);
    void hideNotice();

    proto::CompanionClient* client_ = nullptr;
    proto::ConnectTarget target_;
    model::History history_;
    // The MeshCore public key learned from SELF_INFO. No channel or message is
    // read or written before this is known.
    QByteArray activeDeviceId_;

    model::ChannelModel* channelModel_ = nullptr;
    model::ChatModel* chatModel_ = nullptr;
    MessageDelegate* messageDelegate_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QListView* channelList_ = nullptr;
    QToolButton* addChannelButton_ = nullptr;
    QToolButton* shareChannelButton_ = nullptr;
    QToolButton* removeChannelButton_ = nullptr;
    // Foot of the sidebar: the node, the link, and the way to the connect
    // dialog. There is no status bar and no menu bar to put any of that in.
    NodePane* nodePane_ = nullptr;
    QListView* chatView_ = nullptr;
    QLabel* header_ = nullptr;
    QLineEdit* input_ = nullptr;
    QAction* sendAction_ = nullptr;
    QLabel* charCount_ = nullptr;
    ElidedLabel* notice_ = nullptr;
    QTimer* noticeTimer_ = nullptr;

    // Current daemon slot number of the open conversation, or -1 for none. It
    // is only a wire address; persistent selection follows the channel key.
    int currentChannel_ = -1;
    int directMessageCount_ = 0;

    // Sends the daemon has not answered yet, by the tag the app gave them. They
    // are on screen but not in history: a message is written down only once it
    // is known to have got somewhere.
    struct PendingSend {
        model::Message message;
        QByteArray deviceId;
        QByteArray channelKey;
    };
    QHash<int, PendingSend> pendingSends_;
    // clearChannel() removes the channel from the client's list before its
    // result reaches the window, so retain the key-derived identity here.
    QHash<int, QByteArray> pendingChannelRemovals_;
    int lastSendToken_ = 0;
};
