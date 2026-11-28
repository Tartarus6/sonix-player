#ifndef PODCASTNOTES_H
#define PODCASTNOTES_H

#include "src/gui/shell/gui.h"

void podcastnotes_init(gui_config_t *cfg);
void podcastnotes_open(long long feed_id, long long episode_id, bool from_player);
void podcastnotes_open_path(const char *path, bool from_player);

#endif
