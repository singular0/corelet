#pragma once

#include <QFont>
#include <QStyledItemDelegate>

#include "ui/avatar.h"

// Draws one row of the address book: a disc spanning three lines of text, with
// the node's name, its public key, and when its last advert was heard and how
// far away it came from.
//
// The disc is the conversation's sender avatar for a person and a type glyph for
// anything else on the mesh -- a repeater or a sensor has no name worth reducing
// to an initial, and what it *is* is the thing worth seeing at a glance.
class ContactDelegate : public QStyledItemDelegate {
public:
    explicit ContactDelegate(QObject* parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

private:
    // Larger than the sidebar's, which spans two lines rather than three.
    static constexpr int IconSize = 36;
    // The glyph sits inset inside the disc, leaving a ring of background around
    // it so the circle reads as a backdrop and not as a clipped icon.
    static constexpr int GlyphInset = 9;
    static constexpr int LineGap = 2;
    static constexpr int VerticalPadding = 7;

    Avatar avatar_;
    QFont nameFont_;
    QFont metaFont_;
    // The key is 64 hex characters of no particular shape; a fixed pitch is what
    // makes comparing one against a written-down copy possible at all.
    QFont keyFont_;
};
