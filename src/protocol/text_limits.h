#pragma once

#include <QString>

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

inline int utf8Bytes(const QString& text) { return int(text.toUtf8().size()); }

// Room for the body of a channel message sent under `senderName`. Clamped at
// zero rather than going negative, so a caller can compare a size against it
// without a second bounds check.
inline int maxMessageBytes(const QString& senderName) {
    return qMax(0, MaxChannelTextBytes - SenderSeparatorBytes - utf8Bytes(senderName));
}

}  // namespace proto
