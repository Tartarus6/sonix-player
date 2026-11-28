// Generated compatibility layer for V1 streaming pages.
//
// The ported pages are built against the V1 service APIs, but the unified
// three-device tree has no network backends for those services. These stubs
// keep the UI compiling and make every service report "not configured" or
// "unavailable" instead of reaching the network or the card. HiBy-safe by
// construction: no sockets, no downloads, no writes.

#include "v1_streaming_compat.h"
#include <string.h>

void soundcloud_init(void) { (void)0; }

void soundcloud_reload_streamkeys(void) { (void)0; }

bool soundcloud_save_draft(const soundcloud_credentials_t *credentials, bool remember, char *error, size_t error_size) { return false; }

bool soundcloud_configure(const soundcloud_credentials_t *credentials, bool remember, char *error, size_t error_size) { return false; }

void soundcloud_credentials(soundcloud_credentials_t *out, bool *remembered) {
	if (out) memset(out, 0, sizeof(*out));
	if (remembered) *remembered = false;
}

void soundcloud_account_info(soundcloud_account_info_t *out) { if (out) memset(out, 0, sizeof(*out)); }

bool soundcloud_has_account(void) { return false; }

bool soundcloud_can_like(void) { return false; }

bool soundcloud_forget(void) { return false; }

int soundcloud_get_quality(void) { return 0; }

void soundcloud_set_quality(int quality) { (void)0; }

const char * soundcloud_expected_mime(void) { return "not available"; }

bool soundcloud_api_busy(void) { return false; }

const char * soundcloud_last_error(void) { return "not available"; }

void soundcloud_artwork_url(const char *raw, int size_px, char *out, size_t out_size) { (void)0; }

void soundcloud_headers_for_url(const char *for_url, char *out, size_t out_size) { (void)0; }

int soundcloud_search_tracks(const char *query, int offset, soundcloud_track_t *out, int max, bool *more) { return 0; }

int soundcloud_liked_tracks(const char *cursor, soundcloud_track_t *out, int max, char *next_cursor, size_t next_cursor_size) { return 0; }

int soundcloud_reposts(const char *cursor, soundcloud_repost_t *out, int max, char *next_cursor, size_t next_cursor_size) { return 0; }

int soundcloud_playlists(const char *cursor, soundcloud_playlist_t *out, int max, char *next_cursor, size_t next_cursor_size) { return 0; }

int soundcloud_playlist_tracks(long long playlist_id, const char *secret_token, int offset, soundcloud_track_t *out, int max, bool *more) { return 0; }

bool soundcloud_get_stream(long long track_id, const char *secret_token, bool allow_aac, soundcloud_stream_t *out) { return false; }

bool soundcloud_get_track_stream(const soundcloud_track_t *track, bool allow_aac, soundcloud_stream_t *out) { return false; }

bool soundcloud_get_track_stream_ex(const soundcloud_track_t *track, bool allow_aac, bool allow_progressive, soundcloud_stream_t *out) { return false; }

bool soundcloud_is_liked(long long track_id) { return false; }

void soundcloud_note_liked(long long track_id, bool liked) { (void)0; }

bool soundcloud_like_track(long long track_id, bool like) { return false; }

void soundcloud_set_endpoints_for_test(const char *api_url, const char *web_url) { (void)0; }

void soundcloudcache_set_root(const char *sd_root) { (void)0; }

const char * soundcloudcache_dir(void) { return "not available"; }

bool soundcloudcache_available(void) { return false; }

const char * soundcloudcache_last_error(void) { return "not available"; }

int soundcloudcache_get_max_mb(void) { return 0; }

void soundcloudcache_set_max_mb(int max_mb) { (void)0; }

long long soundcloudcache_get_max_bytes(void) { return 0; }

void soundcloudcache_set_max_bytes(long long max_bytes) { (void)0; }

bool soundcloudcache_get(const soundcloud_track_t *track, char *out_path, size_t out_size) { return false; }

bool soundcloudcache_prefetch(const soundcloud_track_t *track) { return false; }

void soundcloudcache_cancel(void) { (void)0; }

bool soundcloudcache_downloading(void) { return false; }

long long soundcloudcache_current_id(void) { return 0; }

bool soundcloudcache_is_soundcloud_path(const char *path) { return false; }

long long soundcloudcache_track_id_of(const char *path) { return 0; }

bool soundcloudcache_placeholder_path(long long id, char *out_path, size_t out_size) { return false; }

bool soundcloudcache_find_playable(long long id, char *out_path, size_t out_size) { return false; }

void soundcloudcache_protect(const char *path) { (void)0; }

void soundcloudcache_clear(void) { (void)0; }

void soundcloudcache_clear_on_exit(void) { (void)0; }

