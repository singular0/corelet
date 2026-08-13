#pragma once

#include <QMainWindow>

#include "model/history.h"
#include "protocol/client.h"

class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QSplitter;
class MessageDelegate;

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

private:
    void buildUi();
    // Points the client at a target, replacing whatever it was talking to.
    void connectTo(const proto::ConnectTarget& target);
    void openConnectDialog();
    // Shows a channel list, from the device or from the offline cache.
    void showChannels(const QVector<model::Channel>& channels);
    void loadCachedChannels();
    void selectChannel(int channelIndex);
    void showChannel(int channelIndex);
    void appendToView(const model::Message& msg);
    void updateInputState();
    void updateHeader();

    proto::CompanionClient* client_ = nullptr;
    proto::ConnectTarget target_;
    model::History history_;

    model::ChannelModel* channelModel_ = nullptr;
    model::ChatModel* chatModel_ = nullptr;
    MessageDelegate* messageDelegate_ = nullptr;

    QSplitter* splitter_ = nullptr;
    QListView* channelList_ = nullptr;
    QListView* chatView_ = nullptr;
    QLabel* header_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QLabel* charCount_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    // Names the current target and reopens the connect dialog. There is no menu
    // bar to hang a Connect action off, and 480 rows is no place to grow one.
    QPushButton* targetButton_ = nullptr;

    // Daemon slot number of the open conversation, or -1 for none. Not a row:
    // rows shift when channels are re-enumerated after a reconnect.
    int currentChannel_ = -1;
    int directMessageCount_ = 0;
};
