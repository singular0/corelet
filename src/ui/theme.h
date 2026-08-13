#pragma once

#include <QColor>
#include <QFont>

class QApplication;

// A dark theme sized for the uConsole's 1280x480 panel: the screen is wide but
// very short, so vertical space is the scarce resource and every padding here
// is deliberately tighter than a desktop default.
namespace theme {

inline const QColor Background {0x12, 0x15, 0x1a};
inline const QColor Sidebar {0x17, 0x1b, 0x22};
inline const QColor SidebarSelected {0x25, 0x2d, 0x3a};
inline const QColor Surface {0x23, 0x2a, 0x35};   // incoming bubble
inline const QColor Outgoing {0x1f, 0x4d, 0x45};  // our own bubble
inline const QColor Border {0x2a, 0x31, 0x3d};
// Disc behind a channel's type icon. Lifted clear of both the sidebar and its
// selected row, so the icon keeps its circle in either state.
inline const QColor IconBackground {0x33, 0x3c, 0x4a};
inline const QColor Text {0xe6, 0xe9, 0xef};
inline const QColor TextMuted {0x8a, 0x93, 0xa3};
inline const QColor Accent {0x4d, 0xd0, 0xb0};
inline const QColor Warning {0xe0, 0xa0, 0x50};
inline const QColor Error {0xe0, 0x6c, 0x6c};

// Sender names are coloured from a fixed palette hashed on the name, which is
// the cheapest way to tell speakers apart in a channel where nothing else
// identifies them.
QColor senderColor(const QString& name);

void apply(QApplication& app);

}  // namespace theme
