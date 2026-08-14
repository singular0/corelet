#pragma once

#include <QWidget>

#include "protocol/client.h"

class ElidedLabel;
class QEvent;
class QLabel;
class QToolButton;

// The foot of the sidebar: which node this app is talking to, and how that is
// going. It answers "am I connected, and to what" without spending a strip of
// the 480-row panel on a status bar. Its header also carries the connection
// action -- there is no menu bar to hang that action off.
class NodePane : public QWidget {
    Q_OBJECT

public:
    explicit NodePane(QWidget* parent = nullptr);

    // Where the link points, including which icon distinguishes TCP from BLE.
    void setTarget(const proto::ConnectTarget& target);
    // What the far end says it is, including the last battery reading.
    void setDevice(const proto::CompanionClient::DeviceInfo& info);
    // Already-worded link state, coloured by the caller: MainWindow owns the
    // mapping from client state to what the user should read. `active` includes
    // an in-progress connection and the retry loop, both of which can be stopped.
    // `connected` is narrower: only a ready link can expose live node details.
    void setConnection(const QString& text, const QColor& color, bool active, bool connected);

Q_SIGNALS:
    void connectRequested();
    void disconnectRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void showDeviceInfo();
    void updateBatteryDisplay();
    void updateConnectionAction(bool active);

    QToolButton* infoButton_ = nullptr;
    QToolButton* connectionButton_ = nullptr;
    QWidget* batteryRow_ = nullptr;
    QLabel* batteryIcon_ = nullptr;
    QLabel* targetIcon_ = nullptr;
    QLabel* statusIndicator_ = nullptr;
    ElidedLabel* name_ = nullptr;
    ElidedLabel* target_ = nullptr;
    ElidedLabel* battery_ = nullptr;
    ElidedLabel* status_ = nullptr;
    proto::CompanionClient::DeviceInfo device_;
    bool connectionActive_ = false;
    bool connected_ = false;
    bool showBatteryVoltage_ = false;
};
