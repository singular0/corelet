#include "ui/menu_button.h"

#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QMenu>
#include <QPoint>
#include <QToolButton>

namespace ui {

QAction* popupUnder(QMenu& menu, QToolButton* button) {
    QAction* chosen = menu.exec(button->mapToGlobal(QPoint(0, button->height())));

    if (!button->rect().contains(button->mapFromGlobal(QCursor::pos()))) {
        button->setAttribute(Qt::WA_UnderMouse, false);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(button, &leave);
    }
    return chosen;
}

}  // namespace ui
