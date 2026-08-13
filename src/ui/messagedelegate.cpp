#include "ui/messagedelegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>

#include "model/chatmodel.h"
#include "ui/theme.h"

namespace {

// Tight by desktop standards, because the uConsole panel is 480 px tall and
// every pixel spent on padding is a pixel of conversation not shown.
constexpr int PadX = 9;
constexpr int PadY = 5;
constexpr int RowGap = 5;
constexpr int MarginX = 8;
constexpr int HeaderGap = 2;
constexpr int SeparatorHeight = 22;
constexpr int BubbleRadius = 6;
constexpr int MinBubbleWidth = 90;

// Bubbles stop well short of the full width so that the left/right alignment
// stays readable as "who said this", and so long lines do not run the whole
// 1280 px.
int bubbleMaxWidth(int viewportWidth) {
    return qMax(MinBubbleWidth, qMin(int(viewportWidth * 0.74), 560));
}

}  // namespace

MessageDelegate::MessageDelegate(QObject* parent) : QStyledItemDelegate(parent) {
    bodyFont_ = QApplication::font();
    headerFont_ = bodyFont_;
    headerFont_.setPointSizeF(qMax(6.5, bodyFont_.pointSizeF() - 1.5));
    separatorFont_ = headerFont_;
}

void MessageDelegate::setViewportWidth(int width) {
    if (width == viewportWidth_ || width <= 0) return;
    viewportWidth_ = width;
    if (auto* view = qobject_cast<QAbstractItemView*>(parent())) view->doItemsLayout();
}

QString MessageDelegate::metaText(const QModelIndex& index, int availableWidth) const {
    const QDateTime ts = index.data(model::ChatModel::TimestampRole).toDateTime();
    const QString time = ts.toString(QStringLiteral("HH:mm"));
    if (!index.data(model::ChatModel::HasSignalRole).toBool()) return time;

    const float snr = index.data(model::ChatModel::SnrRole).toFloat();
    const int pathLen = index.data(model::ChatModel::PathLenRole).toInt();
    const QString hops =
        pathLen == 0xFF ? QStringLiteral("flood") : QStringLiteral("%1 hop").arg(pathLen);

    // Degrade rather than elide: the time is what a reader looks for first, so
    // drop routing, then signal, until what is left fits the bubble.
    const QFontMetrics fm(headerFont_);
    const QString full =
        QStringLiteral("%1 dB · %2 · %3").arg(snr, 0, 'f', 1).arg(hops, time);
    if (fm.horizontalAdvance(full) <= availableWidth) return full;

    const QString medium = QStringLiteral("%1 dB · %2").arg(snr, 0, 'f', 1).arg(time);
    if (fm.horizontalAdvance(medium) <= availableWidth) return medium;
    return time;
}

MessageDelegate::Layout MessageDelegate::layoutFor(const QModelIndex& index, int width) const {
    const bool outgoing = index.data(model::ChatModel::OutgoingRole).toBool();
    const bool dayBreak = index.data(model::ChatModel::DayBreakRole).toBool();
    const QString text = index.data(model::ChatModel::TextRole).toString();
    const QString sender = index.data(model::ChatModel::SenderRole).toString();

    const int maxBubble = bubbleMaxWidth(width);
    const int maxContent = maxBubble - 2 * PadX;

    const QFontMetrics bodyFm(bodyFont_);
    const QFontMetrics headerFm(headerFont_);

    QRect textBounds =
        bodyFm.boundingRect(QRect(0, 0, maxContent, 0), Qt::TextWordWrap, text);

    const QString name = outgoing ? QString() : sender;
    const QString meta = metaText(index, maxContent);
    // The header is sender on the left and metadata on the right, on one line,
    // so a short message costs two lines rather than three.
    int headerWidth = headerFm.horizontalAdvance(meta);
    if (!name.isEmpty()) headerWidth += headerFm.horizontalAdvance(name) + 12;

    const int contentWidth = qMax(qMin(textBounds.width(), maxContent), headerWidth);
    const int bubbleWidth = qMin(maxBubble, contentWidth + 2 * PadX);
    const int headerHeight = headerFm.height();
    const int bubbleHeight = 2 * PadY + headerHeight + HeaderGap + textBounds.height();

    Layout l;
    int y = 0;
    if (dayBreak) {
        l.separator = QRect(0, 0, width, SeparatorHeight);
        y = SeparatorHeight;
    }

    const int x = outgoing ? width - MarginX - bubbleWidth : MarginX;
    l.bubble = QRect(x, y, bubbleWidth, bubbleHeight);
    l.header = QRect(x + PadX, y + PadY, bubbleWidth - 2 * PadX, headerHeight);
    l.text = QRect(x + PadX, l.header.bottom() + 1 + HeaderGap, bubbleWidth - 2 * PadX,
                   textBounds.height());
    return l;
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    const Layout l = layoutFor(index, viewportWidth_);
    return QSize(viewportWidth_, l.bubble.bottom() + 1 + RowGap);
}

void MessageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    const Layout l = layoutFor(index, option.rect.width());
    const bool outgoing = index.data(model::ChatModel::OutgoingRole).toBool();

    painter->save();
    painter->translate(option.rect.topLeft());
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (!l.separator.isNull()) {
        const QDateTime ts = index.data(model::ChatModel::TimestampRole).toDateTime();
        const QDate date = ts.date();
        QString label = date.toString(QStringLiteral("d MMMM yyyy"));
        if (date == QDate::currentDate())
            label = QStringLiteral("Today");
        else if (date == QDate::currentDate().addDays(-1))
            label = QStringLiteral("Yesterday");

        painter->setFont(separatorFont_);
        const QFontMetrics fm(separatorFont_);
        const int textW = fm.horizontalAdvance(label);
        const int cy = l.separator.center().y();
        const int gap = 10;
        const int leftEnd = (l.separator.width() - textW) / 2 - gap;

        painter->setPen(QPen(theme::Border, 1));
        painter->drawLine(MarginX, cy, leftEnd, cy);
        painter->drawLine(l.separator.width() - leftEnd, cy, l.separator.width() - MarginX, cy);
        painter->setPen(theme::TextMuted);
        painter->drawText(l.separator, Qt::AlignCenter, label);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? theme::Outgoing : theme::Surface);
    painter->drawRoundedRect(l.bubble, BubbleRadius, BubbleRadius);

    const QString sender = index.data(model::ChatModel::SenderRole).toString();
    const QString meta = metaText(index, l.header.width());

    painter->setFont(headerFont_);
    if (!outgoing && !sender.isEmpty()) {
        painter->setPen(theme::senderColor(sender));
        painter->drawText(l.header, Qt::AlignLeft | Qt::AlignVCenter, sender);
    }
    painter->setPen(theme::TextMuted);
    painter->drawText(l.header, Qt::AlignRight | Qt::AlignVCenter, meta);

    painter->setFont(bodyFont_);
    painter->setPen(theme::Text);
    painter->drawText(l.text, Qt::TextWordWrap,
                      index.data(model::ChatModel::TextRole).toString());

    painter->restore();
}
