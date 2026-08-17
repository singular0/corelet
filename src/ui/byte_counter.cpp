#include "ui/byte_counter.h"

#include <QFontMetrics>
#include <QLineEdit>

#include "protocol/text_limits.h"
#include "ui/theme.h"

namespace {

// Far enough out to be worth noticing, close enough that it is not amber for
// most of a message.
constexpr int WarningThreshold = 10;

}  // namespace

ByteCounter::ByteCounter(QWidget* parent) : QLabel(parent) {
    setFont(theme::secondaryFont(font()));
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Fixed rather than hinted from the text: the width would otherwise change
    // with every digit and shove whatever shares the row with it.
    setFixedWidth(fontMetrics().horizontalAdvance(QStringLiteral("000/000")));
}

void ByteCounter::attach(QLineEdit* field, int budget) {
    field_ = field;
    budget_ = budget;
    // textChanged rather than textEdited: a field is also written to from code
    // -- the text of a failed send is handed back to the message box -- and that
    // has to fit as much as anything typed does.
    connect(field_, &QLineEdit::textChanged, this, [this] { enforce(true); });
    enforce(false);
}

void ByteCounter::setBudget(int budget) {
    budget_ = budget;
    enforce(false);
}

void ByteCounter::enforce(bool atCursor) {
    if (!field_) return;

    const QString current = field_->text();
    if (proto::utf8Bytes(current) > budget_) {
        QString kept;
        int cursor = 0;
        if (atCursor) {
            // Everything from the cursor on was already in the field and already
            // fitted, so only what is in front of it can be given back.
            const QString tail = current.mid(field_->cursorPosition());
            const QString head = proto::clampToUtf8Bytes(
                current.left(field_->cursorPosition()), budget_ - proto::utf8Bytes(tail));
            kept = head + tail;
            cursor = int(head.size());
        }
        if (!atCursor || proto::utf8Bytes(kept) > budget_) {
            kept = proto::clampToUtf8Bytes(current, budget_);
            cursor = int(kept.size());
        }
        // This comes straight back through here with the shortened text, which
        // then finds nothing left to do. The count below is painted either way,
        // since a field handed the text it already had reports no change at all.
        field_->setText(kept);
        field_->setCursorPosition(cursor);
    }

    // Read back rather than reused: what the field holds now is what fits.
    const int left = budget_ - proto::utf8Bytes(field_->text());
    setText(QStringLiteral("%1/%2").arg(left).arg(budget_));

    const QColor color = left <= WarningThreshold ? theme::Warning : theme::TextMuted;
    // A stylesheet assignment re-polishes the widget and this runs on every
    // keystroke, so only say it when the colour actually changed. The disabled
    // rule goes with it: a stylesheet colour otherwise outranks the palette, and
    // a counter left amber beside a field the user has switched away from is
    // saying something about nothing.
    if (color == color_) return;
    color_ = color;
    setStyleSheet(QStringLiteral("QLabel { color: %1; } QLabel:disabled { color: %2; }")
                      .arg(color.name(), theme::TextMuted.name()));
}
