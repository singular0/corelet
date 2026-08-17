#include "ui/byte_limit.h"

#include <QLineEdit>

#include "protocol/text_limits.h"

ByteLimit::ByteLimit(QLineEdit* field, int budget)
    : QObject(field), field_(field), budget_(budget) {
    // textChanged rather than textEdited: a field is also written to from code
    // -- the text of a failed send is handed back to the message box -- and that
    // has to fit as much as anything typed does.
    connect(field_, &QLineEdit::textChanged, this, [this] { enforce(true); });
    enforce(false);
}

void ByteLimit::setBudget(int budget) {
    budget_ = budget;
    enforce(false);
}

void ByteLimit::enforce(bool atCursor) {
    const QString current = field_->text();
    if (proto::utf8Bytes(current) <= budget_) return;

    QString kept;
    int cursor = 0;
    if (atCursor) {
        // Everything from the cursor on was already in the field and already
        // fitted, so only what is in front of it can be given back.
        const QString tail = current.mid(field_->cursorPosition());
        const QString head = proto::clampToUtf8Bytes(current.left(field_->cursorPosition()),
                                                     budget_ - proto::utf8Bytes(tail));
        kept = head + tail;
        cursor = int(head.size());
    }
    if (!atCursor || proto::utf8Bytes(kept) > budget_) {
        kept = proto::clampToUtf8Bytes(current, budget_);
        cursor = int(kept.size());
    }

    // This comes straight back through here with the shortened text, which then
    // finds nothing left to do.
    field_->setText(kept);
    field_->setCursorPosition(cursor);
}
