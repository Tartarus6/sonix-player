#include "pod_layout.h"

#include <stdbool.h>
#include <stdio.h>

#include "src/gui/library/audiobooks.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/settings/settings.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/wireless/sonixlink.h"
#include "src/gui/wireless/wifitransfer.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

#include "src/gui/ebook/ebookpage.h"
#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/bluetooth/btsettings.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/library/filespage.h"
#include "src/gui/shell/morepage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/streaming.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/wireless/airplay.h"
#include "src/gui/wireless/dlna.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/gui/wireless/wireless.h"

// ---------------------------------------------------------------------------
// In-memory layout state
// ---------------------------------------------------------------------------

static pod_app_t grid[POD_GRID_SLOTS];     /* POD_APP_NONE for empty */
static pod_app_t dock[POD_DOCK_SLOTS];
static bool      visible[POD_APP_COUNT];
static int       remembered[POD_APP_COUNT]; /* -1 or encoded slot index */

// ---------------------------------------------------------------------------
// Action stubs for shortcuts that open a media-list view rather than a whole
// screen. Each one calls the public medialist_open() / playlistpage_open() API
// that music.c uses internally, so Pod shortcuts reach the same destination as
// the Music-page tiles.
// ---------------------------------------------------------------------------

static void action_albums(void) {
    medialist_open(tr("albums"), LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL);
}

static void action_artists(void) {
    medialist_open(tr("artists"), LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL);
}

static void action_playlists(void) {
    playlistpage_open();
}

// ---------------------------------------------------------------------------
// Shortcut metadata tables
// ---------------------------------------------------------------------------

typedef struct {
    const char           *label;    /* translation tag */
    const lv_image_dsc_t *icon;
    lv_obj_t            **target;   /* NULL when action-based */
    void                (*action)(void); /* NULL when target-based */
} pod_app_meta_t;

