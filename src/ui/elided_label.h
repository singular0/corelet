#pragma once

#include <QLabel>
#include <QResizeEvent>
#include <QString>

// A one-line label that gives way instead of widening the pane it sits in.
// QLabel neither elides its text nor forgets the width the full string wants,
// so a long device name or a BLE scan message in the sidebar would push the
// splitter open and stay there. Both are fixed here: the text is elided to
// whatever width the label is given, and the minimum width is zero.
class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;

    void setFullText(const QString& text) {
        if (text == full_) return;
        full_ = text;
        applyElision();
    }

    QSize minimumSizeHint() const override {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        applyElision();
    }

private:
    void applyElision() {
        // QLabel::setText ignores an unchanged string, so this cannot bounce
        // between here and resizeEvent.
        setText(fontMetrics().elidedText(full_, Qt::ElideRight, width()));
    }

    QString full_;
};
