#include <QString>

#include "protocol/text_limits.h"

namespace {

bool check(bool condition, const char* expression) {
    if (!condition) qCritical("check failed: %s", expression);
    return condition;
}

// What one channel message costs on the air, worked out independently of the
// constants under test: the node builds "Name: body", puts a 5-byte text header
// in front, encrypts with AES-ECB (which pads out to whole 16-byte blocks) and
// wraps the result in a channel hash and a 2-byte MAC. The result has to fit the
// 184-byte mesh payload, and that is the only thing the budget exists to
// guarantee -- so the test asserts against the packet rather than against 171.
constexpr int MeshPayload = 184;

int sealedSize(const QString& sender, const QString& body) {
    const int plaintext = 5 + int(sender.toUtf8().size()) + 2 + int(body.toUtf8().size());
    const int blocks = (plaintext + 15) / 16 * 16;
    return 1 + 2 + blocks;
}

// `n` bytes of `unit` repeated, for a body that lands exactly on a limit.
QString repeatToBytes(const QString& unit, int bytes) {
    const int per = int(unit.toUtf8().size());
    QString out;
    for (int i = 0; i + per <= bytes; i += per) out += unit;
    return out;
}

}  // namespace

int main() {
    // --- encoded size, not character count ---------------------------------
    if (!check(proto::utf8Bytes(QStringLiteral("hello")) == 5, "ASCII is one byte a character") ||
        !check(proto::utf8Bytes(QString::fromUtf8("привет")) == 12, "Cyrillic is two") ||
        !check(proto::utf8Bytes(QString::fromUtf8("日本語")) == 9, "CJK is three") ||
        !check(proto::utf8Bytes(QString::fromUtf8("👋")) == 4, "an emoji is four") ||
        // The bug this whole file is about: one QString character is not one
        // byte, so a character count over-promises by a factor of four.
        !check(QString::fromUtf8("日本語").size() == 3, "three characters") ||
        !check(QString::fromUtf8("👋").size() == 2, "one emoji, two QChars"))
        return 1;

    // --- the message budget shrinks by the sender's name --------------------
    if (!check(proto::maxMessageBytes(QString()) == 169, "no name leaves the separator only") ||
        !check(proto::maxMessageBytes(QStringLiteral("node")) == 165, "an ASCII name costs 4") ||
        !check(proto::maxMessageBytes(QString::fromUtf8("узел")) == 161,
               "a Cyrillic name of the same length costs 8") ||
        !check(proto::maxMessageBytes(QString(200, QLatin1Char('x'))) == 0,
               "an implausible name floors at zero rather than going negative"))
        return 1;

    // --- a body at exactly the budget fits one payload, and one byte more
    //     does not, for every width of character ----------------------------
    const QString sender = QString::fromUtf8("узел-1");
    const int budget = proto::maxMessageBytes(sender);
    if (!check(sealedSize(sender, repeatToBytes(QStringLiteral("x"), budget)) <= MeshPayload,
               "a body filling the budget exactly fits one mesh payload") ||
        !check(sealedSize(sender, repeatToBytes(QStringLiteral("x"), budget + 1)) > MeshPayload,
               "and one byte past it does not"))
        return 1;

    for (const QString& unit : {QStringLiteral("x"), QString::fromUtf8("ж"),
                                QString::fromUtf8("語"), QString::fromUtf8("👋")}) {
        const int per = proto::utf8Bytes(unit);
        // The last whole character that still fits, then one more of the same.
        const QString atLimit = repeatToBytes(unit, budget - budget % per);
        if (!check(proto::utf8Bytes(atLimit) <= budget, "a body built to the budget is within it") ||
            !check(proto::utf8Bytes(atLimit + unit) > budget, "one character more is over") ||
            !check(sealedSize(sender, atLimit) <= MeshPayload, "and what is within it goes out"))
            return 1;
    }

    // --- channel names are bounded by the field, in bytes -------------------
    const QString name32 = QString(32, QLatin1Char('a'));
    const QString name16Cyrillic = repeatToBytes(QString::fromUtf8("ж"), 32);
    const QString name8Emoji = repeatToBytes(QString::fromUtf8("👋"), 32);
    if (!check(proto::MaxChannelNameBytes == 32, "the name field is 32 bytes") ||
        !check(proto::utf8Bytes(name32) == 32, "32 ASCII characters fill it exactly") ||
        !check(proto::utf8Bytes(name16Cyrillic) == 32, "so do 16 Cyrillic ones") ||
        !check(name16Cyrillic.size() == 16, "which is half the characters") ||
        !check(proto::utf8Bytes(name8Emoji) == 32, "and 8 emoji") ||
        !check(proto::utf8Bytes(name32 + QLatin1Char('a')) > proto::MaxChannelNameBytes,
               "one character past the field is over") ||
        !check(proto::utf8Bytes(name8Emoji + QString::fromUtf8("👋")) == 36,
               "an emoji past the field is four bytes over"))
        return 1;

    // --- clamping keeps what fits and cuts between characters ---------------
    const auto clamp = proto::clampToUtf8Bytes;
    if (!check(clamp(QStringLiteral("hello"), 5) == QStringLiteral("hello"),
               "text at the budget is left alone") ||
        !check(clamp(QStringLiteral("hello"), 99) == QStringLiteral("hello"),
               "so is text well inside it") ||
        !check(clamp(QStringLiteral("hello"), 3) == QStringLiteral("hel"), "ASCII cuts per byte") ||
        !check(clamp(QStringLiteral("hello"), 0).isEmpty(), "no budget keeps nothing") ||
        !check(clamp(QStringLiteral("hello"), -4).isEmpty(), "nor does a negative one"))
        return 1;

    // A cut inside a character would leave bytes that decode to nothing, so the
    // last character that does not fit whole comes out entirely.
    if (!check(clamp(QString::fromUtf8("привет"), 5) == QString::fromUtf8("пр"),
               "an odd budget cannot hold half a two-byte letter") ||
        !check(clamp(QString::fromUtf8("日本語"), 8) == QString::fromUtf8("日本"),
               "nor two thirds of a three-byte one") ||
        !check(clamp(QString::fromUtf8("日本語"), 9) == QString::fromUtf8("日本語"),
               "and an exact fit keeps all three"))
        return 1;

    // The emoji case is the one that would corrupt rather than truncate: half a
    // surrogate pair is not a character at all.
    const QString twoEmoji = QString::fromUtf8("👋👋");
    if (!check(clamp(twoEmoji, 7) == QString::fromUtf8("👋"), "seven bytes hold one emoji") ||
        !check(clamp(twoEmoji, 7).size() == 2, "kept whole, as both of its code units") ||
        !check(clamp(twoEmoji, 3).isEmpty(), "and three bytes hold none of one") ||
        !check(clamp(twoEmoji, 8) == twoEmoji, "eight hold both"))
        return 1;

    // Stepping back by grapheme rather than by code unit, so an accent is never
    // separated from the letter it belongs to.
    // Spelled out rather than written literally: this has to be "e" followed by
    // a combining acute, not the single precomposed character that looks the
    // same, which an editor or a filesystem will quietly swap one for.
    const QString accented = QStringLiteral("ae") + QChar(0x0301);
    if (!check(proto::utf8Bytes(accented) == 4, "a combining accent costs two bytes") ||
        !check(clamp(accented, 4) == accented, "the pair fits at four") ||
        !check(clamp(accented, 3) == QStringLiteral("a"),
               "and at three the letter goes with its accent"))
        return 1;

    // What the input fields rely on: whatever comes back fits, every time, so
    // there is never a count left to show as a negative number.
    for (const int limit : {0, 1, 2, 3, 7, 16, 31, 169}) {
        for (const QString& text : {QStringLiteral("plain ASCII text"),
                                    QString::fromUtf8("привет, мир"),
                                    QString::fromUtf8("👋👋👋"), accented, QString()}) {
            if (!check(proto::utf8Bytes(clamp(text, limit)) <= limit,
                       "a clamped string is within its budget") ||
                !check(clamp(text, limit) == clamp(clamp(text, limit), limit),
                       "and clamping it again changes nothing"))
                return 1;
        }
    }

    return 0;
}
