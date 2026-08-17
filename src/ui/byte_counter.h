#pragma once

#include <QColor>
#include <QFontMetrics>
#include <QLabel>
#include <QString>

#include "ui/theme.h"

// The "room left" readout beside a field whose limit is a byte budget rather
// than a character count -- which is every field that reaches the wire, see
// protocol/text_limits.h. Without one the limit is invisible until the app
// refuses to send, and a character count would be the wrong number anyway: the
// same box holds 32 Latin letters or 8 emoji.
class ByteCounter : public QLabel {
public:
    explicit ByteCounter(QWidget* parent = nullptr) : QLabel(parent) {
        setFont(theme::secondaryFont(font()));
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // Fixed rather than hinted from the text: the width would otherwise
        // change with every digit and shove whatever shares the row with it.
        // Sized for the widest thing it can say -- a three-digit budget, an
        // over-budget minus sign and all.
        setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("-000/000")));
    }

    // Both arguments are byte counts. Going over is shown rather than clamped:
    // how far over is what says how much has to come back out.
    void setUsed(int used, int budget) {
        constexpr int WarningThreshold = 10;
        const int left = budget - used;
        setText(QStringLiteral("%1/%2").arg(left).arg(budget));

        const QColor color = left < 0                   ? theme::Error
                             : left <= WarningThreshold ? theme::Warning
                                                        : theme::TextMuted;
        // A stylesheet assignment re-polishes the widget, and this runs on
        // every keystroke, so only say it when the colour actually changed. The
        // disabled rule goes with it: a stylesheet colour otherwise outranks the
        // palette, and a counter left glowing red beside a field the user has
        // just switched away from is saying something about nothing.
        if (color == color_) return;
        color_ = color;
        setStyleSheet(QStringLiteral("QLabel { color: %1; } QLabel:disabled { color: %2; }")
                          .arg(color.name(), theme::TextMuted.name()));
    }

private:
    QColor color_;  // invalid until the first setUsed(), so that one always applies
};
