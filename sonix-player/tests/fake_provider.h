#ifndef FAKE_PROVIDER_H
#define FAKE_PROVIDER_H

#include "src/system/bluetooth/btbrowser.h"

typedef struct {
	int refresh_calls;
	int window_calls;
	int list_calls;
	int close_calls;
	int play_calls;
	int stale_once; // make the next window() answer "stale" once
	char last_played[512];
} fake_provider_t;

extern fake_provider_t fake;
extern const bt_browser_provider_t fake_bt_provider;

void fake_provider_reset(void);

// Paths used by the harness to find known rows again.
#define FAKE_TRACK0 "/sdcard/music/One Song.mp3"
#define FAKE_TRACK1 "/sdcard/music/Two Song.mp3"
#define FAKE_ARTIST2 "Artist Two\x1f" "2"
#define FAKE_ALBUM_B "Album B\x1f" "B"

#endif /* FAKE_PROVIDER_H */
