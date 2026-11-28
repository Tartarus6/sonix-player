#ifndef SOUNDCLOUDPAGE_H
#define SOUNDCLOUDPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The SoundCloud pages: the main one, plus sign-in, search, results and account
// settings. Built once at startup; every page is reused afterwards.
void soundcloudpage_init(gui_config_t *cfg);

// The main page, for the streaming grid. Global rather than behind a function:
// the grid keeps the address of the variable (grid_entry_t.target).
extern lv_obj_t *soundcloud_screen;

#endif /* SOUNDCLOUDPAGE_H */
