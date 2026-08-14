#include "ui/node_pane.h"

#include <QAction>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/elided_label.h"
#include "ui/dialog_settings.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

constexpr int RowIconSize = 14;

// Everything but the node's own name is a footnote to the channel list above
// it, and sits a step below the body font — the same step the sidebar rows use.
QFont subFont(const QFont& base) {
    QFont f = base;
    f.setPointSizeF(qMax(6.5, base.pointSizeF() - 1.5));
    return f;
}

ElidedLabel* addRow(QVBoxLayout* layout, const QFont& font, const QColor& color,
                    const QString& iconName = {}, QLabel** iconOut = nullptr,
                    QWidget** rowOut = nullptr) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(5);

    // Every row owns the same icon column, including status: its coloured dot
    // changes independently of the text, but both stay aligned with the rows
    // above it.
    auto* icon = new QLabel;
    icon->setFixedSize(RowIconSize, RowIconSize);
    if (!iconName.isEmpty())
        icon->setPixmap(icons::tinted(iconName, RowIconSize, theme::TextMuted,
                                     layout->parentWidget()->devicePixelRatioF()));
    icon->setAlignment(Qt::AlignCenter);
    rowLayout->addWidget(icon);
    if (iconOut) *iconOut = icon;

    auto* label = new ElidedLabel;
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    rowLayout->addWidget(label, 1);

    if (rowOut) {
        // Let an interactive row receive clicks made over either child too.
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        *rowOut = row;
    }
    layout->addWidget(row);
    return label;
}

}  // namespace