static const pod_app_meta_t META[POD_APP_COUNT] = {
    [POD_APP_MUSIC]        = { "music",         &icon_menu_music,         &music_screen,       NULL            },
    [POD_APP_ALBUMS]       = { "albums",         &icon_menu_album,         NULL,                action_albums   },
    [POD_APP_ARTISTS]      = { "artists",        &icon_menu_artist,        NULL,                action_artists  },
    [POD_APP_PLAYLISTS]    = { "playlists",      &icon_menu_playlist,      NULL,                action_playlists},
    [POD_APP_PODCASTS]     = { "podcasts",       &icon_menu_podcast,       &podcast_screen,     NULL            },
    [POD_APP_RADIO]        = { "radio",          &icon_menu_radio,         &radiopage_screen,   NULL            },
    [POD_APP_AUDIOBOOKS]   = { "audiobooks",     &icon_menu_audiobooks,    &audiobooks_screen,  NULL            },
    // An action, as on the More page: the shelf rereads the Ebook folder each
    // time it opens.
    [POD_APP_EBOOKS]       = { "books",          &icon_menu_books,         NULL,                ebookpage_open  },
    [POD_APP_SETTINGS]     = { "settings",       &icon_menu_settings,      &settings_screen,    NULL            },
    // "transfer" is the tag the Wireless page uses; there is no "wifi_transfer".
    [POD_APP_WIFITRANSFER] = { "transfer",       &icon_menu_wifi_transfer, &wifitransfer_screen,NULL            },
    [POD_APP_SONIXLINK]    = { "sonixlink",      &icon_menu_sonixlink,     &sonixlink_screen,   NULL            },

    // The rest mirror the Classic menus' grid_entry_t tables entry for entry
    // (main_menu.c, streaming.c, wireless.c, morepage.c): same label tag,
    // artwork, and destination.
    [POD_APP_STREAMING]    = { "streaming",      &icon_menu_streaming,     &streaming_screen,   NULL            },
    [POD_APP_WIRELESS]     = { "wireless",       &icon_menu_wireless,      &wireless_screen,    NULL            },
    [POD_APP_MORE]         = { "more",           &icon_menu_more,          &morepage_screen,    NULL            },
    [POD_APP_TIDAL]        = { "tidal",          &icon_menu_tidal,         &tidal_screen,       NULL            },
    [POD_APP_QOBUZ]        = { "qobuz",          &icon_menu_qobuz,         &qobuz_screen,       NULL            },
    [POD_APP_WIFI]         = { "wi_fi",          &icon_menu_wifi_settings, &wifisettings_screen,NULL            },
    [POD_APP_BLUETOOTH]    = { "bluetooth",      &icon_menu_bluetooth,     &btsettings_screen,  NULL            },
    [POD_APP_AIRPLAY]      = { "airplay",        &icon_menu_airplay,       &airplay_screen,     NULL            },
    [POD_APP_DLNA]         = { "dlna",           &icon_menu_dlna,          &dlna_screen,        NULL            },
    [POD_APP_DAC]          = { "dac",            &icon_menu_dac,           &dacpage_screen,     NULL            },
    [POD_APP_GEARBOY]      = { "gearboy",        &icon_menu_gearboy,       &gearboypage_screen, NULL            },
    // Actions for the same reasons as on the More page: the file listing and
    // the game artwork are read on the way in.
    [POD_APP_FILES]        = { "file_explorer",  &icon_menu_file_explorer, NULL,                filespage_open  },
    [POD_APP_FLAPPYBIRD]   = { "flappy_bird",    &icon_menu_flappy_bird,   NULL,                flappybird_open },
    [POD_APP_METRONOME]    = { "metronome",      &icon_menu_metronome,     &metronomepage_screen,NULL           },
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Returns true when `app` is a valid, non-sentinel app ID.
static bool app_valid(pod_app_t app) {
    return app >= 0 && app < POD_APP_COUNT;
}

// Finds which grid or dock slot currently holds `app`. Returns the encoded
// slot index (0–8 for grid, POD_DOCK_SLOT_OFFSET + 0–2 for dock), or -1 when
// the app is not placed anywhere.
static int find_slot(pod_app_t app) {
    if (!app_valid(app)) {
        return -1;
    }
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        if (grid[i] == app) {
            return i;
        }
    }
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        if (dock[i] == app) {
            return POD_DOCK_SLOT_OFFSET + i;
        }
    }
    return -1;
}

