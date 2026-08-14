#pragma once

#include <QBoxLayout>
#include <QDialog>

namespace ui {

// Keep short-lived app dialogs modal and content-sized. The title-bar close
// control remains available; QDialog handles it through reject(), just like
// the Cancel and Close buttons used by these dialogs.
inline void configureDialogWindow(QDialog& dialog) {
    dialog.setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                          Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    dialog.setModal(true);
    dialog.setSizeGripEnabled(false);
}

// QLayout::SetFixedSize updates both size bounds from the content. A strut
// preserves each dialog's intended width without making it user-resizable.
inline void lockDialogSize(QDialog& dialog, QBoxLayout& layout, int minimumWidth) {
    const QMargins margins = layout.contentsMargins();
    layout.addStrut(qMax(0, minimumWidth - margins.left() - margins.right()));
    layout.setSizeConstraint(QLayout::SetFixedSize);
    dialog.adjustSize();
}

}  // namespace ui
