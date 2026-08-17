#pragma once

#include <QObject>

class QLineEdit;

// Holds a text field inside a budget of encoded bytes. This is what
// QLineEdit::setMaxLength would be if it counted encoded bytes rather than
// characters, which is the whole reason it exists: every limit that reaches the
// wire is in bytes (see protocol/text_limits.h), and the same 32-byte field
// holds 32 Latin letters, 16 Cyrillic ones or 8 emoji.
//
// Nothing about it is visible. A field that has stopped taking characters is
// the same signal a maximum length has always given, and a byte count on screen
// only invites the question of why an emoji costs four of them.
class ByteLimit : public QObject {
public:
    // Takes over `field`, which also becomes its parent, so the limit lives
    // exactly as long as the field it applies to.
    ByteLimit(QLineEdit* field, int budget);

    // A budget can move under the user: a message's depends on the node's name,
    // which arrives with the handshake and differs from node to node. Text that
    // no longer fits is cut from the end there and then.
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
};
