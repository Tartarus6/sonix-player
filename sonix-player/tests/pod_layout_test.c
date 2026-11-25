/*
 * pod_layout_test.c — unit and property-based tests for pod_layout.c
 *
 * Tests run against the in-memory model only. No LVGL context, no display, no
 * device config file.  All external symbols used by pod_layout.c are stubbed
 * below.
 *
 * Build & run:
 *   cd tests && make pod_layout_test && ./pod_layout_test
 */

/* ---------------------------------------------------------------------------
 * Minimal stubs for pod_layout.c dependencies
 * --------------------------------------------------------------------------- */

/* ---- LVGL type stubs ---- */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

typedef struct { uint8_t cf; } lv_color_format_t;
typedef struct {
    uint32_t magic : 8;
    uint32_t cf    : 8;
    uint32_t flags : 16;
    uint32_t w     : 16;
    uint32_t h     : 16;
    uint32_t stride: 16;
    uint32_t reserved_2 : 16;
} lv_image_header_t;
typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;
typedef struct _lv_obj_t lv_obj_t;

/* lvgl.h would include everything — provide just the types so pod_layout.h compiles. */
#define LVGL_LVGL_H  /* prevent real inclusion */

/* ---- Config stub ---- */
#define CONFIG_MAX_ENTRIES 512
static struct {
    char section[32];
    char key[32];
    char value[64];
} _cfg[CONFIG_MAX_ENTRIES];
static int _cfg_count = 0;

static void _cfg_clear(void) { _cfg_count = 0; }

static int _cfg_find(const char *section, const char *key) {
    for (int i = 0; i < _cfg_count; i++) {
        if (strcmp(_cfg[i].section, section) == 0 && strcmp(_cfg[i].key, key) == 0)
            return i;
    }
    return -1;
}

void config_set_int(const char *section, const char *key, long value) {
    int idx = _cfg_find(section, key);
    if (idx < 0) {
        if (_cfg_count >= CONFIG_MAX_ENTRIES) return;
        idx = _cfg_count++;
        snprintf(_cfg[idx].section, sizeof(_cfg[idx].section), "%s", section);
        snprintf(_cfg[idx].key,     sizeof(_cfg[idx].key),     "%s", key);
    }
    snprintf(_cfg[idx].value, sizeof(_cfg[idx].value), "%ld", value);
}

long config_get_int(const char *section, const char *key, long fallback) {
    int idx = _cfg_find(section, key);
    if (idx < 0) return fallback;
    char *end = NULL;
    long v = strtol(_cfg[idx].value, &end, 10);
    return (end && end != _cfg[idx].value) ? v : fallback;
}

bool config_save(void) { return true; }

/* ---- Lang stub ---- */
const char *tr(const char *text) { return text; }

/* ---- Icon stubs ---- */
const lv_image_dsc_t icon_menu_music         = {0};
const lv_image_dsc_t icon_menu_album         = {0};
const lv_image_dsc_t icon_menu_artist        = {0};
const lv_image_dsc_t icon_menu_playlist      = {0};
const lv_image_dsc_t icon_menu_podcast       = {0};
const lv_image_dsc_t icon_menu_radio         = {0};
const lv_image_dsc_t icon_menu_audiobooks    = {0};
const lv_image_dsc_t icon_menu_books         = {0};
const lv_image_dsc_t icon_menu_settings      = {0};
const lv_image_dsc_t icon_menu_wifi_transfer = {0};
const lv_image_dsc_t icon_menu_sonixlink     = {0};

const lv_image_dsc_t icon_menu_streaming = {0};
const lv_image_dsc_t icon_menu_wireless = {0};
const lv_image_dsc_t icon_menu_more = {0};
const lv_image_dsc_t icon_menu_tidal = {0};
const lv_image_dsc_t icon_menu_qobuz = {0};
const lv_image_dsc_t icon_menu_wifi_settings = {0};
const lv_image_dsc_t icon_menu_bluetooth = {0};
const lv_image_dsc_t icon_menu_airplay = {0};
const lv_image_dsc_t icon_menu_dlna = {0};
const lv_image_dsc_t icon_menu_dac = {0};
const lv_image_dsc_t icon_menu_gearboy = {0};
const lv_image_dsc_t icon_menu_file_explorer = {0};
const lv_image_dsc_t icon_menu_flappy_bird = {0};
const lv_image_dsc_t icon_menu_metronome = {0};
const lv_image_dsc_t icon_menu_calculator = {0};
/* ---- Screen stubs ---- */
lv_obj_t *music_screen        = NULL;
lv_obj_t *podcast_screen      = NULL;
lv_obj_t *radiopage_screen    = NULL;
lv_obj_t *audiobooks_screen   = NULL;
lv_obj_t *ebookpage_screen    = NULL;
lv_obj_t *settings_screen     = NULL;
lv_obj_t *wifitransfer_screen = NULL;
lv_obj_t *sonixlink_screen    = NULL;

