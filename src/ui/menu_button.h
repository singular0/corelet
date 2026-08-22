#pragma once

class QAction;
class QMenu;
class QToolButton;

namespace ui {

// Drops `menu` open under `button` and returns the action chosen, or nullptr.
//
// This exists for the second half rather than the placement: a popup holds the
// mouse grab for as long as it is up, so the button that opened it never sees
// the pointer leave. Dismissed with a click somewhere else, an auto-raised
// button stays lit and its icon stays in Active mode as though the cursor were
// still on it, and no second enter event is coming to correct that. The hover
// is only released when the pointer really has moved off -- clearing the flag
// under a cursor that is still there would unlight the button for good.
QAction* popupUnder(QMenu& menu, QToolButton* button);

}  // namespace ui
