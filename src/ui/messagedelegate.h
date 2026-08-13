#pragma once

#include <QStyledItemDelegate>

// Draws one message as a bubble: incoming left, our own right, with a header
// line carrying the sender and the arrival metadata.
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
        QRect separator;  // empty unless this row starts a new day
    };

    Layout layoutFor(const QModelIndex& index, int width) const;
    QString metaText(const QModelIndex& index, int availableWidth) const;

    int viewportWidth_ = 400;
    QFont headerFont_;
    QFont bodyFont_;
    QFont separatorFont_;
};
