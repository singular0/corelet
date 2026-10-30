#include "ui/node_pane.h"

#include <QAction>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/elided_label.h"
#include "ui/icons.h"
#include "ui/menu_button.h"
#include "ui/theme.h"

namespace {

constexpr int RowIconSize = 14;

ElidedLabel* addRow(QVBoxLayout* layout, const QFont& font, const QColor& color,
                    int iconSize, const QString& iconName = {}, QLabel** iconOut = nullptr,
                    QWidget** rowOut = nullptr) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(5);

    // Every row owns the same icon column, including status: its coloured dot
    // changes independently of the text, but both stay aligned with the rows
    // above it.
    auto* icon = new QLabel;
    icon->setFixedSize(iconSize, iconSize);
    if (!iconName.isEmpty())
        icon->setPixmap(icons::tinted(iconName, iconSize, theme::TextMuted,
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

// One of the node header's icon-only actions. The same SVG in three colours;
// QIcon picks the mode itself, which is cheaper than restyling the button on
// every hover and enable change.
QToolButton* headerButton(const QString& icon, const QString& action, int iconSize, qreal dpr) {
    auto* button = new QToolButton;
    button->setObjectName(QStringLiteral("iconButton"));
    button->setIconSize(QSize(iconSize, iconSize));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setText(action);
    button->setToolTip(action);

    QIcon set;
    set.addPixmap(icons::tinted(icon, iconSize, theme::TextMuted, dpr), QIcon::Normal);
    set.addPixmap(icons::tinted(icon, iconSize, theme::Accent, dpr), QIcon::Active);
    set.addPixmap(icons::tinted(icon, iconSize, theme::Border, dpr), QIcon::Disabled);
    button->setIcon(set);
    button->setEnabled(false);
    return button;
}

}  // namespace

NodePane::NodePane(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("nodePane"));
    iconSize_ = theme::scaled(font(), RowIconSize);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Match the channel header above: the section name takes the spare width
    // and the actions that belong to the node sit at its trailing edge.
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("sidebarHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 4, 6, 4);
    headerLayout->setSpacing(4);

    auto* title = new QLabel(QStringLiteral("NODE"));
    QFont headerFont = theme::secondaryFont(title->font());
    headerFont.setBold(true);
    headerFont.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    title->setFont(headerFont);
    title->setStyleSheet(QStringLiteral("color: %1;").arg(theme::TextMuted.name()));

    // All three need a live link before they have anything to do, so all three
    // start disabled and follow the connection. The advert leads, furthest from
    // the connection action: it is the one button here that puts something on
    // the air, and a trackball slip onto Disconnect instead is a worse mistake
    // than the other way round.
    advertButton_ = headerButton(QStringLiteral("megaphone"), QStringLiteral("Advertise"),
                                 iconSize_, devicePixelRatioF());
    // "Advertise" alone does not say what is being advertised, which is the
    // node itself rather than anything in the window.
    advertButton_->setToolTip(QStringLiteral("Announce this node to the mesh"));
    contactsButton_ = headerButton(QStringLiteral("users"), QStringLiteral("Contacts"),
                                   iconSize_, devicePixelRatioF());
    configButton_ = headerButton(QStringLiteral("settings"), QStringLiteral("Node configuration"),
                                 iconSize_, devicePixelRatioF());

    connectionButton_ = new QToolButton;
    connectionButton_->setObjectName(QStringLiteral("iconButton"));
    connectionButton_->setIconSize(QSize(iconSize_, iconSize_));
    connectionButton_->setAutoRaise(true);
    connectionButton_->setCursor(Qt::PointingHandCursor);
    connectionButton_->setFocusPolicy(Qt::NoFocus);
    updateConnectionAction(false);

    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(advertButton_);
    headerLayout->addWidget(contactsButton_);
    headerLayout->addWidget(configButton_);
    headerLayout->addWidget(connectionButton_);

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(10, 5, 8, 6);
    detailsLayout->setSpacing(3);

    QFont nameFont = font();
    nameFont.setBold(true);

    name_ = addRow(detailsLayout, nameFont, theme::Text, iconSize_,
                   QStringLiteral("radio-tower"));
    const QFont detailsFont = theme::secondaryFont(font());
    target_ = addRow(detailsLayout, detailsFont, theme::TextMuted, iconSize_,
                     QStringLiteral("server"), &targetIcon_);
    battery_ = addRow(detailsLayout, detailsFont, theme::TextMuted, iconSize_,
                      QStringLiteral("battery-medium"), &batteryIcon_, &batteryRow_);
    status_ = addRow(detailsLayout, detailsFont, theme::TextMuted, iconSize_, {},
                     &statusIndicator_, &statusRow_);

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
    connect(advertButton_, &QToolButton::clicked, this, &NodePane::showAdvertMenu);
    connect(configButton_, &QToolButton::clicked, this, &NodePane::configRequested);
    connect(contactsButton_, &QToolButton::clicked, this, &NodePane::contactsRequested);
}

void NodePane::setTarget(const proto::ConnectTarget& target) {
    target_->setFullText(target.label());
    const QString icon = target.kind == proto::ConnectTarget::Kind::Ble
                             ? QStringLiteral("bluetooth")
                             : QStringLiteral("server");
    targetIcon_->setPixmap(
        icons::tinted(icon, iconSize_, theme::TextMuted, devicePixelRatioF()));
}

void NodePane::setDevice(const proto::CompanionClient::DeviceInfo& info) {
    device_ = info;
    name_->setFullText(info.name.isEmpty() ? QStringLiteral("unavailable") : info.name);
    updateBatteryDisplay();
    configButton_->setEnabled(connected_ && info.pubkey.size() == 32);
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
        icons::tinted(QStringLiteral("battery-medium"), iconSize_, tint, devicePixelRatioF()));

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

// How far an advert travels is chosen before it is sent and there is nothing
// else to fill in, so it is a menu on the button rather than a dialog behind it
// -- the same shape the sidebar's add button takes for the same reason.
void NodePane::showAdvertMenu() {
    QMenu menu(this);
    // Tooltips because the difference between these is what the mesh does with
    // the packet, which no wording short enough for a menu row conveys.
    menu.setToolTipsVisible(true);

    QAction* flood = menu.addAction(QStringLiteral("Advertise to the Mesh"));
    flood->setToolTip(
        QStringLiteral("Repeaters pass it on, so nodes out of direct range hear it too"));
    QAction* zeroHop = menu.addAction(QStringLiteral("Advertise to Neighbours"));
    zeroHop->setToolTip(QStringLiteral("Zero hop: only nodes in direct radio range hear it"));

    QAction* chosen = ui::popupUnder(menu, advertButton_);
    if (chosen == flood)
        Q_EMIT advertRequested(true);
    else if (chosen == zeroHop)
        Q_EMIT advertRequested(false);
}

void NodePane::setConnection(const QString& text, const QColor& color, bool active,
                             bool connected) {
    QPixmap indicator(QSize(iconSize_, iconSize_) * devicePixelRatioF());
    indicator.setDevicePixelRatio(devicePixelRatioF());
    indicator.fill(Qt::transparent);
    QPainter painter(&indicator);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const int dotSize = theme::scaled(font(), 8);
    const qreal dotInset = (iconSize_ - dotSize) / 2.0;
    painter.drawEllipse(QRectF(dotInset, dotInset, dotSize, dotSize));
    painter.end();

    statusIndicator_->setPixmap(indicator);
    status_->setFullText(text);
    statusRow_->setToolTip(text);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    connected_ = connected;
    updateBatteryDisplay();
    configButton_->setEnabled(connected_ && device_.pubkey.size() == 32);
    // Unlike the configuration window, neither of these needs a device
    // identity: the address book is the node's own and an advert is the node
    // describing itself, so both only take a link.
    contactsButton_->setEnabled(connected_);
    advertButton_->setEnabled(connected_);
    if (active == connectionActive_) return;

    updateConnectionAction(active);
}

void NodePane::updateConnectionAction(bool active) {
    const QString action = active ? QStringLiteral("Disconnect") : QStringLiteral("Connect");
    const QString iconName = active ? QStringLiteral("unplug") : QStringLiteral("plug");
    const QColor hover = active ? theme::Error : theme::Accent;

    QIcon icon;
    icon.addPixmap(icons::tinted(iconName, iconSize_, theme::TextMuted, devicePixelRatioF()),
                   QIcon::Normal);
    icon.addPixmap(icons::tinted(iconName, iconSize_, hover, devicePixelRatioF()), QIcon::Active);
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