lv_obj_t *streaming_screen = NULL;
lv_obj_t *wireless_screen = NULL;
lv_obj_t *morepage_screen = NULL;
lv_obj_t *tidal_screen = NULL;
lv_obj_t *qobuz_screen = NULL;
lv_obj_t *wifisettings_screen = NULL;
lv_obj_t *btsettings_screen = NULL;
lv_obj_t *airplay_screen = NULL;
lv_obj_t *dlna_screen = NULL;
lv_obj_t *dacpage_screen = NULL;
lv_obj_t *gearboypage_screen = NULL;
lv_obj_t *metronomepage_screen = NULL;
lv_obj_t *calculatorpage_screen = NULL;
/* ---- Stubs for action-based shortcuts ---- */
void ebookpage_open(void) {}
void filespage_open(void) {}
void flappybird_open(void) {}
void medialist_open(const char *title, int list_type, int filter, void *param) { (void)title; (void)list_type; (void)filter; (void)param; }
void playlistpage_open(void) {}

/* ---- Enums from headers that pod_layout.c uses via its deps ---- */
#define LIBRARY_LIST_ALBUMS  0
#define LIBRARY_LIST_ARTISTS 1
#define LIBRARY_FILTER_NONE  0

/* ---------------------------------------------------------------------------
 * Include the module under test
 * --------------------------------------------------------------------------- */
/* We need to prevent pod_layout.h from pulling in the real lvgl.h.
   The easiest approach: provide a fake lvgl/lvgl.h that only includes
   the already-defined types above. */

/* Redefine the include guard so the real file is skipped, but types are there. */
#define LVGL_H
/* pod_layout.h includes "lvgl/lvgl.h" with a relative path; we need to point
   that to our stub. We do this by wrapping the include. */

/* Trick: define the include guard of lvgl.h before including pod_layout.h so
   the #include "lvgl/lvgl.h" inside the header becomes a no-op. The types it
   would have defined are already defined above. */
/* This won't work directly since "lvgl/lvgl.h" is a path include.
   Instead we compile with -I. and provide a local lvgl/lvgl.h stub. */

/* The test Makefile handles this with -include stubs before pod_layout. */
#include "../src/gui/shell/pod_layout.h"
/* Implementation is compiled separately via the Makefile (-DPOD_LAYOUT_TEST). */

/* ---------------------------------------------------------------------------
 * Test framework — minimal pass/fail counting
 * --------------------------------------------------------------------------- */

static int _pass = 0;
static int _fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { \
        printf("  PASS  %s\n", name); \
        _pass++; \
    } else { \
        printf("  FAIL  %s  [line %d]\n", name, __LINE__); \
        _fail++; \
    } \
} while (0)

