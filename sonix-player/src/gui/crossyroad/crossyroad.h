#ifndef GUI_CROSSYROAD_H
#define GUI_CROSSYROAD_H

#include "src/gui/shell/gui.h"
#include "lvgl/lvgl.h"

// Crossy Road, from the More page. A port of the SonixOS web prototype: hop
// forwards with a tap, sideways and back with a swipe, over grass, roads and
// rivers that never end.
//
// Everything is drawn in code, so there are no files to load. The best score
// and the chosen character are kept in the config, [crossyroad] best and
// [crossyroad] character, and whether the game plays its own sounds -- stopping
// the music -- in [crossyroad] sound.

extern lv_obj_t *crossyroad_screen;

void crossyroad_init(gui_config_t *cfg);

// Opens the game on its start screen.
void crossyroad_open(void);

#endif /* GUI_CROSSYROAD_H */
