#pragma once

#include <QMainWindow>

#include "model/history.h"
#include "protocol/client.h"

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
    void onSendResult(int channelIndex, const QString& text, bool ok, const QString& error);
    void onChannelSaveResult(int channelIndex, bool ok, const QString& error);

private:
    void buildUi();
    // Points the client at a target, replacing whatever it was talking to.
    void connectTo(const proto::ConnectTarget& target);
    void openConnectDialog();
    void openAddChannelDialog();
    // Shows a channel list, from the device or from the offline cache.
    void showChannels(const QVector<model::Channel>& channels);
    void loadCachedChannels();
    void selectChannel(int channelIndex);
    void showChannel(int channelIndex);
    void appendToView(const model::Message& msg);
    void updateInputState();
    // Adding a channel is a write to the device, so it needs a live link and a
    // free slot to write into.
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

    model::ChannelModel* channelModel_ = nullptr;
    model::ChatModel* chatModel_ = nullptr;
    MessageDelegate* messageDelegate_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QListView* channelList_ = nullptr;
    QToolButton* addChannelButton_ = nullptr;
    // Foot of the sidebar: the node, the link, and the way to the connect
    // dialog. There is no status bar and no menu bar to put any of that in.
    NodePane* nodePane_ = nullptr;
    QListView* chatView_ = nullptr;
    QLabel* header_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* charCount_ = nullptr;
    ElidedLabel* notice_ = nullptr;
    QTimer* noticeTimer_ = nullptr;

    // Daemon slot number of the open conversation, or -1 for none. Not a row:
    // rows shift when channels are re-enumerated after a reconnect.
    int currentChannel_ = -1;
    int directMessageCount_ = 0;
};
