#ifndef POD_LAYOUT_H
#define POD_LAYOUT_H

// ---------------------------------------------------------------------------
// pod_layout — slot model, persistence, and metadata for the Pod home screen
//
// Owns which App_Shortcut sits in which grid/dock slot and which shortcuts are
// visible. Pure logic: no LVGL objects, no display context. All state lives in
// four static arrays; call pod_layout_save() to put it in [pod_home] in the
// config INI.
//
// Slot index encoding used in the remembered[] array:
//   0–8   → grid slot
//   100–102 → dock slot
//   -1    → none
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define POD_GRID_SLOTS 9  /* 3 × 3 */
#define POD_DOCK_SLOTS 3

// Sentinel that encodes dock slot 0 in the remembered[] array.
#define POD_DOCK_SLOT_OFFSET 100

// ---------------------------------------------------------------------------
// App catalogue
// ---------------------------------------------------------------------------

typedef enum {
    POD_APP_MUSIC        = 0,
    POD_APP_ALBUMS       = 1,
    POD_APP_ARTISTS      = 2,
    POD_APP_PLAYLISTS    = 3,
    POD_APP_PODCASTS     = 4,
    POD_APP_RADIO        = 5,
    POD_APP_AUDIOBOOKS   = 6,
    POD_APP_EBOOKS       = 7,
    POD_APP_SETTINGS     = 8,
    POD_APP_WIFITRANSFER = 9,
    POD_APP_SONIXLINK    = 10,
    // Everything else the Classic menus offer. The numbers are what
    // [pod_home] stores, so new entries only ever go at the end.
    // Main menu folders:
    POD_APP_STREAMING    = 11,
    POD_APP_WIRELESS     = 12,
    POD_APP_MORE         = 13,
    // Streaming page (Radio and Podcasts are above):
    POD_APP_TIDAL        = 14,
    POD_APP_QOBUZ        = 15,
    // Wireless page (Transfer and SonixLink are above):
    POD_APP_WIFI         = 16,
    POD_APP_BLUETOOTH    = 17,
    POD_APP_AIRPLAY      = 18,
    POD_APP_DLNA         = 19,
    // More page (Books is above):
    POD_APP_DAC          = 20,
    POD_APP_GEARBOY      = 21,
    POD_APP_FILES        = 22,
    POD_APP_FLAPPYBIRD   = 23,
    POD_APP_METRONOME    = 24,
    POD_APP_CALCULATOR   = 25,
    POD_APP_CROSSYROAD   = 26,
    POD_APP_COUNT        = 27,
    POD_APP_NONE         = -1   /* empty slot sentinel */
} pod_app_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Loads persisted layout from [pod_home] in the config; applies the factory
// default and logs a warning when data is absent or unreadable.
void pod_layout_init(void);

// Applies and persists the factory default layout (Requirements 7.4):
//   Grid 0–5 → Music / Albums / Artists / Playlists / Podcasts / Radio
//   Grid 6–8 → empty
//   Dock 0   → Settings
//   Dock 1–2 → empty
void pod_layout_default(void);

// ---------------------------------------------------------------------------
// Grid slot accessors  (index 0–8, row-major)
// ---------------------------------------------------------------------------

pod_app_t pod_layout_grid_at(int idx);

// Places `app` in grid slot `idx`. Clears the app from its previous slot first
// to maintain the uniqueness invariant. Returns false and leaves the layout
// unchanged when `idx` is out of range.
bool pod_layout_grid_set(pod_app_t app, int idx);

// ---------------------------------------------------------------------------
// Dock slot accessors  (index 0–2)
// ---------------------------------------------------------------------------

pod_app_t pod_layout_dock_at(int idx);

// Places `app` in dock slot `idx`. Same uniqueness guarantee as grid_set.
// Returns false and leaves the layout unchanged when `idx` is out of range.
bool pod_layout_dock_set(pod_app_t app, int idx);

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

bool pod_layout_visible(pod_app_t app);

// Hide (visible = false): removes the app from its current slot and records
//   that slot in remembered[] using the grid/dock encoding described above.
// Show (visible = true): restores to the remembered slot if it is still empty;
//   otherwise scans grid slots 0–8 for the first POD_APP_NONE.
void pod_layout_set_visible(pod_app_t app, bool visible);

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Writes all grid_N, dock_N, visible_N, and remembered_N keys to [pod_home]
// via config_set_int(), then calls config_save().
void pod_layout_save(void);

// ---------------------------------------------------------------------------
// Shortcut metadata  (static lookup tables)
// ---------------------------------------------------------------------------

// A translation tag suitable for tr(), e.g. "music", "albums".
const char *pod_app_label(pod_app_t app);

// Full-colour 128 px ARGB8888 artwork (the Classic menu's icon_menu_*).
// Draw it as it is: recolouring turns it into a flat blob.
const lv_image_dsc_t *pod_app_icon(pod_app_t app);

// The screen to navigate to, or NULL when the app uses an action instead.
lv_obj_t **pod_app_target(pod_app_t app);

// The action to invoke, or NULL when the app uses a target screen instead.
void (*pod_app_action(pod_app_t app))(void);

#endif /* POD_LAYOUT_H */
