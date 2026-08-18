#include "ui/avatar.h"

#include <QFontMetrics>
#include <QPainter>
#include <QTextBoundaryFinder>

#include "ui/theme.h"

namespace {

// Mesh operators routinely put an emoji in their name, and it identifies them
// far better than a letter does. Everything a font would actually draw as a
// picture, which is wider than the Emoji property: the pictographic blocks, the
// flag pair range, and anything a variation selector asks to be drawn as emoji.
bool isPictographic(char32_t c) {
    return (c >= 0x1F300 && c <= 0x1FAFF) ||  // emoticons, pictographs, transport, symbols
           (c >= 0x1F000 && c <= 0x1F0FF) ||  // tiles and playing cards
           (c >= 0x1F1E6 && c <= 0x1F1FF) ||  // regional indicators, i.e. flags
           (c >= 0x2600 && c <= 0x27BF) ||    // miscellaneous symbols and dingbats
           (c >= 0x2B00 && c <= 0x2BFF) ||    // stars and arrows
           c == 0x3030 || c == 0x303D || c == 0x3297 || c == 0x3299;
}

char32_t firstCodePoint(const QString& cluster) {
    if (cluster.size() > 1 && cluster.at(0).isHighSurrogate())
        return QChar::surrogateToUcs4(cluster.at(0), cluster.at(1));
    return cluster.at(0).unicode();
}

// The ink on the coloured disc. The sender palette is light and saturated, so
// this is near-black in practice; measured rather than hardcoded so a darker
// entry added later does not silently produce an unreadable avatar.
QColor ink(const QColor& background) {
    const qreal luma = 0.299 * background.redF() + 0.587 * background.greenF() +
                       0.114 * background.blueF();
    return luma > 0.55 ? theme::Background : theme::Text;
}

}  // namespace

Avatar::Avatar(const QFont& base) : font_(base) { font_.setBold(true); }

QString Avatar::glyph(const QString& name) const {
    const auto cached = glyphs_.constFind(name);
    if (cached != glyphs_.constEnd()) return *cached;

    // Walk grapheme clusters, not code points, so a ZWJ sequence, a flag or a
    // skin-toned emoji stays in one piece instead of drawing as its first half.
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, name);
    const QFontMetrics fm(font_);
    QString initial;
    QString picture;
    for (int start = 0, end = finder.toNextBoundary(); end > 0;
         start = end, end = finder.toNextBoundary()) {
        const QString cluster = name.mid(start, end - start);
        const char32_t first = firstCodePoint(cluster);
        if (isPictographic(first) || cluster.contains(QChar(0xFE0F))) {
            // A box of tofu says less than a letter does, and an emoji font is
            // not a given on a bare Debian install.
            if (fm.inFontUcs4(first)) {
                picture = cluster;
                break;
            }
        } else if (initial.isEmpty() && cluster.at(0).isLetterOrNumber()) {
            initial = cluster.at(0).toUpper();
        }
    }
    if (picture.isEmpty()) picture = initial.isEmpty() ? QStringLiteral("?") : initial;

    glyphs_.insert(name, picture);
    return picture;
}

void Avatar::paint(QPainter* painter, const QRect& rect, const QString& name) const {
    // The colour a name is written in elsewhere, so the disc and the text agree
    // on who this is.
    const QColor tint = theme::senderColor(name);
    painter->setPen(Qt::NoPen);
    painter->setBrush(tint);
    painter->drawEllipse(rect);
    painter->setFont(font_);
    painter->setPen(ink(tint));
    painter->drawText(rect, Qt::AlignCenter, glyph(name));
}
