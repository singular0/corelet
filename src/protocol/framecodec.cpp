#include "protocol/framecodec.h"

namespace proto {

void FrameReader::feed(const QByteArray& data) { buf_.append(data); }

std::optional<QByteArray> FrameReader::next() {
    for (;;) {
        // Resync: drop anything before a start marker.
        int start = 0;
        while (start < buf_.size() && quint8(buf_[start]) != FrameToDevice &&
               quint8(buf_[start]) != FrameToApp)
            start++;
        if (start > 0) buf_.remove(0, start);

        if (buf_.size() < 3) return std::nullopt;  // need marker + length

        const int len = quint8(buf_[1]) | (quint8(buf_[2]) << 8);
        if (len > MaxFrameSize) {
            // The length is nonsense, so this is not really a frame start.
            // Drop the marker and look for the next one.
            buf_.remove(0, 1);
            continue;
        }

        if (buf_.size() < 3 + len) return std::nullopt;  // still arriving

        QByteArray frame = buf_.mid(3, len);
        buf_.remove(0, 3 + len);
        return frame;
    }
}

QByteArray frameCommand(const QByteArray& payload) {
    QByteArray out;
    out.reserve(3 + payload.size());
    out.append(char(FrameToDevice));
    out.append(char(payload.size() & 0xFF));
    out.append(char((payload.size() >> 8) & 0xFF));
    out.append(payload);
    return out;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

Writer& Writer::u8(quint8 v) {
    buf_.append(char(v));
    return *this;
}

Writer& Writer::u16(quint16 v) {
    buf_.append(char(v & 0xFF));
    buf_.append(char((v >> 8) & 0xFF));
    return *this;
}

Writer& Writer::u32(quint32 v) {
    buf_.append(char(v & 0xFF));
    buf_.append(char((v >> 8) & 0xFF));
    buf_.append(char((v >> 16) & 0xFF));
    buf_.append(char((v >> 24) & 0xFF));
    return *this;
}

Writer& Writer::padded(const QByteArray& data, int width) {
    const int n = qMin(data.size(), qsizetype(width));
    buf_.append(data.constData(), n);
    buf_.append(width - n, '\0');
    return *this;
}

Writer& Writer::tail(const QByteArray& data) {
    buf_.append(data);
    return *this;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

QByteArray Reader::take(int n) {
    if (n < 0 || remaining() < n) {
        ok_ = false;
        return {};
    }
    QByteArray out = d_.mid(off_, n);
    off_ += n;
    return out;
}

quint8 Reader::u8() {
    QByteArray b = take(1);
    return b.isEmpty() ? 0 : quint8(b[0]);
}

quint16 Reader::u16() {
    QByteArray b = take(2);
    if (b.size() < 2) return 0;
    return quint16(quint8(b[0]) | (quint8(b[1]) << 8));
}

quint32 Reader::u32() {
    QByteArray b = take(4);
    if (b.size() < 4) return 0;
    return quint32(quint8(b[0])) | (quint32(quint8(b[1])) << 8) |
           (quint32(quint8(b[2])) << 16) | (quint32(quint8(b[3])) << 24);
}

qint32 Reader::i32() { return static_cast<qint32>(u32()); }

QString Reader::fixedString(int width) {
    QByteArray b = take(width);
    const int nul = b.indexOf('\0');
    if (nul >= 0) b.truncate(nul);
    // Names are bytes on the wire and are not guaranteed to be valid UTF-8,
    // so decode leniently rather than dropping the whole field.
    return QString::fromUtf8(b);
}

QByteArray Reader::rest() { return take(remaining()); }

QString errorText(quint8 code) {
    switch (code) {
        case ErrUnsupportedCmd: return QStringLiteral("command not supported");
        case ErrNotFound: return QStringLiteral("not found");
        case ErrTableFull: return QStringLiteral("table full");
        case ErrBadState: return QStringLiteral("bad state");
        case ErrFileIoError: return QStringLiteral("storage error");
        case ErrIllegalArg: return QStringLiteral("invalid argument");
        default: return QStringLiteral("error %1").arg(code);
    }
}

}  // namespace proto
