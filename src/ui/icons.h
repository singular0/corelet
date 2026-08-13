#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>

// Lucide (https://lucide.dev), ISC licensed — see src/ui/icons/LICENSE. The
// SVGs are stroke drawings on a 24x24 grid with `stroke="currentColor"`, which
// nothing resolves for us: `tinted()` substitutes the colour before rendering
// rather than compositing over a black raster, so a 16px glyph stays crisp.
namespace icons {

// `name` is a file in the resource bundle without its extension, e.g. "globe".
// Rasterised at `size` logical pixels for `dpr`, and cached: a list delegate
// asks for the same handful of pixmaps on every paint.
QPixmap tinted(const QString& name, int size, const QColor& color, qreal dpr);

}  // namespace icons