#define CHECKF(cond, name, ...) do { \
    if (cond) { \
        printf("  PASS  " name "\n", ##__VA_ARGS__); \
        _pass++; \
    } else { \
        printf("  FAIL  " name "  [line %d]\n", ##__VA_ARGS__, __LINE__); \
        _fail++; \
    } \
} while (0)

/* ---------------------------------------------------------------------------
 * Property-based test helpers (simple PRNG, 100+ iterations per property)
 * --------------------------------------------------------------------------- */

static uint64_t _prng_state = 0;

static void prng_seed(uint64_t seed) { _prng_state = seed ? seed : 12345678ULL; }

/* xorshift64 */
static uint64_t prng_next(void) {
    _prng_state ^= _prng_state << 13;
    _prng_state ^= _prng_state >> 7;
    _prng_state ^= _prng_state << 17;
    return _prng_state;
}

static int prng_int(int lo, int hi) {
    /* lo inclusive, hi exclusive */
    if (hi <= lo) return lo;
    return lo + (int)(prng_next() % (unsigned)(hi - lo));
}

/* Generate a random valid app (0..POD_APP_COUNT-1). */
static pod_app_t gen_app(void) {
    return (pod_app_t)prng_int(0, POD_APP_COUNT);
}

/* Generate an optional app: POD_APP_NONE with ~20% probability. */
static pod_app_t gen_app_or_none(void) {
    if (prng_int(0, 5) == 0) return POD_APP_NONE;
    return gen_app();
}

/* Build a fresh random layout state without duplicates.
   Places apps into random slots (70% chance each), hiding 25% randomly. */
static void gen_layout_state(void) {
    /* Start from a clean slate: clear all slots, then hide all apps while
       they are unplaced so remembered[] is reset to -1 for all. */
    pod_layout_default();
    for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
    for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);
    /* Hide all while unplaced → remembered[all] = -1, visible[all] = false. */
    for (pod_app_t a = 0; a < POD_APP_COUNT; a++) pod_layout_set_visible(a, false);

    bool grid_used[POD_GRID_SLOTS] = {false};
    bool dock_used[POD_DOCK_SLOTS] = {false};
    for (pod_app_t a = 0; a < POD_APP_COUNT; a++) {
        if (prng_int(0, 10) < 7) {
            /* Try up to 20 times to find a free slot. */
            for (int tries = 0; tries < 20; tries++) {
                int area = prng_int(0, 2); /* 0 = grid, 1 = dock */
                if (area == 0) {
                    int idx = prng_int(0, POD_GRID_SLOTS);
                    if (!grid_used[idx]) {
                        pod_layout_grid_set(a, idx);
                        grid_used[idx] = true;
                        break;
                    }
                } else {
                    int idx = prng_int(0, POD_DOCK_SLOTS);
                    if (!dock_used[idx]) {
                        pod_layout_dock_set(a, idx);
                        dock_used[idx] = true;
                        break;
                    }
                }
            }
        }
        /* Mark as visible (apps are hidden from the reset above). */
        if (prng_int(0, 4) != 0) {
            /* 75% visible: but we must not use set_visible(true) on unplaced apps
               since that would trigger a placement scan. Set visible flag directly
               via a hide/show on the placed slot: if placed, set_visible(false) then
               set_visible(true); if not placed, leave hidden.
               Simplest: just hide ~25% of placed apps, leave rest visible. */
        }
    }
    /* For placed apps: randomly hide 25% of them. */
    for (pod_app_t a = 0; a < POD_APP_COUNT; a++) {
        /* Check if placed. */
        bool in_grid = false, in_dock = false;
        for (int i = 0; i < POD_GRID_SLOTS; i++) if (pod_layout_grid_at(i) == a) { in_grid = true; break; }
        for (int i = 0; i < POD_DOCK_SLOTS; i++) if (pod_layout_dock_at(i) == a) { in_dock = true; break; }
        if (in_grid || in_dock) {
            if (prng_int(0, 4) == 0) {
                pod_layout_set_visible(a, false); /* hide ~25% of placed apps */
            } else {
                /* Mark visible without triggering placement scan: use a direct hide+show
                   but since it's placed, set_visible(true) now just sets the flag. */
                pod_layout_set_visible(a, true);
            }
        }
        /* Unplaced apps remain hidden (visible=false from the reset). */
    }
}

/* ---------------------------------------------------------------------------
 * Helper: check the uniqueness invariant on the current layout state.
 * Returns true when no non-NONE app appears more than once.
 * --------------------------------------------------------------------------- */
static bool check_uniqueness(void) {
    bool seen[POD_APP_COUNT] = {false};
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        pod_app_t a = pod_layout_grid_at(i);
        if (a == POD_APP_NONE) continue;
        if (a < 0 || a >= POD_APP_COUNT) return false;
        if (seen[a]) return false;
        seen[a] = true;
    }
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        pod_app_t a = pod_layout_dock_at(i);
        if (a == POD_APP_NONE) continue;
        if (a < 0 || a >= POD_APP_COUNT) return false;
        if (seen[a]) return false;
        seen[a] = true;
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Helper: snapshot / restore layout state for round-trip tests
 * --------------------------------------------------------------------------- */
typedef struct {
    pod_app_t grid[POD_GRID_SLOTS];
    pod_app_t dock[POD_DOCK_SLOTS];
    bool      visible[POD_APP_COUNT];
} layout_snapshot_t;

static layout_snapshot_t snap_capture(void) {
    layout_snapshot_t s;
    for (int i = 0; i < POD_GRID_SLOTS; i++) s.grid[i] = pod_layout_grid_at(i);
    for (int i = 0; i < POD_DOCK_SLOTS; i++) s.dock[i] = pod_layout_dock_at(i);
    for (int i = 0; i < POD_APP_COUNT; i++) s.visible[i] = pod_layout_visible((pod_app_t)i);
    return s;
}

