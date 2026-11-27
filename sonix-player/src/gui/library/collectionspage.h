#ifndef COLLECTIONSPAGE_H
#define COLLECTIONSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Collections page: one row per named collection, plus a "+ New
// collection" row, in one of two modes. Opened from the Music page it is a
// browser -- tapping a collection opens its albums. Opened from the Albums
// list's selection or the Now Playing menu it is a picker: the same list, with
// the tapped collection taking the pending albums, and a "New collection" row
// that asks for a name first. See collections.h for what the system layer
// answers.
extern lv_obj_t *collectionspage_screen;

void collectionspage_init(gui_config_t *cfg);

// Browse: the collections themselves.
void collectionspage_open(void);

// Pick: choose (or create) the collection `album_value` is added to. The page
// keeps its own copy of the value.
void collectionspage_add_album(const char *album_value);

// Pick for a set of albums at once: the chosen collection gets all of them, in
// this order, written in the background. The page keeps its own copy of the
// values.
void collectionspage_add_albums(const char *const *album_values, int count);

#endif /* COLLECTIONSPAGE_H */
