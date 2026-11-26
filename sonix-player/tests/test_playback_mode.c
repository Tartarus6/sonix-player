// Host-side regression test for the playback-mode state machine and automatic
// queue advance. The real playlist.c and device_state.c are linked in; the
// library, audio, audiobook and config layers they normally talk to are
// stubbed below, so this needs neither LVGL nor the host audio libraries.
//
// The case that matters here: "stop after current track" is a music mode. It
// must stop a music queue, but must not stop an audiobook's folder queue
// between parts, nor an ordered (podcast) queue between episodes.

#include "src/system/playback/playlist.h"

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/library/cue.h"
#include "src/system/library/folderorder.h"
#include "src/system/library/library.h"
#include "src/system/library/metadata.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/readahead.h"
#include "src/system/audio/replaygain.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playhistory.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/radio.h"
#include "src/system/streaming/tidalcache.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Stubs for the layers playlist.c talks to. None of them is reached by the
// queues built below (explicit path lists, no library handle, no cue sheets),
// but they have to exist for the link.
// ---------------------------------------------------------------------------

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

bool cue_is_sheet(const char *path) {
	(void)path;
	return false;
}

bool cue_parse(const char *cue_path, cue_sheet_t *out) {
	(void)cue_path;
	(void)out;
	return false;
}

void cue_virtual_path(const char *cue_path, int track_number, char *out, size_t size) {
	(void)cue_path;
	(void)track_number;
	if (size) {
		out[0] = '\0';
	}
}

bool cue_order_tracks(const char *a, const char *b, int *out) {
	(void)a;
	(void)b;
	(void)out;
	return false;
}

void folderorder_places(const char *folder, const char *const *names, size_t count, bool read_tags,
						folder_place_t *out) {
	(void)folder;
	(void)names;
	(void)read_tags;
	memset(out, 0, count * sizeof(*out));
}

int folderorder_cmp(const folder_place_t *a, const folder_place_t *b) {
	(void)a;
	(void)b;
	return 0;
}

bool has_extension(const char *name, const char *ext) {
	size_t n = strlen(name), e = strlen(ext);
	return n >= e && strcasecmp(name + n - e, ext) == 0;
}

library_index_t *library_index_open(library_list_t kind, library_filter_t filter, const char *value,
									library_order_t order, bool desc) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)desc;
	return NULL;
}

void library_index_close(library_index_t *ix) { (void)ix; }
int library_index_count(const library_index_t *ix) { (void)ix; return 0; }
bool library_index_stale(const library_index_t *ix) { (void)ix; return false; }

int library_index_window(const library_index_t *ix, int offset, int count, library_row_cb cb, void *user) {
	(void)ix;
	(void)offset;
	(void)count;
	(void)cb;
	(void)user;
	return 0;
}

void library_index_describe(const library_index_t *ix, library_index_spec_t *out) {
	(void)ix;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
}

int library_index_find_path(const library_index_t *ix, const char *path) {
	(void)ix;
	(void)path;
	return -1;
}

void library_queue_save_album(const char *album) { (void)album; }

// ---------------------------------------------------------------------------
// Stubs for device_state.c's other dependencies. The knobs below stand in for
// the audiobook/audio state a real device would report, and the counters let
// the test tell "stopped" from "loaded the next track".
// ---------------------------------------------------------------------------

static const char *tracks[] = {"/card/book/part1.mp3", "/card/book/part2.mp3", "/card/book/part3.mp3"};

static bool stub_audiobook_playing;
static bool stub_audiobook_has_next_part;
static int playhistory_left_calls;
static int readahead_calls;
static char readahead_path[512];
static int audio_play_calls;
static char audio_play_path[512];

static void reset_observers(void) {
	playhistory_left_calls = 0;
	readahead_calls = 0;
	readahead_path[0] = '\0';
	audio_play_calls = 0;
	audio_play_path[0] = '\0';
}

// Loads the first track through the real device-state entry point, so the
// layer's own "currently loaded file" starts at the same entry as the queue
// in every case. Set the audiobook knobs before calling this: loading a track
// consults them.
static void reset_device(bool ordered) {
	if (ordered) {
		device_state_play_list_ordered(tracks, 3, 0);
	} else {
		device_state_play_list(tracks, 3, 0);
	}
	reset_observers();
}

