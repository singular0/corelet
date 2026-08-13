#include "ui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QString>

namespace theme {

QColor senderColor(const QString& name) {
    static const QColor palette[] = {
        QColor(0x6c, 0xb6, 0xff), QColor(0x7d, 0xd8, 0x8f), QColor(0xe8, 0xa0, 0x6b),
        QColor(0xc8, 0x92, 0xea), QColor(0x5f, 0xd4, 0xd0), QColor(0xe8, 0x8f, 0xa8),
        QColor(0xd6, 0xc7, 0x6a), QColor(0x8f, 0xa8, 0xf0),
    };
    if (name.isEmpty()) return TextMuted;
    // qHash is seeded per process by default, which would repaint every name a
    // different colour on each launch; hash the bytes directly instead.
    uint h = 2166136261u;
    for (const QChar c : name) h = (h ^ c.unicode()) * 16777619u;
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

void apply(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, Background);
    p.setColor(QPalette::WindowText, Text);
    p.setColor(QPalette::Base, Background);
    p.setColor(QPalette::AlternateBase, Sidebar);
    p.setColor(QPalette::Text, Text);
    p.setColor(QPalette::Button, Surface);
    p.setColor(QPalette::ButtonText, Text);
    p.setColor(QPalette::Highlight, SidebarSelected);
    p.setColor(QPalette::HighlightedText, Text);
    p.setColor(QPalette::ToolTipBase, Surface);
    p.setColor(QPalette::ToolTipText, Text);
    p.setColor(QPalette::PlaceholderText, TextMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, TextMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, TextMuted);
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: %1; color: %2; }
        QListView { border: none; background: %1; }
        QListView#channelList { background: %3; }
        QLineEdit {
            background: %4; border: 1px solid %5; border-radius: 4px;
            padding: 5px 8px; color: %2; selection-background-color: %6;
        }
        QLineEdit:focus { border-color: %6; }
        QPushButton {
            background: %6; color: #11221e; border: none; border-radius: 4px;
            padding: 5px 14px; font-weight: bold;
        }
        QPushButton:hover { background: #5fe0c0; }
        QPushButton:disabled { background: %5; color: %7; }
        QStatusBar { background: %3; color: %7; border-top: 1px solid %5; }
        QStatusBar::item { border: none; }
        /* A link, not a button: it sits in the status bar and must not shout. */
        QPushButton#statusLink {
            background: transparent; color: %7; font-weight: normal;
            padding: 1px 6px; border-radius: 3px;
        }
        QPushButton#statusLink:hover { background: %4; color: %2; }
        QTabWidget::pane { border: 1px solid %5; border-radius: 3px; }
        QTabBar::tab {
            background: transparent; color: %7; padding: 5px 12px;
            border-bottom: 2px solid transparent;
        }
        QTabBar::tab:selected { color: %2; border-bottom-color: %6; }
        QListWidget { background: %4; border: 1px solid %5; border-radius: 3px; }
        QListWidget::item { padding: 3px 6px; }
        QListWidget::item:selected { background: %6; color: #11221e; }
        QSplitter::handle { background: %5; width: 1px; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
        QScrollBar::handle:vertical { background: %5; border-radius: 4px; min-height: 24px; }
        QScrollBar::handle:vertical:hover { background: %7; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QLabel#header { padding: 6px 10px; border-bottom: 1px solid %5; }
        /* Same strip as QLabel#header, but a container: the sidebar's title
           shares it with the add-channel button. */
        QWidget#sidebarHeader { border-bottom: 1px solid %5; }
        /* Icon-only, and quiet until it is pointed at: the icon carries the
           colour, so the button contributes no chrome of its own. */
        QToolButton#iconButton {
            background: transparent; border: none; border-radius: 3px; padding: 2px;
        }
        QToolButton#iconButton:hover { background: %4; }
        QRadioButton { spacing: 6px; }
    )")
                          .arg(Background.name(), Text.name(), Sidebar.name(), Surface.name(),
                               Border.name(), Accent.name(), TextMuted.name()));
}

}  // namespace theme