// Removes `app` from wherever it is placed. No-op when not placed.
static void clear_app(pod_app_t app) {
    if (!app_valid(app)) {
        return;
    }
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        if (grid[i] == app) {
            grid[i] = POD_APP_NONE;
        }
    }
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        if (dock[i] == app) {
            dock[i] = POD_APP_NONE;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void pod_layout_default(void) {
    // Grid slots 0–5: the six primary shortcuts.
    grid[0] = POD_APP_MUSIC;
    grid[1] = POD_APP_ALBUMS;
    grid[2] = POD_APP_ARTISTS;
    grid[3] = POD_APP_PLAYLISTS;
    grid[4] = POD_APP_PODCASTS;
    grid[5] = POD_APP_RADIO;
    grid[6] = POD_APP_NONE;
    grid[7] = POD_APP_NONE;
    grid[8] = POD_APP_NONE;

    // Dock: Settings in slot 0, rest empty.
    dock[0] = POD_APP_SETTINGS;
    dock[1] = POD_APP_NONE;
    dock[2] = POD_APP_NONE;

    // All shortcuts are visible by default.
    for (int i = 0; i < POD_APP_COUNT; i++) {
        visible[i] = true;
        remembered[i] = -1;
    }
}

void pod_layout_init(void) {
    // Start from an all-empty state so the validation logic below can rely on
    // a clean slate when it falls back to defaults for individual slots.
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        grid[i] = POD_APP_NONE;
    }
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        dock[i] = POD_APP_NONE;
    }
    for (int i = 0; i < POD_APP_COUNT; i++) {
        visible[i] = true;
        remembered[i] = -1;
    }

    // Track which apps have already been assigned so duplicate IDs from the
    // config are discarded (earlier slot wins).
    bool placed[POD_APP_COUNT] = {false};

    bool any_bad = false;

    // ---- grid slots ----
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "grid_%d", i);
        long v = config_get_int("pod_home", key, (long)POD_APP_NONE);

        if (v == (long)POD_APP_NONE) {
            grid[i] = POD_APP_NONE;
            continue;
        }

        if (v < 0 || v >= POD_APP_COUNT) {
            fprintf(stderr, "pod_layout: grid_%d has out-of-range id %ld, treating as empty\n", i, v);
            any_bad = true;
            grid[i] = POD_APP_NONE;
            continue;
        }

        pod_app_t app = (pod_app_t)v;
        if (placed[app]) {
            fprintf(stderr, "pod_layout: app %d seen twice; discarding duplicate at grid_%d\n", (int)app, i);
            any_bad = true;
            grid[i] = POD_APP_NONE;
            continue;
        }

        grid[i] = app;
        placed[app] = true;
    }

    // ---- dock slots ----
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dock_%d", i);
        long v = config_get_int("pod_home", key, (long)POD_APP_NONE);

        if (v == (long)POD_APP_NONE) {
            dock[i] = POD_APP_NONE;
            continue;
        }

        if (v < 0 || v >= POD_APP_COUNT) {
            fprintf(stderr, "pod_layout: dock_%d has out-of-range id %ld, treating as empty\n", i, v);
            any_bad = true;
            dock[i] = POD_APP_NONE;
            continue;
        }

        pod_app_t app = (pod_app_t)v;
        if (placed[app]) {
            fprintf(stderr, "pod_layout: app %d seen twice; discarding duplicate at dock_%d\n", (int)app, i);
            any_bad = true;
            dock[i] = POD_APP_NONE;
            continue;
        }

        dock[i] = app;
        placed[app] = true;
    }

    // ---- visibility and remembered-slot ----
    for (int i = 0; i < POD_APP_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "visible_%d", i);
        long v = config_get_int("pod_home", key, 1); /* default: visible */
        visible[i] = (v != 0);

        snprintf(key, sizeof(key), "remembered_%d", i);
        long r = config_get_int("pod_home", key, -1);
        // Validate: must be -1, 0–8, or POD_DOCK_SLOT_OFFSET + 0–2.
        bool ok = (r == -1)
               || (r >= 0 && r < POD_GRID_SLOTS)
               || (r >= POD_DOCK_SLOT_OFFSET && r < POD_DOCK_SLOT_OFFSET + POD_DOCK_SLOTS);
        if (ok) {
            remembered[i] = (int)r;
        } else {
            fprintf(stderr, "pod_layout: remembered_%d has invalid value %ld, clearing\n", i, r);
            any_bad = true;
            remembered[i] = -1;
        }
    }

    // If nothing was stored at all (all keys missing) treat it as "no config
    // yet" and apply the factory default. The absence of grid_0 is used as the
    // sentinel because it is always written on save.
    bool section_present = config_get_int("pod_home", "grid_0", -999) != -999;
    if (!section_present) {
        fprintf(stderr, "pod_layout: [pod_home] section absent, applying default layout\n");
        pod_layout_default();
        return;
    }

    if (any_bad) {
        fprintf(stderr, "pod_layout: some layout keys were invalid; partial default applied\n");
    }
}

// ---------------------------------------------------------------------------
// Grid slot accessors
// ---------------------------------------------------------------------------

pod_app_t pod_layout_grid_at(int idx) {
    if (idx < 0 || idx >= POD_GRID_SLOTS) {
        return POD_APP_NONE;
    }
    return grid[idx];
}

bool pod_layout_grid_set(pod_app_t app, int idx) {
    if (idx < 0 || idx >= POD_GRID_SLOTS) {
        return false;
    }
    // Enforce uniqueness: remove the app from wherever it currently sits.
    if (app_valid(app)) {
        clear_app(app);
    }
    grid[idx] = app;
    return true;
}