static bool snap_equal(const layout_snapshot_t *a, const layout_snapshot_t *b) {
    if (memcmp(a->grid, b->grid, sizeof(a->grid)) != 0) return false;
    if (memcmp(a->dock, b->dock, sizeof(a->dock)) != 0) return false;
    if (memcmp(a->visible, b->visible, sizeof(a->visible)) != 0) return false;
    return true;
}

/* ---------------------------------------------------------------------------
 * === UNIT TESTS ===
 * --------------------------------------------------------------------------- */

static void test_unit_default_layout(void) {
    printf("\n[Unit] Default layout\n");
    _cfg_clear();
    pod_layout_default();

    CHECK(pod_layout_grid_at(0) == POD_APP_MUSIC,     "grid[0] == MUSIC");
    CHECK(pod_layout_grid_at(1) == POD_APP_ALBUMS,    "grid[1] == ALBUMS");
    CHECK(pod_layout_grid_at(2) == POD_APP_ARTISTS,   "grid[2] == ARTISTS");
    CHECK(pod_layout_grid_at(3) == POD_APP_PLAYLISTS, "grid[3] == PLAYLISTS");
    CHECK(pod_layout_grid_at(4) == POD_APP_PODCASTS,  "grid[4] == PODCASTS");
    CHECK(pod_layout_grid_at(5) == POD_APP_RADIO,     "grid[5] == RADIO");
    CHECK(pod_layout_grid_at(6) == POD_APP_NONE,      "grid[6] == empty");
    CHECK(pod_layout_grid_at(7) == POD_APP_NONE,      "grid[7] == empty");
    CHECK(pod_layout_grid_at(8) == POD_APP_NONE,      "grid[8] == empty");
    CHECK(pod_layout_dock_at(0) == POD_APP_SETTINGS,  "dock[0] == SETTINGS");
    CHECK(pod_layout_dock_at(1) == POD_APP_NONE,      "dock[1] == empty");
    CHECK(pod_layout_dock_at(2) == POD_APP_NONE,      "dock[2] == empty");
}

static void test_unit_init_empty_config(void) {
    printf("\n[Unit] pod_layout_init() with empty config → default layout\n");
    _cfg_clear();
    pod_layout_init(); /* no pod_home keys → should call pod_layout_default() */

    /* Verify default layout is applied. */
    CHECK(pod_layout_grid_at(0) == POD_APP_MUSIC,    "init+empty → grid[0]==MUSIC");
    CHECK(pod_layout_dock_at(0) == POD_APP_SETTINGS, "init+empty → dock[0]==SETTINGS");
    for (int i = 6; i < POD_GRID_SLOTS; i++) {
        char name[64];
        snprintf(name, sizeof(name), "init+empty → grid[%d]==empty", i);
        CHECK(pod_layout_grid_at(i) == POD_APP_NONE, name);
    }
}

static void test_unit_out_of_range_returns_false(void) {
    printf("\n[Unit] Out-of-range slot index → returns false\n");
    _cfg_clear();
    pod_layout_default();

    CHECK(!pod_layout_grid_set(POD_APP_MUSIC, 9),   "grid_set(app,  9) → false");
    CHECK(!pod_layout_grid_set(POD_APP_MUSIC, -1),  "grid_set(app, -1) → false");
    CHECK(!pod_layout_dock_set(POD_APP_MUSIC,  3),  "dock_set(app,  3) → false");
    CHECK(!pod_layout_dock_set(POD_APP_MUSIC, -1),  "dock_set(app, -1) → false");

    /* Music should still be at grid[0] unchanged */
    CHECK(pod_layout_grid_at(0) == POD_APP_MUSIC, "music still at grid[0] after out-of-range calls");
}

