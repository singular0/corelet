#pragma once

#include <QColor>
#include <QLabel>

class QLineEdit;

// The byte budget of a text field, made visible and kept true: it shows the room
// left and holds the field inside it, so the count is never asked to show a
// negative number. Every field that reaches the wire has one, because the limit
// is encoded bytes (see protocol/text_limits.h) and QLineEdit::setMaxLength --
// the only cap it has -- counts characters, which is a different number: the
// same 32-byte field holds 32 Latin letters, 16 Cyrillic ones or 8 emoji.
class ByteCounter : public QLabel {
public:
    explicit ByteCounter(QWidget* parent = nullptr);

    // Takes charge of `field`. What is typed or pasted past `budget` bytes is
    // dropped, exactly as setMaxLength drops what is past its character count.
    void attach(QLineEdit* field, int budget);

    // A budget can move under the user: a message's depends on the node's name,
    // which arrives with the handshake and differs from node to node. Text that
    // no longer fits is cut from the end there and then, since there is no
    // honest count to show for a field that is over.
    void setBudget(int budget);

private:
    // `atCursor` says where an overflow came from. An edit put it in at the
    // cursor and that is where it comes back out, so typing into a full field
    // does nothing rather than eating a character somewhere else; a budget that
    // shrank under text which already fit inserted nothing anywhere, and comes
    // off the end.
    void enforce(bool atCursor);

    QLineEdit* field_ = nullptr;
    int budget_ = 0;
    QColor color_;  // invalid until the first paint, so that one always applies
};
