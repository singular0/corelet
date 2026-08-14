#include "ui/connect_dialog.h"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

#include "protocol/ble_transport.h"
#include "ui/dialog_settings.h"
#include "ui/theme.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#include <QCoreApplication>
#include <QPermissions>
#define CORELET_HAVE_PERMISSIONS 1
#endif

namespace {

// Tab order is stored as the remembered transport, so these are not just
// positions.
constexpr int TcpTab = 0;
constexpr int BleTab = 1;

constexpr int ScanTimeoutMs = 12000;

constexpr int IdRole = Qt::UserRole;
constexpr int NameRole = Qt::UserRole + 1;

}  // namespace

ConnectDialog::ConnectDialog(QWidget* parent) : QDialog(parent) {
    ui::configureDialogWindow(*this);
    setWindowTitle(QStringLiteral("Connect"));
    saved_ = lastTarget();
    buildUi();

    host_->setText(saved_.host);
    port_->setText(QString::number(saved_.port));
    tabs_->setCurrentIndex(saved_.kind == proto::ConnectTarget::Kind::Ble ? BleTab : TcpTab);
    rebuildDeviceList();
    updateConnectButton();

    if (tabs_->currentIndex() == BleTab) scanWhenPermitted();
}

ConnectDialog::~ConnectDialog() { stopScan(); }