void soundcloudcache_stats(int *files, long long *bytes) { if (files) *files = 0; if (bytes) *bytes = 0; }

void soundcloudcache_set_aac_override_for_test(int mode) { (void)0; }

void subsonic_init(void) { (void)0; }

bool subsonic_configure(const subsonic_credentials_t *credentials, bool remember, char *error, size_t error_size) { return false; }

void subsonic_credentials(subsonic_credentials_t *out, bool *remembered) {
	if (out) memset(out, 0, sizeof(*out));
	if (remembered) *remembered = false;
}

bool subsonic_configured(void) { return false; }

bool subsonic_forget(void) { return false; }

subsonic_quality_t subsonic_quality(void) { return (subsonic_quality_t){0}; }

void subsonic_set_quality(subsonic_quality_t quality) { (void)0; }

bool subsonic_ping(char *error, size_t error_size) { return false; }

bool subsonic_api_busy(void) { return false; }

int subsonic_browse(subsonic_browse_t browse, const char *id, int offset, subsonic_entry_t *entries, int limit, bool *more, char *error, size_t error_size) { return 0; }

bool subsonic_stream_url(const char *id, subsonic_quality_t quality, char *out, size_t size, char *error, size_t error_size) { return false; }

bool subsonic_cover_url(const char *id, char *out, size_t size, char *error, size_t error_size) { return false; }

const char * subsonicdl_root(void) { return "not available"; }

bool subsonicdl_path(const subsonic_entry_t *track, subsonic_quality_t quality, char *out, size_t size) { return false; }

bool subsonicdl_exists(const subsonic_entry_t *track, subsonic_quality_t quality) { return false; }

void subsonicdl_safe_name(const char *title, const char *fallback, char *out, size_t size) { (void)0; }

bool subsonicdl_start(const subsonicdl_request_t *request) { return false; }

void subsonicdl_cancel(void) { (void)0; }

void subsonicdl_progress(subsonicdl_progress_t *out) { if (out) memset(out, 0, sizeof(*out)); }

bool subsonicdl_busy(void) { return false; }

void subsonicdl_acknowledge(void) { (void)0; }

int subsonicdl_history(subsonicdl_entry_t *entries, int max) { return 0; }

bool subsonicdl_delete(const char *path) { return false; }

bool subsonicdl_owns(const char *path) { return false; }

bool subsonicstream_owns(const char *path) { return false; }

bool subsonicstream_queue(const subsonic_entry_t *tracks, int count, int start, char paths[][96], const char **pointers, int *queued, int *selected, char *error, size_t error_size) { return false; }

bool subsonicstream_prepare(char *path, size_t path_size) { return false; }

void subsonicstream_prefetch(const char *path) { (void)0; }

bool subsonicstream_ready_for_crossfade(const char *path) { return false; }

void subsonicstream_cancel(void) { (void)0; }

void subsonicstream_reset(void) { (void)0; }

bool subsonicstream_network_wanted(void) { return false; }

streambridge_t * subsonicstream_acquire(const char *path, int *duration, int *bitrate) { return NULL; }

void subsonicstream_release(streambridge_t *bridge) { (void)0; }

bool subsonicstream_metadata(const char *path, song_metadata_t *out) { (void)path; if (out) memset(out, 0, sizeof(*out)); return false; }

bool subsonicstream_take_error(char *out, size_t size) { return false; }

share_status_t networkmusic_list_start(const share_server_t *server, const char *folder) { return SHARE_UNAVAILABLE; }

share_status_t networkmusic_save_start(const share_server_t *server, const char *folder, const share_entry_t *entries, size_t count) { return SHARE_UNAVAILABLE; }

void networkmusic_progress(networkmusic_progress_t *out) { if (out) memset(out, 0, sizeof(*out)); }

bool networkmusic_busy(void) { return false; }

bool networkmusic_take_result(networkmusic_result_t *out) { return false; }

void networkmusic_cancel(void) { (void)0; }

void networkmusic_release_card(void) { (void)0; }

bool networkshare_server_init(share_server_t *out, share_kind_t kind, const char *address, const char *username, const char *password, const char *ca_file) { return false; }

bool networkshare_valid_folder(const share_server_t *server, const char *folder) { return false; }

bool networkshare_valid_track(const char *name) { return false; }

bool networkshare_child_folder(const share_server_t *server, const char *folder, const char *name, char *out, size_t size) { return false; }

share_status_t networkshare_list(const share_server_t *server, const char *folder, share_listing_t *out, share_cancel_fn cancel, void *user) { return SHARE_UNAVAILABLE; }

