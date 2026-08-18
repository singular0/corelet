#pragma once

#include <QFont>
#include <QHash>
#include <QRect>
#include <QString>

class QPainter;

// The round monogram a name is drawn as when there is no picture to show: the
// sender disc beside a message, and a person's row in the address book. Shared
// so the two lists agree on what a given name looks like, which is the point --
// the disc is how a reader recognises somebody without reading the name again.
//
// One per delegate rather than a free function: picking the glyph walks the name
// by grapheme cluster and asks the font whether it can draw the result, so it is
// memoised, and the answer depends on the font doing the drawing.
class Avatar {
public:
    // `base` is the delegate's body font; the disc draws in a bold version of it.
    explicit Avatar(const QFont& base);

    void paint(QPainter* painter, const QRect& rect, const QString& name) const;

private:
    // The name's own emoji if it has one the font can draw, otherwise its first
    // letter.
    QString glyph(const QString& name) const;

    QFont font_;
    mutable QHash<QString, QString> glyphs_;
};