void ConnectDialog::buildUi() {
    tabs_ = new QTabWidget;

    // --- network ------------------------------------------------------------
    auto* tcpTab = new QWidget;
    auto* tcpLayout = new QFormLayout(tcpTab);
    tcpLayout->setContentsMargins(12, 10, 12, 10);
    tcpLayout->setSpacing(6);

    host_ = new QLineEdit;
    host_->setPlaceholderText(QStringLiteral("127.0.0.1"));
    port_ = new QLineEdit;
    port_->setValidator(new QIntValidator(1, 65535, port_));
    port_->setMaximumWidth(90);

    tcpLayout->addRow(QStringLiteral("Host"), host_);
    tcpLayout->addRow(QStringLiteral("Port"), port_);

    // --- bluetooth ----------------------------------------------------------
    auto* bleTab = new QWidget;
    auto* bleLayout = new QVBoxLayout(bleTab);
    bleLayout->setContentsMargins(12, 10, 12, 10);
    bleLayout->setSpacing(6);

    devices_ = new QListWidget;
    devices_->setMinimumHeight(104);
    devices_->setAlternatingRowColors(false);

    bleStatus_ = new QLabel;
    bleStatus_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    scanButton_ = new QPushButton(QStringLiteral("Scan"));

    auto* bleTop = new QHBoxLayout;
    bleTop->addWidget(bleStatus_, 1);
    bleTop->addWidget(scanButton_);

    bleLayout->addLayout(bleTop);
    bleLayout->addWidget(devices_, 1);

    tabs_->addTab(tcpTab, QStringLiteral("Network"));
    tabs_->addTab(bleTab, QStringLiteral("Bluetooth"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    connectButton_ = buttons->addButton(QStringLiteral("Connect"), QDialogButtonBox::AcceptRole);
    connectButton_->setDefault(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    layout->addWidget(tabs_, 1);
    layout->addWidget(buttons);

    connect(tabs_, &QTabWidget::currentChanged, this, &ConnectDialog::onTabChanged);
    connect(scanButton_, &QPushButton::clicked, this, &ConnectDialog::scanWhenPermitted);
    connect(devices_, &QListWidget::itemSelectionChanged, this,
            &ConnectDialog::updateConnectButton);
    connect(devices_, &QListWidget::itemDoubleClicked, this, &ConnectDialog::onAccepted);
    connect(host_, &QLineEdit::textChanged, this, &ConnectDialog::updateConnectButton);
    connect(port_, &QLineEdit::textChanged, this, &ConnectDialog::updateConnectButton);
    connect(buttons, &QDialogButtonBox::accepted, this, &ConnectDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Wide enough for a BLE name and short enough for the uConsole's 480 rows.
    ui::lockDialogSize(*this, *layout, 420);
}

// ---------------------------------------------------------------------------
// Bluetooth discovery
// ---------------------------------------------------------------------------

void ConnectDialog::scanWhenPermitted() {
#ifdef CORELET_HAVE_PERMISSIONS
    // macOS and Android gate Bluetooth behind a user grant, and a scan started
    // without one finds nothing at all rather than failing.
    QBluetoothPermission permission;
    permission.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(permission)) {
        case Qt::PermissionStatus::Undetermined:
            setBleStatus(QStringLiteral("Waiting for Bluetooth permission..."));
            qApp->requestPermission(permission, this, [this](const QPermission& result) {
                if (result.status() == Qt::PermissionStatus::Granted)
                    startScan();
                else
                    setBleStatus(QStringLiteral("Bluetooth permission denied"), true);
            });
            return;
        case Qt::PermissionStatus::Denied:
            setBleStatus(QStringLiteral("Bluetooth permission denied — grant it in "
                                        "system settings"),
                         true);
            return;
        case Qt::PermissionStatus::Granted:
            break;
    }
#endif
    startScan();
}

void ConnectDialog::startScan() {
    stopScan();
    found_.clear();
    scanned_ = true;
    rebuildDeviceList();

    agent_ = new QBluetoothDeviceDiscoveryAgent(this);
    agent_->setLowEnergyDiscoveryTimeout(ScanTimeoutMs);

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
            [this](const QBluetoothDeviceInfo& info) {
                if (!(info.coreConfigurations() &
                      QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
                    return;
                for (const QBluetoothDeviceInfo& known : found_)
                    if (proto::bleDeviceId(known) == proto::bleDeviceId(info)) return;
                found_.append(info);
                rebuildDeviceList();
            });

    // A device's name usually arrives in the scan response, after the
    // advertisement that first announced it, so rows fill in as they resolve.
    connect(agent_, &QBluetoothDeviceDiscoveryAgent::deviceUpdated, this,
            [this](const QBluetoothDeviceInfo& info, QBluetoothDeviceInfo::Fields) {
                for (QBluetoothDeviceInfo& known : found_) {
                    if (proto::bleDeviceId(known) != proto::bleDeviceId(info)) continue;
                    known = info;
                    rebuildDeviceList();
                    return;
                }
            });

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::finished, this, [this] {
        scanButton_->setEnabled(true);
        // Counted from the scan results, not from the list: the remembered
        // device is listed without having been seen and must not inflate this.
        int found = 0;
        for (const QBluetoothDeviceInfo& info : found_)
            if (proto::looksLikeMeshCore(info)) found++;
        setBleStatus(found == 0 ? QStringLiteral("No devices found")
                                : QStringLiteral("%1 device%2 found")
                                      .arg(found)
                                      .arg(found == 1 ? QString() : QStringLiteral("s")));
    });

    connect(agent_, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
            [this](QBluetoothDeviceDiscoveryAgent::Error error) {
                scanButton_->setEnabled(true);
                setBleStatus(error == QBluetoothDeviceDiscoveryAgent::PoweredOffError
                                 ? QStringLiteral("Bluetooth is turned off")
                                 : agent_->errorString(),
                             true);
            });

    scanButton_->setEnabled(false);
    setBleStatus(QStringLiteral("Scanning..."));
    agent_->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void ConnectDialog::stopScan() {
    if (!agent_) return;
    agent_->disconnect(this);
    agent_->stop();
    agent_->deleteLater();
    agent_ = nullptr;
}

void ConnectDialog::rebuildDeviceList() {
    const QListWidgetItem* current = devices_->currentItem();
    const QString keep = current ? current->data(IdRole).toString() : QString();

    devices_->clear();

    auto addDevice = [this](const QString& id, const QString& name, const QString& note) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1%2").arg(name.isEmpty() ? id : name, note), devices_);
        item->setData(IdRole, id);
        item->setData(NameRole, name);
        item->setToolTip(id);
    };

    bool savedIsListed = false;
    for (const QBluetoothDeviceInfo& info : found_)
        if (proto::bleDeviceId(info) == saved_.bleId) savedIsListed = true;

    if (saved_.kind == proto::ConnectTarget::Kind::Ble && !saved_.bleId.isEmpty() &&
        !savedIsListed)
        addDevice(saved_.bleId, saved_.bleName, QStringLiteral("   (last used)"));

    for (const QBluetoothDeviceInfo& info : found_) {
        if (!proto::looksLikeMeshCore(info)) continue;
        addDevice(proto::bleDeviceId(info), info.name(), {});
    }

    for (int row = 0; row < devices_->count(); row++) {
        if (devices_->item(row)->data(IdRole).toString() != keep) continue;
        devices_->setCurrentRow(row);
        return;
    }
    // Nothing to restore: the top row is the best guess, and it saves a click.
    if (devices_->count() > 0) devices_->setCurrentRow(0);
    updateConnectButton();
}

void ConnectDialog::setBleStatus(const QString& text, bool error) {
    bleStatus_->setText(text);
    bleStatus_->setStyleSheet(
        QStringLiteral("color: %1;").arg((error ? theme::Error : theme::TextMuted).name()));
}

// ---------------------------------------------------------------------------
// Result
// ---------------------------------------------------------------------------

void ConnectDialog::onTabChanged(int index) {
    // Only the first visit scans: coming back to the tab should not throw away
    // a list the user is looking at.
    if (index == BleTab && !scanned_) scanWhenPermitted();
    updateConnectButton();
}

void ConnectDialog::updateConnectButton() {
    if (tabs_->currentIndex() == BleTab) {
        connectButton_->setEnabled(devices_->currentItem() != nullptr);
        return;
    }
    connectButton_->setEnabled(!host_->text().trimmed().isEmpty() && port_->hasAcceptableInput());
}

void ConnectDialog::onAccepted() {
    proto::ConnectTarget target;

    if (tabs_->currentIndex() == BleTab) {
        const QListWidgetItem* item = devices_->currentItem();
        if (!item) return;
        target.kind = proto::ConnectTarget::Kind::Ble;
        target.bleId = item->data(IdRole).toString();
        target.bleName = item->data(NameRole).toString();
    } else {
        if (!port_->hasAcceptableInput()) return;
        target.kind = proto::ConnectTarget::Kind::Tcp;
        target.host = host_->text().trimmed();
        target.port = quint16(port_->text().toUInt());
        if (target.host.isEmpty()) return;
    }

    stopScan();
    target_ = target;
    rememberTarget(target_);
    accept();
}

// ---------------------------------------------------------------------------
// Remembering
// ---------------------------------------------------------------------------

proto::ConnectTarget ConnectDialog::lastTarget() {
    QSettings settings;
    proto::ConnectTarget target;
    target.kind = settings.value(QStringLiteral("connection/kind")).toString() ==
                          QStringLiteral("ble")
                      ? proto::ConnectTarget::Kind::Ble
                      : proto::ConnectTarget::Kind::Tcp;
    target.host = settings.value(QStringLiteral("connection/host"), target.host).toString();
    target.port = quint16(settings.value(QStringLiteral("connection/port"), target.port).toUInt());
    target.bleId = settings.value(QStringLiteral("connection/bleId")).toString();
    target.bleName = settings.value(QStringLiteral("connection/bleName")).toString();

    // A remembered BLE target with nothing to identify it is worse than no
    // memory at all: it would open on a device that can never be found.
    if (target.kind == proto::ConnectTarget::Kind::Ble && !target.isValid())
        target.kind = proto::ConnectTarget::Kind::Tcp;
    return target;
}

void ConnectDialog::rememberTarget(const proto::ConnectTarget& target) {
    QSettings settings;
    const bool ble = target.kind == proto::ConnectTarget::Kind::Ble;
    settings.setValue(QStringLiteral("connection/kind"),
                      ble ? QStringLiteral("ble") : QStringLiteral("tcp"));
    if (ble) {
        settings.setValue(QStringLiteral("connection/bleId"), target.bleId);
        settings.setValue(QStringLiteral("connection/bleName"), target.bleName);
    } else {
        settings.setValue(QStringLiteral("connection/host"), target.host);
        settings.setValue(QStringLiteral("connection/port"), target.port);
    }
}
