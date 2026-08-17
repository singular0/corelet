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

    return 0;
}
