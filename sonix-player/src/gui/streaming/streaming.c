#include "streaming.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/streaming/jellyfinpage.h"
#include "src/gui/streaming/networkmusicpage.h"
#include "src/gui/streaming/podcastsaved.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/radiopage.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/streaming/soundcloudpage.h"
#include "src/gui/streaming/subsonicpage.h"
#include "src/gui/streaming/tidalpage.h"

lv_obj_t *streaming_screen;

// The shell switches to the main menu after this function returns, so the
// preview hook loads the requested page one tick later.
static void v1_open_screen_cb(lv_timer_t *timer) {
	lv_obj_t *screen = (lv_obj_t *)lv_timer_get_user_data(timer);
	lv_timer_delete(timer);
	if (screen) {
		switch_screen(screen);
	}
}

void streaming_disable_preview_action(lv_obj_t *button, lv_obj_t *label) {
	// The LAN actions are real in this build; the V1 preview-only state is
	// never reached, so there is nothing to grey out.
	(void)button;
	(void)label;
}

// The V1-fork streaming pages are plugin candidates, not part of the default
// three-device build's menu: set SONIX_V1_STREAMING=1 to build and expose their
// tiles (used by the host-simulator visual checks), optionally with
// SONIX_V1_STREAMING_OPEN=jellyfin|soundcloud|subsonic|network|saved to land on
// one of them. Without the variable the stock four tiles are unchanged.
static bool v1_streaming_preview(void) { return getenv("SONIX_V1_STREAMING") != NULL; }

void streaming_init(gui_config_t *cfg) {
	qobuzpage_init(cfg);
	tidalpage_init(cfg);
	podcastpage_init(cfg);

	grid_entry_t entries[9] = {
		{"tidal", &icon_menu_tidal, &tidal_screen, NULL},
		{"qobuz", &icon_menu_qobuz, &qobuz_screen, NULL},
		{"radio", &icon_menu_radio, &radiopage_screen, NULL},
		{"podcasts", &icon_menu_podcast, &podcast_screen, NULL},
	};
	int count = 4;
	bool preview = v1_streaming_preview();
	if (preview) {
		jellyfinpage_init(cfg);
		soundcloudpage_init(cfg);
		subsonicpage_init(cfg);
		networkmusic_page_init(cfg);
		podcastsaved_init(cfg);
		entries[count++] = (grid_entry_t){"jellyfin", &icon_menu_streaming, &jellyfin_screen, NULL};
		entries[count++] = (grid_entry_t){"soundcloud", &icon_music_note, &soundcloud_screen, NULL};
		entries[count++] = (grid_entry_t){"subsonic", &icon_menu_settings, &subsonic_screen, NULL};
		entries[count++] = (grid_entry_t){"Network folders", &icon_folder, &networkmusic_screen, NULL};
		entries[count++] = (grid_entry_t){"downloaded", &icon_podcast_list, &podcastsaved_screen, NULL};
		printf("streaming: V1 plugin preview enabled, %d tiles\n", count);
	}

	// Two columns and three rows of tile, exactly like Music and Wireless, so a
	// tile here is the same size as a tile there. Four of them fill the first
	// two rows; the third stays empty rather than the tiles growing to fill it.
	lv_obj_t *grid = gridpage_build(streaming_screen, cfg, entries, count, 2, 3, true);
	if (preview && count > 6) {
		gridpage_set_scrolling(grid);
	}

	settingsrow_title(streaming_screen, cfg, "streaming");

	const char *open = getenv("SONIX_V1_STREAMING_OPEN");
	if (!open) {
		return;
	}
	lv_obj_t *target = NULL;
	if (!strcmp(open, "jellyfin")) {
		target = jellyfin_screen;
	} else if (!strcmp(open, "soundcloud")) {
		target = soundcloud_screen;
	} else if (!strcmp(open, "subsonic")) {
		target = subsonic_screen;
	} else if (!strcmp(open, "network")) {
		target = networkmusic_screen;
	} else if (!strcmp(open, "saved")) {
		target = podcastsaved_screen;
	}
	if (target) {
		lv_timer_create(v1_open_screen_cb, 1500, target);
	}
}
