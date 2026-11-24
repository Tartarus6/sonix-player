#ifndef PODMENU_H
#define PODMENU_H

// ---------------------------------------------------------------------------
// podmenu — Layout Editor for the Pod home screen
//
// Opened from Settings > Appearance when the Pod theme is active. Lets the
// user reorder shortcuts across the 9 grid slots and 3 dock slots, and toggle
// per-shortcut visibility.
//
// Two sections on one scrollable canvas (following the musictiles.c pattern):
//
//   "Placed"   — the visible shortcuts in their current slot order.  Each row
//                has a drag grip: grabbing it and dropping it onto another row
//                swaps the two shortcuts' slots.
//
//   "Hidden"   — shortcuts that have been hidden.  Each row has only the
//                icon, name, and visibility toggle; no grip is shown because
//                hidden shortcuts have no slot to swap.
//
// Every row in both sections carries a Visibility_Toggle (lv_switch).  Turning
// it off moves the shortcut to the Hidden section; turning it back on restores
// it to the Placed section (at its remembered or first-free slot).
//
// After every change the module calls pod_layout_save() and pod_home_refresh()
// so the home screen updates immediately.
// ---------------------------------------------------------------------------

#include "lvgl/lvgl.h"

#include "src/gui/shell/gui.h"

extern lv_obj_t *podmenu_screen;

// Creates podmenu_screen and builds all its widgets.
// Call once inside gui_init, after pod_home_create().
void podmenu_init(gui_config_t *cfg);

#endif /* PODMENU_H */
