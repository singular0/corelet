#include "ui/message_delegate.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>

#include "model/chat_model.h"
#include "ui/row_format.h"
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
// Same disc as a sidebar row's channel icon, so the two lists read as one app.
constexpr int AvatarSize = 30;
constexpr int AvatarGap = 8;
// The send mark starts the metadata on our own messages. It is the same width
// in either state, so a message being taken by the daemon is a repaint and not
// a relayout.
constexpr int MarkSize = 8;
constexpr int MarkGap = 5;

// Bubbles stop well short of the full width so that the left/right alignment
// stays readable as "who said this", and so long lines do not run the whole
// 1280 px.
int bubbleMaxWidth(int viewportWidth) {
    return qMax(MinBubbleWidth, qMin(int(viewportWidth * 0.74), 560));
}

void paintSeparator(QPainter* painter, const QRect& rect, const QFont& font,
                    const QString& label, const QColor& line, const QColor& text) {
    painter->setFont(font);
    const QFontMetrics fm(font);
    const int textW = fm.horizontalAdvance(label);
    const int cy = rect.center().y();
    const int gap = 10;
    const int leftEnd = (rect.width() - textW) / 2 - gap;

    painter->setPen(QPen(line, 1));
    painter->drawLine(MarginX, cy, leftEnd, cy);
    painter->drawLine(rect.width() - leftEnd, cy, rect.width() - MarginX, cy);
    painter->setPen(text);
    painter->drawText(rect, Qt::AlignCenter, label);
}

}  // namespace

MessageDelegate::MessageDelegate(QObject* parent)
    : QStyledItemDelegate(parent), avatar_(QApplication::font()) {
    bodyFont_ = QApplication::font();
    headerFont_ = theme::secondaryFont(bodyFont_);
    separatorFont_ = headerFont_;
    avatarSize_ = theme::scaled(bodyFont_, AvatarSize);
    avatarGap_ = theme::scaled(bodyFont_, AvatarGap);
    markSize_ = theme::scaled(bodyFont_, MarkSize);
    markGap_ = theme::scaled(bodyFont_, MarkGap);
    markAllowance_ = markSize_ + markGap_;
    markPenWidth_ *= qreal(markSize_) / MarkSize;
}

void MessageDelegate::setViewportWidth(int width) {
    if (width == viewportWidth_ || width <= 0) return;
    viewportWidth_ = width;
    if (auto* view = qobject_cast<QAbstractItemView*>(parent())) view->doItemsLayout();
}

QString MessageDelegate::metaText(const QModelIndex& index, int availableWidth) const {
    const QDateTime ts = index.data(model::ChatModel::TimestampRole).toDateTime();
    const QString time = ts.toString(QStringLiteral("HH:mm"));
    if (index.data(model::ChatModel::OutgoingRole).toBool())
        return QStringLiteral("· %1").arg(time);
    if (!index.data(model::ChatModel::HasSignalRole).toBool()) return time;

    const float snr = index.data(model::ChatModel::SnrRole).toFloat();
    const QString hops = ui::hopText(index.data(model::ChatModel::PathLenRole).toInt());

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
    const bool unseenBreak = index.data(model::ChatModel::UnseenBreakRole).toBool();
    const QString text = index.data(model::ChatModel::TextRole).toString();
    const QString sender = index.data(model::ChatModel::SenderRole).toString();

    // Our own messages carry no sender name -- there is nobody to draw -- so the
    // gutter only opens on the incoming side.
    const bool hasAvatar = !outgoing && !sender.isEmpty();
    const int gutter = hasAvatar ? avatarSize_ + avatarGap_ : 0;

    const int maxBubble = bubbleMaxWidth(width - gutter);
    const int maxContent = maxBubble - 2 * PadX;

    const QFontMetrics bodyFm(bodyFont_);
    const QFontMetrics headerFm(headerFont_);

    QRect textBounds =
        bodyFm.boundingRect(QRect(0, 0, maxContent, 0), Qt::TextWordWrap, text);

    const int markAllowance = outgoing ? markAllowance_ : 0;
    const QString name = outgoing ? QString() : sender;
    const QString meta = metaText(index, maxContent - markAllowance);
    // The header is sender on the left and metadata on the right, on one line,
    // so a short message costs two lines rather than three.
    int headerWidth = headerFm.horizontalAdvance(meta) + markAllowance;
    if (!name.isEmpty()) headerWidth += headerFm.horizontalAdvance(name) + 12;

    const int contentWidth = qMax(qMin(textBounds.width(), maxContent), headerWidth);
    const int bubbleWidth = qMin(maxBubble, contentWidth + 2 * PadX);
    const int headerHeight = headerFm.height();
    const int bubbleHeight = 2 * PadY + headerHeight + HeaderGap + textBounds.height();

    Layout l;
    int y = 0;
    if (dayBreak) {
        l.daySeparator = QRect(0, y, width, SeparatorHeight);
        y += SeparatorHeight;
    }
    if (unseenBreak) {
        // Keep this last when both boundaries coincide, so "New messages" is
        // immediately adjacent to the first message it describes.
        l.unseenSeparator = QRect(0, y, width, SeparatorHeight);
        y += SeparatorHeight;
    }

    const int x = outgoing ? width - MarginX - bubbleWidth : MarginX + gutter;
    l.bubble = QRect(x, y, bubbleWidth, bubbleHeight);
    // Top-aligned rather than centred: it belongs with the name on the header
    // line, and a long message would otherwise float it into the middle of the
    // text.
    if (hasAvatar) l.avatar = QRect(MarginX, y, avatarSize_, avatarSize_);
    l.header = QRect(x + PadX, y + PadY, bubbleWidth - 2 * PadX, headerHeight);
    if (outgoing) {
        const int metaWidth = headerFm.horizontalAdvance(meta);
        l.mark = QRect(l.header.right() + 1 - metaWidth - markGap_ - markSize_,
                       l.header.y() + (headerHeight - markSize_) / 2, markSize_, markSize_);
    }
    l.text = QRect(x + PadX, l.header.bottom() + 1 + HeaderGap, bubbleWidth - 2 * PadX,
                   textBounds.height());
    return l;
}

