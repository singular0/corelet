#pragma once

#include <QString>
#include <QTextBoundaryFinder>

#include "protocol/protocol.h"

// How much user-typed text actually fits on the wire. Every limit here is a
// count of encoded UTF-8 bytes, never of QString characters: a Cyrillic letter
// costs two bytes and an emoji four, so a character count promises room that is
// not there and hands the user a refusal from the daemon after they have
// written the message.
namespace proto {

// A channel name occupies its fixed field whole. Nothing follows it inside the
// field, so a name filling all 32 bytes needs no terminator and both ends read
// it back complete.
inline constexpr int MaxChannelNameBytes = ChannelNameField;

// A channel message rides in one 184-byte mesh payload:
//
//   184   payload
//    -1   channel hash
//    -2   MAC
//   ----
//   181   ciphertext, which AES-ECB pads out to whole 16-byte blocks, so 176
//         is the most that can be carried
//    -5   text header: 4-byte timestamp, 1 byte of type and attempt
//   ----
//   171   "SenderName: body"
//
// The sending node prepends its own advertised name, which the app knows from
// SELF_INFO, so what is left for the body depends on that name.
inline constexpr int MaxChannelTextBytes = 171;
inline constexpr int SenderSeparatorBytes = 2;  // the ": " between name and body

// A direct message rides the same 184-byte payload under a different envelope
// -- a destination hash, a source hash and the MAC rather than a channel hash
// and the MAC -- but one more header byte still leaves 176 whole AES blocks, so
// the arithmetic lands on the same 171. What differs is that nothing is
// prepended: the recipient knows who it is from because only their key opens
// it, so the whole budget is the body and it does not move with the node's name.
inline constexpr int MaxDirectTextBytes = MaxChannelTextBytes;

inline int utf8Bytes(const QString& text) { return int(text.toUtf8().size()); }

// Room for the body of a channel message sent under `senderName`. Clamped at
// zero rather than going negative, so a caller can compare a size against it
// without a second bounds check.
inline int maxMessageBytes(const QString& senderName) {
    return qMax(0, MaxChannelTextBytes - SenderSeparatorBytes - utf8Bytes(senderName));
}

// The longest leading part of `text` that fits `budget` encoded bytes. The cut
// falls between grapheme clusters, not between bytes and not even between
// QString characters: cutting the UTF-8 would leave bytes that are not a
// character at all, and cutting between code units would leave half of a
// surrogate pair or an accent without the letter it belongs to.
inline QString clampToUtf8Bytes(const QString& text, int budget) {
    if (budget <= 0) return {};
    if (utf8Bytes(text) <= budget) return text;

    // No character encodes to less than a byte per code unit, so this many code
    // units is already an upper bound -- and it keeps the walk below off the
    // whole of a long paste. Where it splits a character the walk repairs it.
    const QString head = text.left(budget);
    QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, head);
    int end = head.size();
    while (end > 0 && utf8Bytes(head.left(end)) > budget) {
        boundary.setPosition(end);
        end = boundary.toPreviousBoundary();
        if (end < 0) return {};
    }
    return head.left(end);
}

}  // namespace proto