static void test_unit_reenable_occupied_remembered_uses_first_free(void) {
    printf("\n[Unit] Re-enable when remembered slot is occupied → first free\n");
    _cfg_clear();
    /* Fill grid completely: place all apps. */
    pod_layout_default();
    /* Clear everything to have a known state. */
    for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
    for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);

    /* Place MUSIC at grid[2] and remember it. */
    pod_layout_grid_set(POD_APP_MUSIC, 2);
    /* Hide MUSIC → remembered[MUSIC] = 2. */
    pod_layout_set_visible(POD_APP_MUSIC, false);
    CHECK(pod_layout_grid_at(2) == POD_APP_NONE, "grid[2] empty after hide");

    /* Now occupy grid[2] with ALBUMS. */
    pod_layout_grid_set(POD_APP_ALBUMS, 2);
    CHECK(pod_layout_grid_at(2) == POD_APP_ALBUMS, "grid[2] occupied by ALBUMS");

    /* Re-enable MUSIC: remembered slot (2) is taken; must land at first empty. */
    pod_layout_set_visible(POD_APP_MUSIC, true);

    /* First empty slot from 0: slot 0 and 1 are free since we cleared everything. */
    bool music_placed = false;
    for (int i = 0; i < POD_GRID_SLOTS; i++) {
        if (pod_layout_grid_at(i) == POD_APP_MUSIC) { music_placed = true; break; }
    }
    CHECK(music_placed, "MUSIC placed somewhere after re-enable");
    /* Must be at grid[0] (first free) since grid[0,1] were empty and grid[2] is taken. */
    CHECK(pod_layout_grid_at(0) == POD_APP_MUSIC, "MUSIC lands at grid[0] (first free)");
}

static void test_unit_duplicate_during_load(void) {
    printf("\n[Unit] Duplicate app ID during load → earlier slot wins\n");
    _cfg_clear();
    /* Manually populate config so MUSIC appears at both grid_0 and grid_1. */
    config_set_int("pod_home", "grid_0", (long)POD_APP_MUSIC);
    config_set_int("pod_home", "grid_1", (long)POD_APP_MUSIC); /* duplicate */
    for (int i = 2; i < POD_GRID_SLOTS; i++) {
        char key[16]; snprintf(key, sizeof(key), "grid_%d", i);
        config_set_int("pod_home", key, (long)POD_APP_NONE);
    }
    for (int i = 0; i < POD_DOCK_SLOTS; i++) {
        char key[16]; snprintf(key, sizeof(key), "dock_%d", i);
        config_set_int("pod_home", key, (long)POD_APP_NONE);
    }
    for (int i = 0; i < POD_APP_COUNT; i++) {
        char key[24];
        snprintf(key, sizeof(key), "visible_%d", i);
        config_set_int("pod_home", key, 1);
        snprintf(key, sizeof(key), "remembered_%d", i);
        config_set_int("pod_home", key, -1);
    }

    pod_layout_init();

    /* Earlier occurrence (grid_0) must win. */
    CHECK(pod_layout_grid_at(0) == POD_APP_MUSIC, "grid[0] holds MUSIC (earlier)");
    /* Later duplicate must be cleared. */
    CHECK(pod_layout_grid_at(1) == POD_APP_NONE,  "grid[1] is empty (duplicate discarded)");
}

static void test_unit_label_icon_accessors(void) {
    printf("\n[Unit] pod_app_label / pod_app_icon accessors\n");
    CHECK(pod_app_label(POD_APP_MUSIC)    != NULL && strlen(pod_app_label(POD_APP_MUSIC))    > 0, "label MUSIC non-empty");
    CHECK(pod_app_label(POD_APP_SETTINGS) != NULL && strlen(pod_app_label(POD_APP_SETTINGS)) > 0, "label SETTINGS non-empty");
    CHECK(pod_app_label(POD_APP_NONE)     != NULL,  "label NONE → non-null (empty string)");
    CHECK(pod_app_icon(POD_APP_MUSIC)     != NULL,  "icon MUSIC non-null");
    CHECK(pod_app_icon(POD_APP_SETTINGS)  != NULL,  "icon SETTINGS non-null");
    CHECK(pod_app_icon(POD_APP_NONE)      == NULL,  "icon NONE → null");
    /* Every valid app should have either a target or an action, not both null. */
    for (pod_app_t a = 0; a < POD_APP_COUNT; a++) {
        bool has_dest = (pod_app_target(a) != NULL || pod_app_action(a) != NULL);
        char name[64]; snprintf(name, sizeof(name), "app %d has target or action", (int)a);
        CHECK(has_dest, name);
    }
}

/* ---------------------------------------------------------------------------
 * === PROPERTY TESTS ===
 * --------------------------------------------------------------------------- */

#define PROP_ITERS 150

/*
 * Property 1: Layout persistence round-trip
 * Feature: ipod-classic-ui, Property 1: Layout persistence round-trip
 * Validates: Requirements 3.7, 4.6, 1.4
 */