// ---------------------------------------------------------------------------
// Dock slot accessors
// ---------------------------------------------------------------------------

pod_app_t pod_layout_dock_at(int idx) {
    if (idx < 0 || idx >= POD_DOCK_SLOTS) {
        return POD_APP_NONE;
    }
    return dock[idx];
}

bool pod_layout_dock_set(pod_app_t app, int idx) {
    if (idx < 0 || idx >= POD_DOCK_SLOTS) {
        return false;
    }
    if (app_valid(app)) {
        clear_app(app);
    }
    dock[idx] = app;
    return true;
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

bool pod_layout_visible(pod_app_t app) {
    if (!app_valid(app)) {
        return false;
    }
    return visible[app];
}

void pod_layout_set_visible(pod_app_t app, bool vis) {
    if (!app_valid(app)) {
        return;
    }

    if (!vis) {
        // ---- Hiding ----
        // Record the current slot before clearing it.
        int slot = find_slot(app);
        remembered[app] = slot; /* -1 when not placed; that is fine */
        clear_app(app);
        visible[app] = false;
        return;
    }

    // ---- Showing ----
    // If the app is already placed somewhere, only mark it visible and return.
    // No placement logic is needed: the app is already occupying its slot.
    if (find_slot(app) >= 0) {
        visible[app] = true;
        return;
    }

    visible[app] = true;

    // Try to restore to the remembered slot.
    int r = remembered[app];
    if (r >= 0 && r < POD_GRID_SLOTS) {
        // Grid remembered slot: restore only when it is empty.
        if (grid[r] == POD_APP_NONE) {
            grid[r] = app;
            return;
        }
    } else if (r >= POD_DOCK_SLOT_OFFSET && r < POD_DOCK_SLOT_OFFSET + POD_DOCK_SLOTS) {
        // Dock remembered slot.
        int di = r - POD_DOCK_SLOT_OFFSET;
        if (dock[di] == POD_APP_NONE) {
            dock[di] = app;
            return;
        }
    }

    // Remembered slot unavailable (or never set): scan grid slots 0–8 for the
    // first empty one (left to right, top to bottom — row-major).
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        if (grid[i] == POD_APP_NONE) {
            grid[i] = app;
            return;
        }
    }

    // All grid slots occupied: leave the shortcut visible but unplaced. The
    // Layout Editor will show it as "not in any slot", which is correct.
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void pod_layout_save(void) {
    // Grid slots.
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "grid_%d", i);
        config_set_int("pod_home", key, (long)grid[i]);
    }

    // Dock slots.
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "dock_%d", i);
        config_set_int("pod_home", key, (long)dock[i]);
    }

    // All 11 visible and remembered entries: written every time to avoid stale
    // data from an earlier partial write (Requirement 4.6).
    for (int i = 0; i < POD_APP_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "visible_%d", i);
        config_set_int("pod_home", key, visible[i] ? 1 : 0);

        snprintf(key, sizeof(key), "remembered_%d", i);
        config_set_int("pod_home", key, (long)remembered[i]);
    }

    if (!config_save()) {
        fprintf(stderr, "pod_layout: config_save() failed; layout kept in memory only\n");
    }
}

// ---------------------------------------------------------------------------
// Shortcut metadata
// ---------------------------------------------------------------------------

const char *pod_app_label(pod_app_t app) {
    if (!app_valid(app)) {
        return "";
    }
    return META[app].label;
}

const lv_image_dsc_t *pod_app_icon(pod_app_t app) {
    if (!app_valid(app)) {
        return NULL;
    }
    return META[app].icon;
}

lv_obj_t **pod_app_target(pod_app_t app) {
    if (!app_valid(app)) {
        return NULL;
    }
    return META[app].target;
}

void (*pod_app_action(pod_app_t app))(void) {
    if (!app_valid(app)) {
        return NULL;
    }
    return META[app].action;
}
