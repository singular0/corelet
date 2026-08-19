#pragma once

#include <QFont>
#include <QString>
#include <QStyledItemDelegate>

#include "model/types.h"
#include "ui/avatar.h"

// Draws one message as a bubble: incoming left, our own right, with a header
// line carrying the sender and the arrival metadata, and a round sender avatar
// in the left gutter.
//
// A delegate rather than a QTextBrowser full of HTML because the view then
// only lays out and paints the rows on screen. On a CM4 driving a long channel
// history that is the difference between instant and visibly slow.
class MessageDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit MessageDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

    // QListView does not hand sizeHint a usable width, so the view pushes its
    // viewport width here on every resize and relayouts.
    void setViewportWidth(int width);

private:
    struct Layout {
        QRect bubble;
        QRect header;
        QRect text;
        QRect avatar;     // empty for our own messages, which carry no sender
        QRect daySeparator;     // empty unless this row starts a new day
        QRect unseenSeparator;  // empty unless this is the first unseen row
        QRect mark;       // send state; empty for anything we did not send
    };

    Layout layoutFor(const QModelIndex& index, int width) const;
    QString metaText(const QModelIndex& index, int availableWidth) const;
    // Ring or tick, drawn rather than written: a bare Debian install is not
    // guaranteed a font with U+2713 in it, and two strokes cost less per row
    // than laying out another piece of text.
    void paintMark(QPainter* painter, const QRect& mark,
                   model::Message::SendState state) const;

    int viewportWidth_ = 400;
    QFont headerFont_;
    QFont bodyFont_;
    QFont separatorFont_;
    Avatar avatar_;
    int avatarSize_ = 30;
    int avatarGap_ = 8;
    int markSize_ = 8;
    int markGap_ = 5;
    int markAllowance_ = 13;
    qreal markPenWidth_ = 1.4;
};