static void prop_1_persistence_round_trip(void) {
    printf("\n[Property 1] Persistence round-trip (%d iterations)\n", PROP_ITERS);
    int failures = 0;
    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        gen_layout_state();

        layout_snapshot_t before = snap_capture();
        pod_layout_save();

        /* Wipe in-memory state so we can verify init restores it. */
        pod_layout_default(); /* clobber */

        pod_layout_init(); /* should read back what was saved */
        layout_snapshot_t after = snap_capture();

        if (!snap_equal(&before, &after)) {
            failures++;
            if (failures <= 3) {
                printf("    iter %d: round-trip mismatch\n", iter);
                /* Dump first divergence. */
                for (int i = 0; i < POD_GRID_SLOTS; i++) {
                    if (before.grid[i] != after.grid[i])
                        printf("      grid[%d]: saved %d, loaded %d\n", i, before.grid[i], after.grid[i]);
                }
                for (int i = 0; i < POD_DOCK_SLOTS; i++) {
                    if (before.dock[i] != after.dock[i])
                        printf("      dock[%d]: saved %d, loaded %d\n", i, before.dock[i], after.dock[i]);
                }
                for (int i = 0; i < POD_APP_COUNT; i++) {
                    if (before.visible[i] != after.visible[i])
                        printf("      visible[%d]: saved %d, loaded %d\n", i, (int)before.visible[i], (int)after.visible[i]);
                }
            }
        }
    }
    CHECK(failures == 0, "Property 1: round-trip holds for all iterations");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 2: Slot uniqueness invariant
 * Feature: ipod-classic-ui, Property 2: Slot uniqueness invariant
 * Validates: Requirements 3.3
 */
static void prop_2_slot_uniqueness(void) {
    printf("\n[Property 2] Slot uniqueness invariant (%d iterations)\n", PROP_ITERS);
    int failures = 0;
    _cfg_clear();
    pod_layout_default();

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        /* Apply a random mutation. */
        int op = prng_int(0, 4);
        if (op == 0) {
            /* grid_set with random app and slot */
            pod_app_t a = gen_app_or_none();
            int idx = prng_int(0, POD_GRID_SLOTS);
            pod_layout_grid_set(a, idx);
        } else if (op == 1) {
            /* dock_set */
            pod_app_t a = gen_app_or_none();
            int idx = prng_int(0, POD_DOCK_SLOTS);
            pod_layout_dock_set(a, idx);
        } else if (op == 2) {
            /* set_visible false */
            pod_app_t a = gen_app();
            pod_layout_set_visible(a, false);
        } else {
            /* set_visible true */
            pod_app_t a = gen_app();
            pod_layout_set_visible(a, true);
        }

        if (!check_uniqueness()) {
            failures++;
            if (failures <= 3) printf("    iter %d: uniqueness violated\n", iter);
        }
    }
    CHECK(failures == 0, "Property 2: uniqueness holds after every mutation");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 5: Shortcut assignment covers all slots
 * Feature: ipod-classic-ui, Property 5: Shortcut assignment covers all slots
 * Validates: Requirements 3.1, 3.2
 */
