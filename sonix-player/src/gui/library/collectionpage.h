#ifndef COLLECTIONPAGE_H
#define COLLECTIONPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// One collection's albums: a windowed list of the rows the collection holds,
// with covers, and a play button that queues every track the albums still
// have. Reached from the collections list page and the Music tile; see
// collections.h for what the system layer answers.
extern lv_obj_t *collectionpage_screen;

void collectionpage_init(gui_config_t *cfg);

// Opens the page on a collection. `name` is shown as the heading exactly as it
// is stored -- it is data, not a translation tag.
void collectionpage_open(int collection_id, const char *name);

#endif /* COLLECTIONPAGE_H */
