#ifndef CHAPTERS_H
#define CHAPTERS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Chapter navigation for the playing audiobook or podcast episode.
extern lv_obj_t *chapters_screen;

void chapters_init(gui_config_t *cfg);

// Opens it from the player: closes the sheet, so backing out of the list
// comes back to the player rather than to whatever page was behind it.
void chapters_open(void);

// Checks for a complete MP3 chapter tag when opening the podcast menu.
bool chapters_podcast_available(const char *path);
void chapters_open_podcast(void);

#endif /* CHAPTERS_H */
