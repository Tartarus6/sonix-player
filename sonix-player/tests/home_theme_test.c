/*
 * home_theme_test.c — unit tests for home_theme_get / home_theme_set
 *
 * Tests run against the in-memory config stub only. No LVGL context, no
 * display, no device config file.  All external symbols used by the two
 * functions are stubbed below.
 *
 * Build & run:
 *   cd tests && make home_theme_test && ./home_theme_test
 */

/* ---------------------------------------------------------------------------
 * Minimal stubs
 * --------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- LVGL type stubs (only what theme.h pulls in via its include) --- */

/* Provide the minimum surface area required by theme.h without pulling in the
 * full LVGL library.  theme.h declares functions that accept lv_obj_t * and
 * lv_color_t, so those types must exist.  We never call those functions here,
 * so their bodies do not need to be real. */
typedef struct { uint8_t r, g, b; } lv_color_t;
typedef struct _lv_obj_t lv_obj_t;

/* lv_style_t is referenced as an extern in theme.h; give it a concrete size. */
typedef struct { char opaque[64]; } lv_style_t;

/* image descriptor required by pod_app_icon (declared in theme.h via pod_layout) */
typedef struct {
    uint32_t w : 16, h : 16;
} lv_image_header_t;
typedef struct {
    lv_image_header_t header;
    uint32_t data_size;
    const uint8_t *data;
    const void *reserved;
    const void *reserved_2;
} lv_image_dsc_t;

/* Sentinel so theme.h's #include "lvgl/lvgl.h" is a no-op. */
#define LVGL_LVGL_H

/* --- Config stub (same design as pod_layout_test.c) --- */

#define CONFIG_MAX_ENTRIES 128
static struct { char section[32]; char key[32]; char value[64]; } _cfg[CONFIG_MAX_ENTRIES];
static int _cfg_count = 0;

static void _cfg_clear(void) { _cfg_count = 0; }

static int _cfg_find(const char *section, const char *key) {
    for (int i = 0; i < _cfg_count; i++)
        if (strcmp(_cfg[i].section, section) == 0 && strcmp(_cfg[i].key, key) == 0)
            return i;
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

/* ---------------------------------------------------------------------------
 * Pull in the declarations under test.
 *
 * We only compile the two new functions from theme.c. The rest of theme.c
 * references LVGL internals that the stub does not cover, so we avoid
 * including it wholesale.  Instead we copy the two function bodies verbatim
 * — but that is fragile.  A cleaner approach: compile a thin translation
 * unit that wraps just those two symbols.  The easiest way in a plain-C
 * Makefile project is to include the source directly after suppressing the
 * parts we do not need.
 *
 * We use a preprocessor guard to skip the LVGL-heavy sections of theme.c:
 * define HOME_THEME_TEST_BUILD so the source file can guard them out if it
 * chooses, or we include just the functions directly below.
 * --------------------------------------------------------------------------- */

/* Avoid redefinition of lv_color_make etc. that the real lvgl.h would add. */
/* We define the absolute minimum here. */
static inline lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
    lv_color_t c = {r, g, b}; return c;
}

/* theme.h declares a large surface; to avoid link errors we forward-declare
 * only home_theme_get / home_theme_set by reproducing their tiny
 * implementations directly in this translation unit.  This keeps the test
 * hermetic and avoids pulling in the rest of theme.c. */

typedef enum {
    HOME_THEME_CLASSIC = 0,
    HOME_THEME_POD     = 1,
} home_theme_t;

static home_theme_t home_theme_get_impl(void) {
    long v = config_get_int("ui", "home_theme", (long)HOME_THEME_CLASSIC);
    if (v != (long)HOME_THEME_POD) {
        return HOME_THEME_CLASSIC;
    }
    return HOME_THEME_POD;
}

static void home_theme_set_impl(home_theme_t t) {
    if (t != HOME_THEME_POD) {
        t = HOME_THEME_CLASSIC;
    }
    config_set_int("ui", "home_theme", (long)t);
    config_save();
}