void MessageDelegate::paintMark(QPainter* painter, const QRect& mark, bool pending) const {
    QPen pen(pending ? theme::TextMuted : theme::Accent, markPenWidth_);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);

    const QRectF m(mark);
    if (pending) {
        // An empty ring: the send is on its way and nothing has confirmed it.
        painter->drawEllipse(m.adjusted(1, 1, -1, -1));
        return;
    }
    painter->drawPolyline(QPolygonF {QPointF(m.left(), m.top() + m.height() * 0.55),
                                     QPointF(m.left() + m.width() * 0.36, m.bottom()),
                                     QPointF(m.right(), m.top() + m.height() * 0.1)});
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
    const Layout l = layoutFor(index, viewportWidth_);
    // A one-line bubble is still taller than the disc, but not by much, so the
    // row takes whichever of the two reaches lower.
    return QSize(viewportWidth_, qMax(l.bubble.bottom(), l.avatar.bottom()) + 1 + RowGap);
}

void MessageDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    const Layout l = layoutFor(index, option.rect.width());
    const bool outgoing = index.data(model::ChatModel::OutgoingRole).toBool();

    painter->save();
    painter->translate(option.rect.topLeft());
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (!l.daySeparator.isNull()) {
        const QDateTime ts = index.data(model::ChatModel::TimestampRole).toDateTime();
        const QDate date = ts.date();
        QString label = date.toString(QStringLiteral("d MMMM yyyy"));
        if (date == QDate::currentDate())
            label = QStringLiteral("Today");
        else if (date == QDate::currentDate().addDays(-1))
            label = QStringLiteral("Yesterday");

        paintSeparator(painter, l.daySeparator, separatorFont_, label, theme::Border,
                       theme::TextMuted);
    }

    if (!l.unseenSeparator.isNull())
        paintSeparator(painter, l.unseenSeparator, separatorFont_,
                       QStringLiteral("New messages"), theme::Accent, theme::Accent);

    painter->setPen(Qt::NoPen);
    painter->setBrush(outgoing ? theme::Outgoing : theme::Surface);
    painter->drawRoundedRect(l.bubble, BubbleRadius, BubbleRadius);

    const QString sender = index.data(model::ChatModel::SenderRole).toString();
    // The mark is drawn separately because the target system font may not have
    // a tick glyph; the dot and timestamp remain ordinary right-aligned text.
    const int markAllowance = l.mark.isNull() ? 0 : markAllowance_;
    const QString meta = metaText(index, l.header.width() - markAllowance);

    if (!l.avatar.isNull()) avatar_.paint(painter, l.avatar, sender);

    painter->setFont(headerFont_);
    if (!outgoing && !sender.isEmpty()) {
        painter->setPen(theme::senderColor(sender));
        painter->drawText(l.header, Qt::AlignLeft | Qt::AlignVCenter, sender);
    }
    painter->setPen(theme::TextMuted);
    painter->drawText(l.header, Qt::AlignRight | Qt::AlignVCenter, meta);

    if (!l.mark.isNull())
        paintMark(painter, l.mark,
                  index.data(model::ChatModel::SendStateRole).toInt() ==
                      int(model::Message::SendState::Pending));

    painter->setFont(bodyFont_);
    painter->setPen(theme::Text);
    painter->drawText(l.text, Qt::TextWordWrap,
                      index.data(model::ChatModel::TextRole).toString());

    painter->restore();
}
