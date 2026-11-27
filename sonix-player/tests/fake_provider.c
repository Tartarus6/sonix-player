#include "fake_provider.h"

#include <stdio.h>
#include <string.h>

fake_provider_t fake;

typedef struct {
	const char *name;
	const char *key;
} key_row_t;

typedef struct {
	const char *name;
	const char *path;
	const char *artist;
	const char *album_key;
} track_row_t;

static const key_row_t ARTISTS[] = {
	{"Artist One", "Artist One"},
	{"Artist Two", FAKE_ARTIST2},
};

static const key_row_t ALBUMS[] = {
	{"Album A", "Album A\x1f" "A"},
	{"Album B", FAKE_ALBUM_B},
	{"Album C", "Album C\x1f" "C"},
};

static const char *const ALBUM_ARTIST[] = {"Artist One", FAKE_ARTIST2, FAKE_ARTIST2};

static const track_row_t TRACKS[] = {
	{"One Song", FAKE_TRACK0, "Artist One", "Album A\x1f" "A"},
	{"Two Song", FAKE_TRACK1, FAKE_ARTIST2, FAKE_ALBUM_B},
	{"Three Song", "/sdcard/music/Three Song.mp3", FAKE_ARTIST2, FAKE_ALBUM_B},
	{"Four Song", "/sdcard/music/Four Song.mp3", FAKE_ARTIST2, "Album C\x1f" "C"},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void put(browse_item_t *out, int at, const char *name, const char *value, const char *artist) {
	snprintf(out[at].name, sizeof(out[at].name), "%s", name);
	snprintf(out[at].value, sizeof(out[at].value), "%s", value);
	snprintf(out[at].artist, sizeof(out[at].artist), "%s", artist ? artist : "");
}

static int fake_refresh(void *user) {
	(void)user;
	fake.refresh_calls++;
	return (int)COUNT(TRACKS);
}

static int fake_window(void *user, int offset, int count, browse_item_t *out) {
	(void)user;
	fake.window_calls++;
	if (fake.stale_once > 0) {
		fake.stale_once--;
		return -1;
	}
	if (!out || count <= 0 || offset < 0 || (size_t)offset >= COUNT(TRACKS)) {
		return 0;
	}
	int n = 0;
	for (size_t i = (size_t)offset; i < COUNT(TRACKS) && n < count; i++, n++) {
		put(out, n, TRACKS[i].name, TRACKS[i].path, TRACKS[i].artist);
	}
	return n;
}

static void fake_close(void *user) {
	(void)user;
	fake.close_calls++;
}

static void fake_play(const char *path, void *user) {
	(void)user;
	fake.play_calls++;
	snprintf(fake.last_played, sizeof(fake.last_played), "%s", path ? path : "");
}

static int fake_list(void *user, browse_kind_t kind, browse_filter_t filter, const char *value, int offset,
					 int count, browse_item_t *out, int *total) {
	(void)user;
	fake.list_calls++;

	browse_item_t selected[8];
	int n = 0;
	if (kind == BROWSE_KIND_ARTISTS) {
		for (size_t i = 0; i < COUNT(ARTISTS); i++) {
			put(selected, n++, ARTISTS[i].name, ARTISTS[i].key, "");
		}
	} else if (kind == BROWSE_KIND_ALBUMS && filter == BROWSE_FILTER_NONE) {
		for (size_t i = 0; i < COUNT(ALBUMS); i++) {
			put(selected, n++, ALBUMS[i].name, ALBUMS[i].key, ALBUM_ARTIST[i]);
		}
	} else if (kind == BROWSE_KIND_ALBUMS && filter == BROWSE_FILTER_ARTIST) {
		for (size_t i = 0; i < COUNT(ALBUMS); i++) {
			if (strcmp(ALBUM_ARTIST[i], value ? value : "") == 0) {
				put(selected, n++, ALBUMS[i].name, ALBUMS[i].key, ALBUM_ARTIST[i]);
			}
		}
	} else if (kind == BROWSE_KIND_TRACKS && filter == BROWSE_FILTER_ALBUM) {
		for (size_t i = 0; i < COUNT(TRACKS); i++) {
			if (strcmp(TRACKS[i].album_key, value ? value : "") == 0) {
				put(selected, n++, TRACKS[i].name, TRACKS[i].path, TRACKS[i].artist);
			}
		}
	}

	if (total) {
		*total = n;
	}
	if (!out || count <= 0 || offset < 0 || offset >= n) {
		return 0;
	}
	int written = 0;
	for (int i = offset; i < n && written < count; i++, written++) {
		out[written] = selected[i];
	}
	return written;
}

const bt_browser_provider_t fake_bt_provider = {
	.user = NULL,
	.refresh = fake_refresh,
	.window = fake_window,
	.close = fake_close,
	.play = fake_play,
	.list = fake_list,
};

void fake_provider_reset(void) { memset(&fake, 0, sizeof(fake)); }