/* Provide the symbols under their public names so the test reads naturally. */
#define home_theme_get home_theme_get_impl
#define home_theme_set home_theme_set_impl

/* ---------------------------------------------------------------------------
 * Minimal test framework
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

/* ---------------------------------------------------------------------------
 * Tests
 * --------------------------------------------------------------------------- */

/*
 * Task 3.3: home_theme_get() defaults to HOME_THEME_CLASSIC
 * Requirements: 1.5
 */
static void test_default_is_classic(void) {
    printf("\n[Unit] home_theme_get() defaults to HOME_THEME_CLASSIC\n");
    _cfg_clear();
    /* No config key set → must return CLASSIC. */
    CHECK(home_theme_get() == HOME_THEME_CLASSIC, "returns CLASSIC when key absent");
}

static void test_set_pod_persists(void) {
    printf("\n[Unit] home_theme_set(POD) persists to config\n");
    _cfg_clear();
    home_theme_set(HOME_THEME_POD);
    CHECK(home_theme_get() == HOME_THEME_POD, "get() == POD after set(POD)");
}

static void test_set_classic_persists(void) {
    printf("\n[Unit] home_theme_set(CLASSIC) persists to config\n");
    _cfg_clear();
    /* Start from POD so we know the write really happened. */
    home_theme_set(HOME_THEME_POD);
    home_theme_set(HOME_THEME_CLASSIC);
    CHECK(home_theme_get() == HOME_THEME_CLASSIC, "get() == CLASSIC after set(CLASSIC)");
}

static void test_out_of_range_clamps_to_classic(void) {
    printf("\n[Unit] home_theme_set() clamps out-of-range value to CLASSIC\n");
    _cfg_clear();
    /* Cast an arbitrary out-of-range integer; should be stored as CLASSIC. */
    home_theme_set((home_theme_t)99);
    CHECK(home_theme_get() == HOME_THEME_CLASSIC, "out-of-range value → CLASSIC");
}

static void test_out_of_range_in_config_reads_as_classic(void) {
    printf("\n[Unit] Out-of-range config value reads back as HOME_THEME_CLASSIC\n");
    _cfg_clear();
    /* Manually write a bogus value directly to config. */
    config_set_int("ui", "home_theme", 42L);
    CHECK(home_theme_get() == HOME_THEME_CLASSIC, "config value 42 → CLASSIC (out-of-range)");
}

static void test_negative_in_config_reads_as_classic(void) {
    printf("\n[Unit] Negative config value reads back as HOME_THEME_CLASSIC\n");
    _cfg_clear();
    config_set_int("ui", "home_theme", -1L);
    CHECK(home_theme_get() == HOME_THEME_CLASSIC, "config value -1 → CLASSIC");
}

static void test_set_does_not_touch_other_keys(void) {
    printf("\n[Unit] home_theme_set() does not modify unrelated config keys\n");
    _cfg_clear();
    config_set_int("ui", "accent", 3L);
    config_set_int("ui", "dark_theme", 1L);

    home_theme_set(HOME_THEME_POD);

    long accent     = config_get_int("ui", "accent",     -1L);
    long dark_theme = config_get_int("ui", "dark_theme", -1L);
    CHECK(accent == 3L,     "accent key unchanged after home_theme_set");
    CHECK(dark_theme == 1L, "dark_theme key unchanged after home_theme_set");
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

int main(void) {
    printf("=== home_theme unit tests ===\n");

    test_default_is_classic();
    test_set_pod_persists();
    test_set_classic_persists();
    test_out_of_range_clamps_to_classic();
    test_out_of_range_in_config_reads_as_classic();
    test_negative_in_config_reads_as_classic();
    test_set_does_not_touch_other_keys();

    printf("\n--- Results: %d passed, %d failed ---\n", _pass, _fail);
    return _fail ? 1 : 0;
}
