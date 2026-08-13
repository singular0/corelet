#include "ui/node_pane.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/elided_label.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

// Everything but the node's own name is a footnote to the channel list above
// it, and sits a step below the body font — the same step the sidebar rows use.
QFont subFont(const QFont& base) {
    QFont f = base;
    f.setPointSizeF(qMax(6.5, base.pointSizeF() - 1.5));
    return f;
}

ElidedLabel* addRow(QVBoxLayout* layout, const QFont& font, const QColor& color,
                    const QString& iconName = {}, QLabel** iconOut = nullptr) {
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(5);

    if (!iconName.isEmpty()) {
        constexpr int IconSize = 14;
        auto* icon = new QLabel;
        icon->setFixedSize(IconSize, IconSize);
        icon->setPixmap(icons::tinted(iconName, IconSize, theme::TextMuted,
                                      layout->parentWidget()->devicePixelRatioF()));
        icon->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(icon);
        if (iconOut) *iconOut = icon;
    }

    auto* label = new ElidedLabel;
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    rowLayout->addWidget(label, 1);
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

    connectionButton_ = new QToolButton;
    connectionButton_->setObjectName(QStringLiteral("iconButton"));
    connectionButton_->setIconSize(QSize(14, 14));
    connectionButton_->setAutoRaise(true);
    connectionButton_->setCursor(Qt::PointingHandCursor);
    connectionButton_->setFocusPolicy(Qt::NoFocus);
    updateConnectionAction(false);

    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(connectionButton_);

    auto* details = new QWidget;
    auto* detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(10, 5, 8, 6);
    detailsLayout->setSpacing(1);

    QFont nameFont = font();
    nameFont.setBold(true);

    name_ = addRow(detailsLayout, nameFont, theme::Text, QStringLiteral("radio-tower"));
    target_ = addRow(detailsLayout, subFont(font()), theme::TextMuted,
                     QStringLiteral("server"), &targetIcon_);
    battery_ = addRow(detailsLayout, subFont(font()), theme::TextMuted,
                      QStringLiteral("battery-medium"));
    status_ = addRow(detailsLayout, subFont(font()), theme::TextMuted);

    name_->setFullText(QStringLiteral("Node unavailable"));
    battery_->setFullText(QStringLiteral("Battery unavailable"));

    layout->addWidget(header);
    layout->addWidget(details);

    connect(connectionButton_, &QToolButton::clicked, this, [this] {
        if (connectionActive_)
            Q_EMIT disconnectRequested();
        else
            Q_EMIT connectRequested();
    });
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
    name_->setFullText(info.name.isEmpty() ? QStringLiteral("Node unavailable") : info.name);
    battery_->setFullText(info.batteryPercent < 0
                              ? QStringLiteral("Battery unavailable")
                              : QStringLiteral("%1%").arg(info.batteryPercent));
}

void NodePane::setConnection(const QString& text, const QColor& color, bool active) {
    status_->setFullText(QStringLiteral("● %1").arg(text));
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
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