bool audiobook_is_playing(void) { return stub_audiobook_playing; }
bool audiobook_has_next_part(void) { return stub_audiobook_has_next_part; }
bool audiobook_stop_at_chapter_end(void) { return false; }
double audiobook_speed(void) { return 1.0; }
void audiobook_suppress_rewind_once(void) {}
void audiobook_track_changed(const char *filepath) { (void)filepath; }

bool audio_get_gapless(void) { return true; }
void audio_set_speed(double factor) { (void)factor; }

static int record_audio_play(const char *path) {
	audio_play_calls++;
	snprintf(audio_play_path, sizeof(audio_play_path), "%s", path);
	return 0;
}
int audio_play(const char *filepath) { return record_audio_play(filepath); }
int audio_play_at(const char *filepath, double start_secs) {
	(void)start_secs;
	return record_audio_play(filepath);
}
int audio_play_paused(const char *filepath, double start_secs) {
	(void)start_secs;
	return record_audio_play(filepath);
}

void playhistory_left(void) { playhistory_left_calls++; }
void playhistory_started(const char *path, const char *title, const char *artist, bool kept_out, bool waiting) {
	(void)path;
	(void)title;
	(void)artist;
	(void)kept_out;
	(void)waiting;
}

void readahead_file(const char *path) {
	readahead_calls++;
	snprintf(readahead_path, sizeof(readahead_path), "%s", path);
}
void replaygain_load(const song_metadata_t *meta) { (void)meta; }
void metadata_read(const char *filepath, song_metadata_t *out) {
	(void)filepath;
	memset(out, 0, sizeof(*out));
}
const char *metadata_shown_artist(const song_metadata_t *m) {
	(void)m;
	return "";
}

bool radio_is_active(void) { return false; }
void radio_clear(void) {}
bool dlna_owns_path(const char *path) { (void)path; return false; }
bool growfile_is_growing(const char *path) { (void)path; return false; }
double podcast_speed(void) { return 1.0; }
bool podcastcache_is_episode(const char *path) { (void)path; return false; }
double podcaststate_resume_point(const char *path) { (void)path; return 0.0; }
bool qobuzcache_owns(const char *path) { (void)path; return false; }
bool tidalcache_owns(const char *path) { (void)path; return false; }

int library_for_each(library_list_t kind, library_filter_t filter, const char *value, library_row_cb cb, void *user) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)cb;
	(void)user;
	return 0;
}
int library_for_each_ordered(library_list_t kind, library_filter_t filter, const char *value, library_order_t order,
							 library_row_cb cb, void *user) {
	(void)kind;
	(void)filter;
	(void)value;
	(void)order;
	(void)cb;
	(void)user;
	return 0;
}
bool library_album_same(const char *a, const char *b) {
	(void)a;
	(void)b;
	return false;
}
void library_album_title(const char *value, char *out, size_t size) {
	(void)value;
	if (size) {
		out[0] = '\0';
	}
}
void library_queue_save(const char *const *paths, int count, int current_index, bool custom) {
	(void)paths;
	(void)count;
	(void)current_index;
	(void)custom;
}
void library_queue_save_index(int current_index) { (void)current_index; }
void library_queue_save_query(const library_index_spec_t *spec, int position, const char *const *extra,
							  const int *extra_slots, int extra_count) {
	(void)spec;
	(void)position;
	(void)extra;
	(void)extra_slots;
	(void)extra_count;
}

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------

static int failures;

#define CHECK(cond, msg)                                                                    \
	do {                                                                                    \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg));              \
			failures++;                                                                  \
		}                                                                                   \
	} while (0)

static void test_cycle(void) {
	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	CHECK(playlist_cycle_mode() == PLAYBACK_MODE_STOP_AFTER, "cycle NORMAL -> STOP_AFTER");
	CHECK(playlist_cycle_mode() == PLAYBACK_MODE_REPEAT_ALL, "cycle STOP_AFTER -> REPEAT_ALL");
}

