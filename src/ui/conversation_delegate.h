#pragma once

#include <QFont>
#include <QStyledItemDelegate>

#include "ui/avatar.h"

// Draws one row of the conversation sidebar: a disc spanning two lines of text,
// with the name and when it last carried traffic on the first line, and the
// newest message plus an unread pill on the second.
//
// The disc says what kind of conversation this is without a word: a channel
// gets the glyph its key kind is drawn as everywhere else, and a peer gets the
// same monogram the address book and its own messages give it, which is what
// makes a name recognisable at a glance in a list scanned rather than read.
class ConversationDelegate : public QStyledItemDelegate {
public:
    explicit ConversationDelegate(QObject* parent = nullptr);

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

    Avatar avatar_;
};
