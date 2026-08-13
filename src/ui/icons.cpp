#include "ui/icons.h"

#include <QFile>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace icons {

QPixmap tinted(const QString& name, int size, const QColor& color, qreal dpr) {
    const QString key =
        QStringLiteral("umc:%1:%2:%3:%4").arg(name).arg(size).arg(color.name()).arg(dpr);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) return cached;

    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name().toLatin1());

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, size, size));
    p.end();

    QPixmapCache::insert(key, pixmap);
    return pixmap;
}

}  // namespace icons
