#pragma once

#include <QStyledItemDelegate>

// Draws one row of the channel sidebar: a type icon on a round background
// spanning two lines of text, with the channel name and when it last carried
// traffic on the first line, and the newest message plus an unread pill on the
// second.
class ChannelDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    // Big enough to read as the row's anchor next to two lines of text, which is
    // roughly what those two lines are tall.
    static constexpr int IconSize = 30;
    // The glyph sits inset inside the disc, leaving a ring of background around
    // it so the circle reads as a backdrop and not as a clipped icon.
    static constexpr int GlyphInset = 7;
    static constexpr int LineGap = 2;
    static constexpr int VerticalPadding = 6;
};
