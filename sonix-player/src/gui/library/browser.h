#ifndef BROWSER_H
#define BROWSER_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

#include <stdbool.h>

extern lv_obj_t *browser_screen;

void browser_init(gui_config_t *cfg);

// Re-reads what is playing and moves the accent mark onto its row. Called
// from the player at every track change.
void browser_notify_now_playing(void);

// Re-reads the directory currently shown. The player may well have started
// before the OS finished mounting the card -- in which case the first listing
// saw whatever happened to be sitting in the empty mount point -- so the
// browser refreshes itself every time the page is opened. The first call also
// chooses the folder to open (remembered, restored track's, Music, root).
void browser_refresh(void);

// Writes a pending scroll down before a normal power-off or restart, which do
// not come back through the interface. Called on the UI thread.
void browser_state_flush(void);

// Goes up one directory. Returns false when already at the SD card root, which
// is the caller's cue to leave the browser entirely (see back_btn_cb): the
// floating chevron is the only way back, there is no ".." row in the list.
bool browser_go_up(void);
bool browser_can_go_up(void); // true when the chevron/swipe walks up a folder instead of leaving

// Opens a directory directly. Only the soak test uses this; the player itself
// navigates through the rows and the back button.
void browser_open_dir_public(const char *path);

#ifdef HOST_BUILD
// Simulator-only: scroll the current listing to `y`, as a finger would.
void browser_test_scroll(int y);
// Simulator-only: take the folder branch of a row tap, so the ancestor scroll
// is pushed the same way a finger entering a folder would push it.
void browser_test_open_child(const char *path);
#endif

#endif /* BROWSER_H */
