#include "ui/channeldelegate.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QLocale>
#include <QPainter>
#include <QWidget>

#include "model/channelmodel.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace {

// Sidebar timestamps. A channel list is scanned rather than read, so anything
// from the last day is the clock time alone; older than that and the date has
// to come along or the time means nothing.
QString activityStamp(const QDateTime& when) {
    if (!when.isValid()) return {};
    const QString time = QLocale().toString(when.time(), QLocale::ShortFormat);
    // Timestamps come off the mesh and can sit slightly in the future when a
    // sender's clock runs fast; that still counts as just now.
    if (when.secsTo(QDateTime::currentDateTime()) < 24 * 60 * 60) return time;
    return QStringLiteral("%1 %2").arg(
        QLocale().toString(when.date(), QStringLiteral("d MMM")), time);
}

QString iconName(model::ChannelType type) {
    switch (type) {
        case model::ChannelType::Public: return QStringLiteral("globe");
        case model::ChannelType::Hashtag: return QStringLiteral("hash");
        case model::ChannelType::Private: break;
    }
    return QStringLiteral("lock");
}

// The second line and the stamp sit a step below the name, which is what keeps
// the name the thing the eye lands on.
QFont subFont(const QFont& base) {
    QFont f = base;
    f.setPointSizeF(qMax(6.5, base.pointSizeF() - 1.5));
    return f;
}

QFont nameFont(const QFont& base) {
    QFont f = base;
    f.setBold(true);
    return f;
}

}  // namespace

QSize ChannelDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    const int text = QFontMetrics(nameFont(option.font)).height() + LineGap +
                     QFontMetrics(subFont(option.font)).height();
    return QSize(0, qMax(text, IconSize) + 2 * VerticalPadding);
}

void ChannelDelegate::paint(QPainter* p, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
    const bool selected = option.state & QStyle::State_Selected;
    const int unread = index.data(model::ChannelModel::UnreadRole).toInt();

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    if (selected) {
        p->fillRect(option.rect, theme::SidebarSelected);
        // A left bar reads as "this one" even on the uConsole's washed-out
        // viewing angles, where the fill alone is easy to miss.
        p->fillRect(QRect(option.rect.left(), option.rect.top(), 2, option.rect.height()),
                    theme::Accent);
    }

    QRect r = option.rect.adjusted(10, 0, -8, 0);

    // --- type icon, spanning both lines --------------------------------------
    const auto type = model::channelTypeFromInt(index.data(model::ChannelModel::TypeRole).toInt());
    const QColor iconColor = selected     ? theme::Text
                             : unread > 0 ? theme::Accent
                                          : theme::TextMuted;
    const qreal dpr = option.widget ? option.widget->devicePixelRatioF() : 1.0;
    const QRect iconRect(r.left(), r.top() + (r.height() - IconSize) / 2, IconSize, IconSize);
    p->setPen(Qt::NoPen);
    p->setBrush(theme::IconBackground);
    p->drawEllipse(iconRect);
    const QRect glyphRect =
        iconRect.adjusted(GlyphInset, GlyphInset, -GlyphInset, -GlyphInset);
    p->drawPixmap(glyphRect, icons::tinted(iconName(type), glyphRect.width(), iconColor, dpr));
    r.setLeft(iconRect.right() + 10);

    const QFont nameF = nameFont(option.font);
    const QFont subF = subFont(option.font);
    const QFontMetrics nameFm(nameF);
    const QFontMetrics subFm(subF);

    // Both lines as a block, centred against the icon beside them.
    const int block = nameFm.height() + LineGap + subFm.height();
    const int top = r.top() + (r.height() - block) / 2;
    QRect topLine(r.left(), top, r.width(), nameFm.height());
    QRect bottomLine(r.left(), top + nameFm.height() + LineGap, r.width(), subFm.height());

    // A name elided to nothing tells you less than the time or the count does,
    // so those two hold their width and the text lines give way around them.
    const int minTextWidth = 3 * option.fontMetrics.averageCharWidth();

    // --- last message time, right of the name --------------------------------
    const QString stamp =
        activityStamp(index.data(model::ChannelModel::LastActivityRole).toDateTime());
    if (!stamp.isEmpty()) {
        const int stampWidth = subFm.horizontalAdvance(stamp);
        if (stampWidth + 6 <= topLine.width() - minTextWidth) {
            p->setFont(subF);
            p->setPen(theme::TextMuted);
            p->drawText(QRect(topLine.right() - stampWidth, topLine.top(), stampWidth,
                              topLine.height()),
                        Qt::AlignRight | Qt::AlignVCenter, stamp);
            topLine.setRight(topLine.right() - stampWidth - 6);
        }
    }

    // --- unread pill, right of the preview -----------------------------------
    if (unread > 0) {
        const QString badge = unread > 99 ? QStringLiteral("99+") : QString::number(unread);
        QFont badgeFont = subF;
        badgeFont.setBold(true);
        const QFontMetrics bfm(badgeFont);
        const int badgeWidth = qMax(bfm.horizontalAdvance(badge) + 10, 18);
        const int badgeHeight = bfm.height() + 2;

        // Never let the pill start left of the icon, however far the splitter
        // has been dragged in: an unread count is the one thing on the row that
        // must stay readable.
        const QRect pill(qMax(bottomLine.left(), bottomLine.right() - badgeWidth),
                         bottomLine.top() + (bottomLine.height() - badgeHeight) / 2, badgeWidth,
                         badgeHeight);
        p->setPen(Qt::NoPen);
        p->setBrush(theme::Accent);
        p->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        p->setFont(badgeFont);
        p->setPen(QColor(0x11, 0x22, 0x1e));
        p->drawText(pill, Qt::AlignCenter, badge);
        bottomLine.setRight(pill.left() - 6);
    }

    // --- name ----------------------------------------------------------------
    if (topLine.width() > 0) {
        p->setFont(nameF);
        p->setPen(theme::Text);
        p->drawText(topLine, Qt::AlignLeft | Qt::AlignVCenter,
                    nameFm.elidedText(index.data(model::ChannelModel::NameRole).toString(),
                                      Qt::ElideRight, topLine.width()));
    }

    // --- newest message ------------------------------------------------------
    const QString preview = index.data(model::ChannelModel::PreviewRole).toString();
    if (!preview.isEmpty() && bottomLine.width() > 0) {
        p->setFont(subF);
        p->setPen(theme::TextMuted);
        p->drawText(bottomLine, Qt::AlignLeft | Qt::AlignVCenter,
                    subFm.elidedText(preview, Qt::ElideRight, bottomLine.width()));
    }
    p->restore();
}