NodePane::NodePane(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("nodePane"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Match the channel header above: the section name takes the spare width
    // and the one action that belongs to the node sits at its trailing edge.
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("sidebarHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 4, 6, 4);
    headerLayout->setSpacing(4);

    auto* title = new QLabel(QStringLiteral("NODE"));
    QFont headerFont = title->font();
    headerFont.setPointSizeF(qMax(6.5, headerFont.pointSizeF() - 1.5));
    headerFont.setBold(true);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    title->setFont(headerFont);
    title->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    infoButton_ = new QToolButton;
    infoButton_->setObjectName(QStringLiteral("iconButton"));
    infoButton_->setIconSize(QSize(14, 14));
    infoButton_->setAutoRaise(true);
    infoButton_->setCursor(Qt::PointingHandCursor);
    infoButton_->setFocusPolicy(Qt::NoFocus);
    infoButton_->setText(QStringLiteral("Node information"));
    infoButton_->setToolTip(QStringLiteral("Node information"));
    QIcon infoIcon;
    infoIcon.addPixmap(icons::tinted(QStringLiteral("info"), 14, theme::TextMuted,
                                    devicePixelRatioF()),
                       QIcon::Normal);
    infoIcon.addPixmap(icons::tinted(QStringLiteral("info"), 14, theme::Accent,
                                    devicePixelRatioF()),
                       QIcon::Active);
    infoIcon.addPixmap(icons::tinted(QStringLiteral("info"), 14, theme::Border,
                                    devicePixelRatioF()),
                       QIcon::Disabled);
    infoButton_->setIcon(infoIcon);
    infoButton_->setEnabled(false);

    connectionButton_ = new QToolButton;
    connectionButton_->setObjectName(QStringLiteral("iconButton"));
    connectionButton_->setIconSize(QSize(14, 14));
    connectionButton_->setAutoRaise(true);
    connectionButton_->setCursor(Qt::PointingHandCursor);
    connectionButton_->setFocusPolicy(Qt::NoFocus);
    updateConnectionAction(false);

    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(infoButton_);
    headerLayout->addWidget(connectionButton_);

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(10, 5, 8, 6);
    detailsLayout->setSpacing(3);

    QFont nameFont = font();
    nameFont.setBold(true);

    name_ = addRow(detailsLayout, nameFont, theme::Text, QStringLiteral("radio-tower"));
    target_ = addRow(detailsLayout, subFont(font()), theme::TextMuted,
                     QStringLiteral("server"), &targetIcon_);
    battery_ = addRow(detailsLayout, subFont(font()), theme::TextMuted,
                      QStringLiteral("battery-medium"), &batteryIcon_, &batteryRow_);
    status_ = addRow(detailsLayout, subFont(font()), theme::TextMuted, {}, &statusIndicator_);

    name_->setFullText(QStringLiteral("unavailable"));
    batteryRow_->installEventFilter(this);
    updateBatteryDisplay();

    layout->addWidget(header);
    layout->addWidget(details);

    connect(connectionButton_, &QToolButton::clicked, this, [this] {
        if (connectionActive_)
            Q_EMIT disconnectRequested();
        else
            Q_EMIT connectRequested();
    });
    connect(infoButton_, &QToolButton::clicked, this, &NodePane::showDeviceInfo);
}

void NodePane::setTarget(const proto::ConnectTarget& target) {
    constexpr int IconSize = 14;
    target_->setFullText(target.label());
    const QString icon = target.kind == proto::ConnectTarget::Kind::Ble
                             ? QStringLiteral("bluetooth")
                             : QStringLiteral("server");
    targetIcon_->setPixmap(
        icons::tinted(icon, IconSize, theme::TextMuted, devicePixelRatioF()));
}

void NodePane::setDevice(const proto::CompanionClient::DeviceInfo& info) {
    device_ = info;
    name_->setFullText(info.name.isEmpty() ? QStringLiteral("unavailable") : info.name);
    updateBatteryDisplay();
    infoButton_->setEnabled(connected_ && info.pubkey.size() == 32);
}

bool NodePane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == batteryRow_ && event->type() == QEvent::MouseButtonRelease) {
        const auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton && connected_ &&
            device_.batteryPercent >= 0 && device_.batteryMillivolts >= 0) {
            showBatteryVoltage_ = !showBatteryVoltage_;
            updateBatteryDisplay();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void NodePane::updateBatteryDisplay() {
    const bool available =
        connected_ && device_.batteryPercent >= 0 && device_.batteryMillivolts >= 0;

    QColor tint = theme::TextMuted;
    if (available && device_.batteryPercent <= 10)
        tint = theme::Error;
    else if (available && device_.batteryPercent <= 25)
        tint = theme::Warning;
    battery_->setStyleSheet(QStringLiteral("color: %1;").arg(tint.name()));
    batteryIcon_->setPixmap(
        icons::tinted(QStringLiteral("battery-medium"), RowIconSize, tint, devicePixelRatioF()));

    if (!available) {
        battery_->setFullText(QStringLiteral("unavailable"));
        batteryRow_->unsetCursor();
        batteryRow_->setToolTip({});
        return;
    }

    if (showBatteryVoltage_) {
        battery_->setFullText(
            QStringLiteral("%1 V").arg(device_.batteryMillivolts / 1000.0, 0, 'f', 3));
        batteryRow_->setToolTip(QStringLiteral("Click to show battery percentage"));
    } else {
        battery_->setFullText(QStringLiteral("%1%").arg(device_.batteryPercent));
        batteryRow_->setToolTip(QStringLiteral("Click to show battery voltage"));
    }
    batteryRow_->setCursor(Qt::PointingHandCursor);
}

void NodePane::showDeviceInfo() {
    if (device_.pubkey.size() != 32) return;

    QDialog dialog(this);
    ui::configureDialogWindow(dialog);
    dialog.setWindowTitle(QStringLiteral("Node information"));

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const auto addValue = [&form](const QString& name, const QString& value) {
        auto* label = new QLabel(value);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(name, label);
    };

    addValue(QStringLiteral("Name"),
             device_.name.isEmpty() ? QStringLiteral("Unavailable") : device_.name);

    auto* publicKey = new QLineEdit(QString::fromLatin1(device_.pubkey.toHex()));
    publicKey->setReadOnly(true);
    publicKey->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    publicKey->setCursorPosition(0);
    constexpr int CopyIconSize = 16;
    QIcon copyIcon;
    copyIcon.addPixmap(icons::tinted(QStringLiteral("copy"), CopyIconSize, theme::TextMuted,
                                    devicePixelRatioF()),
                       QIcon::Normal);
    copyIcon.addPixmap(icons::tinted(QStringLiteral("copy"), CopyIconSize, theme::Text,
                                    devicePixelRatioF()),
                       QIcon::Active);
    QAction* copy = publicKey->addAction(copyIcon, QLineEdit::TrailingPosition);
    copy->setText(QStringLiteral("Copy public key"));
    copy->setToolTip(QStringLiteral("Copy public key"));
    connect(copy, &QAction::triggered, publicKey,
            [publicKey] { QGuiApplication::clipboard()->setText(publicKey->text()); });
    form->addRow(QStringLiteral("Public key"), publicKey);

    const QString unavailable = QStringLiteral("Unavailable");
    addValue(QStringLiteral("Battery voltage"),
             device_.batteryMillivolts < 0
                 ? unavailable
                 : QStringLiteral("%1 V (%2%)")
                       .arg(device_.batteryMillivolts / 1000.0, 0, 'f', 3)
                       .arg(device_.batteryPercent));
    addValue(QStringLiteral("Latitude"),
             device_.hasLocation
                 ? QStringLiteral("%1 degrees").arg(device_.latitude, 0, 'f', 6)
                 : unavailable);
    addValue(QStringLiteral("Longitude"),
             device_.hasLocation
                 ? QStringLiteral("%1 degrees").arg(device_.longitude, 0, 'f', 6)
                 : unavailable);
    addValue(QStringLiteral("Radio frequency"),
             QStringLiteral("%1 MHz").arg(device_.freqMhz, 0, 'f', 3));
    addValue(QStringLiteral("Bandwidth"),
             QStringLiteral("%1 kHz").arg(device_.bwKhz, 0, 'f', 3));
    addValue(QStringLiteral("Spreading factor"), QString::number(device_.sf));
    addValue(QStringLiteral("Coding rate"), QStringLiteral("4/%1").arg(device_.cr));
    addValue(QStringLiteral("Transmit power"),
             QStringLiteral("%1 dBm").arg(device_.txPowerDbm));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addLayout(form);
    layout->addWidget(buttons);

    // Wide enough to keep the complete 32-byte key visible on one line, while
    // remaining comfortably inside the uConsole's 1280-pixel display.
    ui::lockDialogSize(dialog, *layout, 640);
    dialog.exec();
}

void NodePane::setConnection(const QString& text, const QColor& color, bool active,
                             bool connected) {
    QPixmap indicator(QSize(RowIconSize, RowIconSize) * devicePixelRatioF());
    indicator.setDevicePixelRatio(devicePixelRatioF());
    indicator.fill(Qt::transparent);
    QPainter painter(&indicator);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(3, 3, 8, 8));
    painter.end();

    statusIndicator_->setPixmap(indicator);
    status_->setFullText(text);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    connected_ = connected;
    updateBatteryDisplay();
    infoButton_->setEnabled(connected_ && device_.pubkey.size() == 32);
    if (active == connectionActive_) return;

    updateConnectionAction(active);
}

void NodePane::updateConnectionAction(bool active) {
    constexpr int IconSize = 14;
    const QString action = active ? QStringLiteral("Disconnect") : QStringLiteral("Connect");
    const QString iconName = active ? QStringLiteral("unplug") : QStringLiteral("plug");
    const QColor hover = active ? theme::Error : theme::Accent;

    QIcon icon;
    icon.addPixmap(icons::tinted(iconName, IconSize, theme::TextMuted, devicePixelRatioF()),
                   QIcon::Normal);
    icon.addPixmap(icons::tinted(iconName, IconSize, hover, devicePixelRatioF()), QIcon::Active);
    connectionButton_->setIcon(icon);
    connectionButton_->setText(action);
    connectionButton_->setToolTip(active ? QStringLiteral("Disconnect from this node")
                                         : QStringLiteral("Connect to a daemon or device"));
    connectionActive_ = active;
}

// A QWidget subclass draws no stylesheet background or border of its own, and
// the border is the only thing separating this pane from the last channel row.
void NodePane::paintEvent(QPaintEvent*) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);
}
