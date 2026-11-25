#ifndef POD_HOME_H
#define POD_HOME_H

// ---------------------------------------------------------------------------
// pod_home — the Pod Theme home screen
//
// Shows the 3×3 icon grid and the three-slot dock. Pure display: all layout
// data comes from pod_layout.
//
// The Pod home is not a separate LVGL screen. It is a full-screen, opaque
// layer inside the main menu screen that sits over the Classic tiles. The
// switcher keeps treating main_menu_screen as "home" for back navigation and
// history; the active home theme only decides whether this layer is shown.
//
// Lifecycle:
//   1. pod_home_create() — build the layer (call once, inside gui_init,
//      after pod_layout_init and main_menu_init).
//   2. pod_home_refresh() — repaint every slot from the current pod_layout
//      state; call after any layout change that must appear immediately.
//   3. pod_home_apply_theme() — show or hide the layer after the home theme
//      changes.
// ---------------------------------------------------------------------------

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// The Pod home layer. NULL until pod_home_create() runs.
extern lv_obj_t *pod_home_screen;

// Builds the layer inside `parent` (the main menu screen), then shows or
// hides it per home_theme_get(). Must be called after pod_layout_init() and
// theme_init().
void pod_home_create(lv_obj_t *parent, gui_config_t *cfg);

// Clears and rebuilds every slot widget from the current pod_layout state.
// Safe to call at any time, including before pod_home_create().
void pod_home_refresh(void);

// Shows the layer when the Pod theme is active, hides it for Classic.
void pod_home_apply_theme(void);

// The classic arrival: the icons fly in to their places, from every side, as
// the screensaver leaves. Only when the Pod home is the screen showing; anywhere
// else it does nothing.
void pod_home_play_enter(void);

#endif /* POD_HOME_H */