static void test_stop_after_stops_music(void) {
	char next[512] = {0};

	stub_audiobook_playing = false;
	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	reset_device(false);
	CHECK(!playlist_is_ordered(), "a music queue is not ordered");
	CHECK(device_state_stop_after_music(), "stop-after applies to music");

	// The guard lives in device_state_advance_auto(), not in playlist.c.
	CHECK(!device_state_advance_auto(next, sizeof(next)), "stop-after stops a music queue");
	CHECK(playhistory_left_calls == 1, "the finished track is written to play history");
	CHECK(audio_play_calls == 0, "no next music track is loaded");
	CHECK(next[0] == '\0', "no next path is reported");
}

static void test_stop_after_leaves_audiobook_queue_alone(void) {
	char next[512];

	stub_audiobook_playing = true;
	stub_audiobook_has_next_part = true;
	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	// A folder queue (the shape a multi-part audiobook uses) is an ordinary,
	// non-ordered queue. The mode is global, so this has to keep working.
	reset_device(false);
	CHECK(!playlist_is_ordered(), "a folder-style queue is not ordered");
	CHECK(!device_state_stop_after_music(), "stop-after does not apply to an audiobook");

	// device_state_advance_auto() reaches the audiobook branch before the
	// music-only stop-after guard, so the next part really loads.
	CHECK(device_state_advance_auto(next, sizeof(next)), "audiobook advances in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "advance names part 2");
	CHECK(audio_play_calls == 1, "the next part is loaded");
	CHECK(strcmp(audio_play_path, tracks[1]) == 0, "the next part is played");

	// The playlist layer itself must not block the parts either.
	CHECK(playlist_peek_auto(next, sizeof(next)), "peek of next audiobook part in stop-after mode");
	CHECK(strcmp(next, tracks[2]) == 0, "peek names part 3");
	CHECK(playlist_advance_auto(next, sizeof(next)), "advance to last audiobook part");
	CHECK(strcmp(next, tracks[2]) == 0, "advance names part 3");
	CHECK(!playlist_advance_auto(next, sizeof(next)), "the queue ends after its last part");
}

static void test_ordered_queue_advances_in_stop_after(void) {
	char next[512];

	stub_audiobook_playing = false;
	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	reset_device(true);
	CHECK(playlist_is_ordered(), "podcast queue carries its own order");
	CHECK(!device_state_stop_after_music(), "stop-after does not apply to an ordered queue");
	CHECK(device_state_advance_auto(next, sizeof(next)), "ordered queue advances in stop-after mode");
	CHECK(strcmp(next, tracks[1]) == 0, "ordered advance names episode 2");
	CHECK(audio_play_calls == 1, "the next episode is loaded");
}

static void test_read_ahead_and_cover_rule(void) {
	stub_audiobook_playing = false;
	playlist_set_mode(PLAYBACK_MODE_STOP_AFTER);
	reset_device(false);

	// Gapless is on and the track is near its end, so only the stop-after
	// guard can keep the next file (and its cover) from being fetched.
	device_state_read_ahead(19.0, 20.0);
	CHECK(readahead_calls == 0, "stop-after does not read ahead into music");

	// An audiobook keeps read-ahead, and the cover prefetch follows the same
	// rule via device_state_stop_after_music().
	stub_audiobook_playing = true;
	stub_audiobook_has_next_part = true;
	device_state_read_ahead(19.0, 20.0);
	CHECK(readahead_calls == 1, "an audiobook still reads ahead in stop-after mode");
	CHECK(strcmp(readahead_path, tracks[1]) == 0, "read-ahead names part 2");
}

static void test_normal_mode_stops_at_end(void) {
	char next[512];

	playlist_set_mode(PLAYBACK_MODE_NORMAL);
	playlist_load_paths(tracks, 3, 0);
	CHECK(playlist_advance_auto(next, sizeof(next)), "normal mode advances");
	CHECK(playlist_advance_auto(next, sizeof(next)), "normal mode advances again");
	CHECK(!playlist_advance_auto(next, sizeof(next)), "normal mode stops at the last track");
}

int main(void) {
	test_cycle();
	test_stop_after_stops_music();
	test_stop_after_leaves_audiobook_queue_alone();
	test_ordered_queue_advances_in_stop_after();
	test_read_ahead_and_cover_rule();
	test_normal_mode_stops_at_end();

	if (failures) {
		fprintf(stderr, "%d playback-mode check(s) failed\n", failures);
		return 1;
	}
	printf("playback-mode checks passed\n");
	return 0;
}
