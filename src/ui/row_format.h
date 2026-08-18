#pragma once

#include <QDateTime>
#include <QLocale>
#include <QString>

// Wording the lists share. The channel sidebar, a message's header line and the
// address book all say when something was last heard and how it reached us, and
// they have to say it the same way or the three read as three applications.
namespace ui {

// A list is scanned rather than read, so anything from the last day is the clock
// time alone; older than that and the date has to come along or the time means
// nothing. An invalid time is no time: a node that has never had its clock set
// advertises from 1970, which is worth saying nothing about rather than dating.
inline QString activityStamp(const QDateTime& when) {
    if (!when.isValid()) return {};
    const QString time = QLocale().toString(when.time(), QLocale::ShortFormat);
    // Timestamps come off the mesh and can sit slightly in the future when a
    // sender's clock runs fast; that still counts as just now.
    if (when.secsTo(QDateTime::currentDateTime()) < 24 * 60 * 60) return time;
    return QStringLiteral("%1 %2").arg(
        QLocale().toString(when.date(), QStringLiteral("d MMM")), time);
}

// 0xFF on the wire means no route was known and the packet flooded the mesh;
// zero means it came straight from the node that sent it.
inline QString hopText(int pathLen) {
    if (pathLen == 0xFF) return QStringLiteral("flood");
    if (pathLen == 0) return QStringLiteral("direct");
    return QStringLiteral("%1 hop%2").arg(pathLen).arg(pathLen == 1 ? QString()
                                                                    : QStringLiteral("s"));
}

}  // namespace ui