share_status_t networkshare_fetch(const share_server_t *server, const char *folder, const char *name, share_cancel_fn cancel, share_write_fn write, share_size_fn content_size, void *user) { return SHARE_UNAVAILABLE; }

void podcastplayed_set_root(const char *sd_root) { (void)0; }

void podcastplayed_reload_if_replaced(void) { (void)0; }

bool podcastplayed_get(long long episode_id) { return false; }

void podcastplayed_set(long long episode_id, bool played) { (void)0; }

bool streamkeep_is_stream_track(const char *path) { return false; }

bool streamkeep_keep(const char *path, bool notify_result) { return false; }

bool streamkeep_busy(void) { return false; }

bool streamkeep_last_error(char *out, size_t out_size) { return false; }

streambridge_t * streambridge_create(size_t capacity) { return NULL; }

void streambridge_destroy(streambridge_t *bridge) { (void)0; }

bool streambridge_start(streambridge_t *bridge, const char *url, bool prefetch) { return false; }

void streambridge_promote(streambridge_t *bridge) { (void)0; }

void streambridge_cancel(streambridge_t *bridge) { (void)0; }

void streambridge_state(streambridge_t *bridge, streambridge_state_t *out) { (void)0; }

void streambridge_set_reader_abort(streambridge_abort_fn fn) { (void)0; }

bool streambridge_prime(streambridge_t *bridge) { return false; }

bool streambridge_wait_decode(streambridge_t *bridge, streambridge_abort_fn abort) { return false; }

size_t streambridge_read(streambridge_t *bridge, void *out, size_t bytes) { return 0; }

bool streambridge_seek(streambridge_t *bridge, int64_t offset, bool relative) { return false; }

uint64_t streambridge_tell(streambridge_t *bridge) { return 0; }

size_t streambridge_peek(streambridge_t *bridge, void *out, size_t bytes) { return 0; }

bool streambridge_failed(streambridge_t *bridge) { return false; }

void streambridge_fail(streambridge_t *bridge, const char *error) { (void)0; }


// Date parsing for the saved-podcast list. Kept local (rather than importing
// the fork's system/library/tagdate.c) because the page is the only caller.
#include "src/system/library/tagdate.h"

#include <time.h>

static int v1_date_precision(const tagdate_t *d) {
	return d->day > 0 ? 3 : d->month > 0 ? 2 : d->year > 0 ? 1 : 0;
}

static void v1_date_parts(tagdate_t *d, int year, int month, int day) {
	d->year = year >= 1 && year <= 9999 ? year : 0;
	d->month = d->year && month >= 1 && month <= 12 ? month : 0;
	d->day = d->month && day >= 1 && day <= 31 ? day : 0;
}

static int v1_date_digits(const char *s, int count) {
	int value = 0;
	for (int i = 0; i < count; i++) {
		if (s[i] < '0' || s[i] > '9') return -1;
		value = value * 10 + (s[i] - '0');
	}
	return value;
}

bool tagdate_parse(const char *text, tagdate_t *out) {
	if (!text || !out) return false;
	memset(out, 0, sizeof(*out));
	while (*text == ' ' || *text == '\t') text++;
	int digits = 0;
	while (text[digits] >= '0' && text[digits] <= '9') digits++;
	if (digits == 8) {
		v1_date_parts(out, v1_date_digits(text, 4), v1_date_digits(text + 4, 2), v1_date_digits(text + 6, 2));
		return out->year > 0;
	}
	if (digits == 4) {
		int year = v1_date_digits(text, 4), month = 0, day = 0;
		const char *p = text + 4;
		if (*p == '-' || *p == '/') {
			p++;
			int m = v1_date_digits(p, 2), take = 2;
			if (m < 0) { m = v1_date_digits(p, 1); take = 1; }
			if (m >= 0) {
				month = m;
				p += take;
				if (*p == '-' || *p == '/') day = v1_date_digits(p + 1, 2);
			}
		}
		v1_date_parts(out, year, month, day);
		return out->year > 0;
	}
	return false;
}

bool tagdate_better(const tagdate_t *a, const tagdate_t *b) {
	return v1_date_precision(a ? a : &(tagdate_t){0}) > v1_date_precision(b ? b : &(tagdate_t){0});
}

time_t tagdate_to_time(const tagdate_t *d) {
	if (!d || d->year <= 0) return 0;
	struct tm tm_date;
	memset(&tm_date, 0, sizeof(tm_date));
	tm_date.tm_year = d->year - 1900;
	tm_date.tm_mon = d->month > 0 ? d->month - 1 : 0;
	tm_date.tm_mday = d->day > 0 ? d->day : 1;
	tm_date.tm_isdst = -1;
	time_t when = mktime(&tm_date);
	return when == (time_t)-1 ? 0 : when;
}
