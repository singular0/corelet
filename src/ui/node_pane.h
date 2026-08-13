#pragma once

#include <QWidget>

#include "protocol/client.h"

class ElidedLabel;
class QToolButton;

// The foot of the sidebar: which node this app is talking to, and how that is
// going. It answers "am I connected, and to what" without spending a strip of
// the 480-row panel on a status bar. Its header also carries the connection
// action -- there is no menu bar to hang that action off.
class NodePane : public QWidget {
    Q_OBJECT

public:
    explicit NodePane(QWidget* parent = nullptr);

    // Where the link points: a host:port, or a BLE device name.
    void setTarget(const QString& label);
    // What the far end says it is. Empty fields stay hidden rather than showing
    // a placeholder, so the pane is only as tall as it has something to say.
    void setDevice(const proto::CompanionClient::DeviceInfo& info);
    // Already-worded link state, coloured by the caller: MainWindow owns the
    // mapping from client state to what the user should read. `active` includes
    // an in-progress connection and the retry loop, both of which can be stopped.
    void setConnection(const QString& text, const QColor& color, bool active);

Q_SIGNALS:
    void connectRequested();
    void disconnectRequested();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateConnectionAction(bool active);

    QToolButton* connectionButton_ = nullptr;
    ElidedLabel* name_ = nullptr;
    ElidedLabel* radio_ = nullptr;
    ElidedLabel* target_ = nullptr;
    ElidedLabel* status_ = nullptr;
    bool connectionActive_ = false;
};
