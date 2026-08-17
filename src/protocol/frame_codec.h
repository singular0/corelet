#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

#include "protocol/protocol.h"

namespace proto {

// Incremental de-framer for a byte-stream link. Resynchronises by discarding
// bytes until a start marker appears: a daemon restart or a half-open socket
// can leave us mid-frame, and stalling forever is worse than dropping junk.
class FrameReader {
public:
    void feed(const QByteArray& data);
    // Next complete frame payload, or nullopt when more bytes are needed. Call
    // repeatedly until it returns nullopt.
    std::optional<QByteArray> next();
    void reset() { buf_.clear(); }

private:
    QByteArray buf_;
};

// Wraps a command payload in the length-prefixed app-to-device frame.
QByteArray frameCommand(const QByteArray& payload);

// Little-endian writer for command payloads.
class Writer {
public:
    explicit Writer(quint8 cmd) { buf_.append(char(cmd)); }

    Writer& u8(quint8 v);
    Writer& u16(quint16 v);
    Writer& u32(quint32 v);
    // Fixed-width NUL-padded field. Oversized data is a caller's bug and the
    // last-resort behaviour is to truncate rather than overflow the field --
    // but truncation here cuts bytes, so on user text it can land inside a
    // multi-byte character. Validate against the limits in text_limits.h before
    // calling; the assert is what catches a call site that forgets.
    Writer& padded(const QByteArray& data, int width);
    // Trailing variable-length text: no length prefix, no terminator.
    Writer& tail(const QByteArray& data);

    const QByteArray& bytes() const { return buf_; }

private:
    QByteArray buf_;
};

// Bounded little-endian reader. Every accessor is safe past the end: it sets
// the error flag and returns zero, so a truncated or hostile frame produces a
// zeroed struct and one `ok()` check rather than a crash.
class Reader {
public:
    explicit Reader(const QByteArray& data) : d_(data) {}

    quint8 u8();
    quint16 u16();
    quint32 u32();
    qint32 i32();
    qint8 i8() { return static_cast<qint8>(u8()); }

    QByteArray take(int n);
    // Fixed-width field truncated at the first NUL, as names are encoded.
    QString fixedString(int width);
    QByteArray rest();

    void skip(int n) { take(n); }
    int remaining() const { return d_.size() - off_; }
    bool ok() const { return ok_; }

private:
    QByteArray d_;
    int off_ = 0;
    bool ok_ = true;
};

// Human-readable text for an ERR code, for the status bar.
QString errorText(quint8 code);

}  // namespace proto
