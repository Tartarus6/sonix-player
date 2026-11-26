#ifndef PIXELPLAYER_H
#define PIXELPLAYER_H

#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// The "Pixel" arrangement of the now-playing page: a rounded sleeve, a title
// with a lyrics button, a wavy progress line, three big transport keys and a
// pill of shuffle, repeat and favourite. All of it takes its colours from the
// sleeve, the way a Material You player does: one hue from the cover, and the
// background, the ink, the keys and the accent are worked out from it.
//
// It is a layer on top of the player sheet rather than a rearrangement of the
// ordinary widgets: while it is up it reads what is playing from the device
// state, and draws its own copy of the sleeve, so the rest of the player
// carries on underneath without knowing.

// Builds the layer inside `sheet` (the player sheet) at its full size, hidden.
void pixelplayer_init(lv_obj_t *sheet, gui_config_t *cfg);

// Shows or hides the layer. Hidden, it holds no pictures and its timer rests.
void pixelplayer_set_active(bool on);

bool pixelplayer_is_active(void);

#endif /* PIXELPLAYER_H */
