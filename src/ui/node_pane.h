#pragma once

#include <QWidget>

#include "protocol/client.h"

class ElidedLabel;

// The foot of the sidebar: which node this app is talking to, and how that is
// going. It answers "am I connected, and to what" without spending a strip of
// the 480-row panel on a status bar, and the whole surface is the way back to
// the connect dialog -- there is no menu bar to hang that action off.
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
    // mapping from client state to what the user should read.
    void setConnection(const QString& text, const QColor& color);

Q_SIGNALS:
    void connectRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void setHovered(bool hovered);

    ElidedLabel* name_ = nullptr;
    ElidedLabel* radio_ = nullptr;
    ElidedLabel* target_ = nullptr;
    ElidedLabel* status_ = nullptr;
};