static void prop_5_assignment_all_slots(void) {
    printf("\n[Property 5] Assignment covers all slots (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        pod_layout_default();

        /* Pick a random valid app and a random slot. */
        pod_app_t a = gen_app();
        int area = prng_int(0, 2); /* 0 = grid, 1 = dock */
        if (area == 0) {
            int idx = prng_int(0, POD_GRID_SLOTS);
            bool ok = pod_layout_grid_set(a, idx);
            if (!ok || pod_layout_grid_at(idx) != a) {
                failures++;
                if (failures <= 3)
                    printf("    iter %d: grid_set(%d,%d) failed or not reflected\n", iter, (int)a, idx);
            }
        } else {
            int idx = prng_int(0, POD_DOCK_SLOTS);
            bool ok = pod_layout_dock_set(a, idx);
            if (!ok || pod_layout_dock_at(idx) != a) {
                failures++;
                if (failures <= 3)
                    printf("    iter %d: dock_set(%d,%d) failed or not reflected\n", iter, (int)a, idx);
            }
        }
    }
    CHECK(failures == 0, "Property 5: set succeeds and getter reflects it for all valid inputs");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 6: Move updates both source and destination
 * Feature: ipod-classic-ui, Property 6: Move updates both source and destination
 * Validates: Requirements 3.4
 *
 * "Move" is modelled as grid_set(a, dst); the source slot that held `a` before
 * must become POD_APP_NONE.
 */
static void prop_6_move_source_and_dest(void) {
    printf("\n[Property 6] Move updates source and destination (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        /* Start from default and place one random app. */
        for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
        for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);

        pod_app_t a = gen_app();
        int src = prng_int(0, POD_GRID_SLOTS);
        pod_layout_grid_set(a, src);
        if (pod_layout_grid_at(src) != a) continue; /* should not happen */

        int dst = prng_int(0, POD_GRID_SLOTS);
        while (dst == src) dst = prng_int(0, POD_GRID_SLOTS);

        /* Move: set a at dst. */
        pod_layout_grid_set(a, dst);

        bool dest_ok   = (pod_layout_grid_at(dst) == a);
        bool source_ok = (pod_layout_grid_at(src) == POD_APP_NONE);

        if (!dest_ok || !source_ok) {
            failures++;
            if (failures <= 3)
                printf("    iter %d: a=%d src=%d dst=%d: dest_ok=%d source_ok=%d\n",
                       iter, (int)a, src, dst, dest_ok, source_ok);
        }
    }
    CHECK(failures == 0, "Property 6: source cleared and destination holds app after move");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 7: Disable toggle removes shortcut from layout
 * Feature: ipod-classic-ui, Property 7: Disable toggle removes shortcut from layout
 * Validates: Requirements 4.2, 4.3
 */
static void prop_7_disable_removes_shortcut(void) {
    printf("\n[Property 7] Disable removes shortcut (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
        for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);

        /* Place a random app in a random slot (grid or dock). */
        pod_app_t a = gen_app();
        int area = prng_int(0, 2);
        if (area == 0) {
            int idx = prng_int(0, POD_GRID_SLOTS);
            pod_layout_grid_set(a, idx);
        } else {
            int idx = prng_int(0, POD_DOCK_SLOTS);
            pod_layout_dock_set(a, idx);
        }

        pod_layout_set_visible(a, true); /* ensure it starts visible */
        pod_layout_set_visible(a, false);

        /* After disabling: visible flag must be false and app must not appear in any slot. */
        bool vis_ok = !pod_layout_visible(a);
        bool absent = true;
        for (int i = 0; i < POD_GRID_SLOTS; i++) {
            if (pod_layout_grid_at(i) == a) { absent = false; break; }
        }
        for (int i = 0; i < POD_DOCK_SLOTS; i++) {
            if (pod_layout_dock_at(i) == a) { absent = false; break; }
        }

        if (!vis_ok || !absent) {
            failures++;
            if (failures <= 3)
                printf("    iter %d: a=%d vis_ok=%d absent=%d\n", iter, (int)a, vis_ok, absent);
        }
    }
    CHECK(failures == 0, "Property 7: disabled app is invisible and not in any slot");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 8: Re-enable restores to remembered slot
 * Feature: ipod-classic-ui, Property 8: Re-enable restores to remembered slot
 * Validates: Requirements 4.4
 */
static void prop_8_reenable_remembered_slot(void) {
    printf("\n[Property 8] Re-enable restores to remembered slot (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
        for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);

        pod_app_t a = gen_app();
        int src_idx = prng_int(0, POD_GRID_SLOTS);
        pod_layout_grid_set(a, src_idx);

        /* Hide it: remembered slot = src_idx. */
        pod_layout_set_visible(a, false);

        /* Remembered slot is currently empty; re-enabling should restore to it. */
        pod_layout_set_visible(a, true);

        bool restored = (pod_layout_grid_at(src_idx) == a);
        if (!restored) {
            failures++;
            if (failures <= 3)
                printf("    iter %d: a=%d src_idx=%d, got grid[%d]=%d\n",
                       iter, (int)a, src_idx, src_idx, (int)pod_layout_grid_at(src_idx));
        }
    }
    CHECK(failures == 0, "Property 8: re-enabled app restores to remembered slot when empty");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 9: Re-enable with no remembered slot uses first empty grid slot
 * Feature: ipod-classic-ui, Property 9: Re-enable with no remembered slot uses first empty grid slot
 * Validates: Requirements 4.5
 */
static void prop_9_reenable_no_remembered_uses_first_empty(void) {
    printf("\n[Property 9] Re-enable (no remembered slot) → first empty (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        /* Start with all slots empty and all apps hidden with no remembered slot.
           We achieve this by hiding all apps while they are unplaced, so remembered=-1. */
        pod_layout_default();
        /* First, clear all slots without hiding (so apps get remembered = -1 on hide). */
        for (int i = 0; i < POD_GRID_SLOTS; i++) pod_layout_grid_set(POD_APP_NONE, i);
        for (int i = 0; i < POD_DOCK_SLOTS; i++) pod_layout_dock_set(POD_APP_NONE, i);
        /* Now hide all apps while unplaced → remembered[all] = -1. */
        for (pod_app_t x = 0; x < POD_APP_COUNT; x++) {
            pod_layout_set_visible(x, false);
        }

        /* Shuffle app order. */
        pod_app_t available[POD_APP_COUNT];
        int nav = POD_APP_COUNT;
        for (int i = 0; i < nav; i++) available[i] = (pod_app_t)i;
        for (int i = nav - 1; i > 0; i--) {
            int j = prng_int(0, i + 1);
            pod_app_t tmp = available[i]; available[i] = available[j]; available[j] = tmp;
        }

        /* Fill some prefix of grid slots by directly writing (bypassing set_visible). */
        int filled = prng_int(0, POD_GRID_SLOTS); /* 0..8 occupied slots */
        for (int i = 0; i < filled; i++) {
            pod_layout_grid_set(available[i], i);
            /* Mark them visible too so they aren't re-placed by set_visible. */
        }

        /* Choose the app that will be re-enabled — the one right after the filled ones. */
        if (filled >= nav) continue; /* not enough apps; skip */
        pod_app_t chosen = available[filled];
        int expected_slot = filled; /* first empty slot */

        /* Re-enable chosen (remembered=-1, all prior slots occupied). */
        pod_layout_set_visible(chosen, true);

        bool landed = (pod_layout_grid_at(expected_slot) == chosen);
        if (!landed) {
            failures++;
            if (failures <= 3)
                printf("    iter %d: chosen=%d filled=%d expected grid[%d]=%d, got %d\n",
                       iter, (int)chosen, filled, expected_slot,
                       (int)chosen, (int)pod_layout_grid_at(expected_slot));
        }
    }
    CHECK(failures == 0, "Property 9: re-enable with no remembered slot uses first empty grid slot");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/*
 * Property 11: Theme configurations are isolated
 * Feature: ipod-classic-ui, Property 11: Theme configurations are isolated
 * Validates: Requirements 6.1, 6.2, 6.3, 6.4
 *
 * After any pod_layout_* write, no [menu] key must be touched.
 * We simulate this by pre-populating a fake [menu] key and verifying it
 * survives pod_layout_save().
 */
static void prop_11_theme_isolation(void) {
    printf("\n[Property 11] Theme isolation (%d iterations)\n", PROP_ITERS);
    int failures = 0;

    for (int iter = 0; iter < PROP_ITERS; iter++) {
        _cfg_clear();
        /* Seed a [menu] key that must not be touched. */
        config_set_int("menu", "tile_0", 42L + iter);
        config_set_int("menu", "tile_1", 7L);
        config_set_int("ui",   "home_theme", 0L);

        gen_layout_state();
        pod_layout_save();

        /* Verify the [menu] keys are unchanged. */
        long t0 = config_get_int("menu", "tile_0", -1L);
        long t1 = config_get_int("menu", "tile_1", -1L);
        long ht = config_get_int("ui",   "home_theme", -1L);

        if (t0 != 42L + iter || t1 != 7L || ht != 0L) {
            failures++;
            if (failures <= 3)
                printf("    iter %d: menu/tile_0=%ld (expected %ld), tile_1=%ld (expected 7), home_theme=%ld (expected 0)\n",
                       iter, t0, 42L + iter, t1, ht);
        }
    }
    CHECK(failures == 0, "Property 11: pod_layout_save() does not touch [menu] or other sections");
    if (failures > 0) printf("    (%d / %d iterations failed)\n", failures, PROP_ITERS);
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

int main(void) {
    prng_seed((uint64_t)time(NULL) ^ 0xdeadbeefULL);

    printf("=== pod_layout unit + property tests ===\n");

    /* ---- Unit tests ---- */
    test_unit_default_layout();
    test_unit_init_empty_config();
    test_unit_out_of_range_returns_false();
    test_unit_reenable_occupied_remembered_uses_first_free();
    test_unit_duplicate_during_load();
    test_unit_label_icon_accessors();

    /* ---- Property tests ---- */
    prop_1_persistence_round_trip();
    prop_2_slot_uniqueness();
    prop_5_assignment_all_slots();
    prop_6_move_source_and_dest();
    prop_7_disable_removes_shortcut();
    prop_8_reenable_remembered_slot();
    prop_9_reenable_no_remembered_uses_first_empty();
    prop_11_theme_isolation();

    printf("\n=== Results: %d passed, %d failed ===\n", _pass, _fail);
    return _fail > 0 ? 1 : 0;
}
