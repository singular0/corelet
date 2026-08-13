#include "ui/node_pane.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <QVBoxLayout>

#include "ui/elided_label.h"
#include "ui/theme.h"

namespace {

// Everything but the node's own name is a footnote to the channel list above
// it, and sits a step below the body font — the same step the sidebar rows use.
QFont subFont(const QFont& base) {
    QFont f = base;
    f.setPointSizeF(qMax(6.5, base.pointSizeF() - 1.5));
    return f;
}

ElidedLabel* addRow(QVBoxLayout* layout, const QFont& font, const QColor& color) {
    auto* label = new ElidedLabel;
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    // The pane as a whole is the click target; a label that took mouse events
    // would leave dead spots across most of it.
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(label);
    return label;
}

}  // namespace

NodePane::NodePane(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("nodePane"));
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Connect to a different daemon or device"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 5, 8, 6);
    layout->setSpacing(1);

    QFont nameFont = font();
    nameFont.setBold(true);

    // Rows that can vanish go on top: the pane is anchored to the bottom of the
    // window, so the target and the link state keep their place on screen when
    // the device's answer arrives and pushes the rest upwards.
    name_ = addRow(layout, nameFont, theme::Text);
    radio_ = addRow(layout, subFont(font()), theme::TextMuted);
    target_ = addRow(layout, subFont(font()), theme::TextMuted);
    status_ = addRow(layout, subFont(font()), theme::TextMuted);

    name_->hide();
    radio_->hide();
}

void NodePane::setTarget(const QString& label) {
    target_->setFullText(label);
}

void NodePane::setDevice(const proto::CompanionClient::DeviceInfo& info) {
    name_->setFullText(info.name);
    name_->setVisible(!info.name.isEmpty());

    // Frequency and spreading factor decide who can hear this node at all,
    // which is what makes them worth carrying in a chat window; the rest of the
    // radio settings are the daemon's business.
    radio_->setFullText(
        QStringLiteral("%1 MHz · SF%2").arg(info.freqMhz, 0, 'f', 3).arg(info.sf));
    radio_->setVisible(info.freqMhz > 0);
}

void NodePane::setConnection(const QString& text, const QColor& color) {
    status_->setFullText(QStringLiteral("● %1").arg(text));
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

// A QWidget subclass draws no stylesheet background or border of its own, and
// the border is the only thing separating this pane from the last channel row.
void NodePane::paintEvent(QPaintEvent*) {
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void NodePane::mousePressEvent(QMouseEvent* event) {
    // Accepting the press is what makes the matching release arrive here.
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    event->accept();
}

void NodePane::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    // A press that wanders off the pane before release is a cancelled click.
    if (rect().contains(event->position().toPoint())) Q_EMIT connectRequested();
}

void NodePane::enterEvent(QEnterEvent* event) {
    QWidget::enterEvent(event);
    setHovered(true);
}

void NodePane::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    setHovered(false);
}

// The target line brightens under the pointer: without it nothing says the
// pane can be clicked, and a tooltip alone is no use on a touchscreen.
void NodePane::setHovered(bool hovered) {
    target_->setStyleSheet(
        QStringLiteral("color: %1;").arg((hovered ? theme::Text : theme::TextMuted).name()));
}
