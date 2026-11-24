#ifndef PIXELLIBRARY_H
#define PIXELLIBRARY_H

#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// The "Pixel" look of the Music page: a big "Library" heading, a row of tabs
// along the top, and the chosen tab's contents underneath -- the albums as a
// grid of cards tinted with their sleeve's colour, and the other lists as rows
// with a picture.
//
// The tabs are the Music page's own tiles, in the order and with the choice
// made in Music settings > Library > Rearrange main page, so moving or hiding
// one there moves or hides the tab. Everything a row does is what the ordinary
// lists do: a track plays with its list as the queue, anything else opens the
// same list page as before.

// Builds the layer inside `screen` (the Music page), hidden.
void pixellibrary_create(lv_obj_t *screen, gui_config_t *cfg);

// Shows or hides the layer. Hidden, it holds no pictures and no list.
void pixellibrary_show(bool on);

// The tabs again, from the tiles' saved order.
void pixellibrary_refresh_tabs(void);

#endif /* PIXELLIBRARY_H */
