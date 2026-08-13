#pragma once

#include <QBluetoothDeviceInfo>
#include <QDialog>
#include <QVector>

#include "protocol/transport.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTabWidget;
class QBluetoothDeviceDiscoveryAgent;

// Asks what to connect to: a umeshcored over TCP, or a device over BLE.
//
// Shown at startup and again whenever the user wants to point the app somewhere
// else, so it has to work both from cold — nothing remembered, no scan done —
// and while a session is already up.
class ConnectDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectDialog(QWidget* parent = nullptr);
    ~ConnectDialog() override;

    // Valid once the dialog has been accepted.
    proto::ConnectTarget target() const { return target_; }

    // What was connected to last, for launching straight into it. Kind::Tcp on
    // its loopback default when nothing has been saved yet.
    static proto::ConnectTarget lastTarget();
    static void rememberTarget(const proto::ConnectTarget& target);

private Q_SLOTS:
    void startScan();
    void onTabChanged(int index);
    void onAccepted();

private:
    void buildUi();
    void stopScan();
    void rebuildDeviceList();
    void updateConnectButton();
    void setBleStatus(const QString& text, bool error = false);
    // Bluetooth needs an explicit grant on some platforms, and a scan started
    // without it fails silently. Calls startScan() once it has one.
    void scanWhenPermitted();

    proto::ConnectTarget target_;

    QTabWidget* tabs_ = nullptr;
    QLineEdit* host_ = nullptr;
    QLineEdit* port_ = nullptr;
    QListWidget* devices_ = nullptr;
    QCheckBox* showAll_ = nullptr;
    QPushButton* scanButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QLabel* bleStatus_ = nullptr;

    QBluetoothDeviceDiscoveryAgent* agent_ = nullptr;
    QVector<QBluetoothDeviceInfo> found_;
    // The remembered device is offered before any scan has seen it: the
    // transport rescans on connect anyway, so waiting out a discovery just to
    // click the same node again would be a wasted ten seconds.
    proto::ConnectTarget saved_;
    bool scanned_ = false;
};
