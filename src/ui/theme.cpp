#include "ui/theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
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

QFont secondaryFont(const QFont& base) {
    constexpr qreal Scale = 0.85;
    QFont font = base;
    if (base.pointSizeF() > 0.0)
        font.setPointSizeF(qMax(6.5, base.pointSizeF() * Scale));
    else if (base.pixelSize() > 0)
        font.setPixelSize(qMax(9, qRound(base.pixelSize() * Scale)));
    return font;
}

int scaled(const QFont& font, int pixels) {
    constexpr int BaselineFontHeight = 16;
    const int fontHeight = QFontMetrics(font).height();
    return qMax(pixels, qRound(pixels * qreal(fontHeight) / BaselineFontHeight));
}

void apply(QApplication& app) {
    app.setStyle(QStringLiteral("Fusion"));
    // Fusion supplies the widget chrome, but its defaults must not replace the
    // font chosen in the desktop theme (especially on the uConsole).
    app.setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));

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
        /* Closes the sidebar column the way the header opens it. There is no
           status bar, so this is where the link and the node it reaches live. */
        QWidget#nodePane { border-top: 1px solid %5; }
        /* A passing line above the message box, shown only while it has
           something to say; lined up with the input row it interrupts. */
        QLabel#notice { padding: 3px 10px; }
        /* Icon-only, and quiet until it is pointed at: the icon carries the
           colour, so the button contributes no chrome of its own. */
        QToolButton#iconButton {
            background: transparent; border: none; border-radius: 3px; padding: 2px;
        }
        QToolButton#iconButton:hover { background: %4; }
        QRadioButton { spacing: 6px; }
        /* A popup is a QWidget too, so without this it would take the flat
           window background above and stand off the page by nothing at all.
           Only the colours are set: the item metrics stay the style's, which is
           what keeps the icon column right under a larger desktop font. */
        QMenu { background: %4; border: 1px solid %5; padding: 4px; }
        /* The accent, as in the list above, rather than the sidebar's selected
           grey: a menu highlight follows the pointer and is gone again, so it
           has to be obvious at a glance and cannot rely on being compared with
           the row beside it. */
        QMenu::item:selected { background: %6; color: #11221e; }
        QMenu::item:disabled { color: %7; }
    )")
                          .arg(Background.name(), Text.name(), Sidebar.name(), Surface.name(),
                               Border.name(), Accent.name(), TextMuted.name()));
}

}  // namespace theme
