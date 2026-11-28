#ifndef SOUNDCLOUD_H
#define SOUNDCLOUD_H

#include <stdbool.h>
#include <stddef.h>

// SoundCloud, using the public API at api.soundcloud.com with OAuth 2.1.
//
// Two credentials come from the sealed keys file (see streamkeys.h):
// client_id and client_secret. A person signs in with the authorization code
// flow: the player shows a URL, the person approves it in a browser and types
// the code back on the device. PKCE keeps the exchange bound to this player.
// The refresh token keeps the session alive; access tokens last about an hour.
//
// Everything here blocks on the network. Call it from a worker thread, never
// from the LVGL or audio thread.

#define SOUNDCLOUD_TITLE_MAX 200
#define SOUNDCLOUD_NAME_MAX 128
#define SOUNDCLOUD_ID_MAX 64
#define SOUNDCLOUD_URL_MAX 2048
#define SOUNDCLOUD_TOKEN_MAX 512

// SoundCloud addresses resources by URN now ("soundcloud:tracks:12345").
// Keep the whole string: building URLs from a numeric id would break the day
// they change the prefix, and playlists and users use different ones.
typedef struct {
	char id[SOUNDCLOUD_ID_MAX];
	char title[SOUNDCLOUD_TITLE_MAX];
	char artist[SOUNDCLOUD_NAME_MAX];
	char cover[SOUNDCLOUD_URL_MAX];
	int duration; // seconds
	bool streamable; // false for blocked and preview-only tracks
} soundcloud_track_t;

typedef struct {
	char id[SOUNDCLOUD_ID_MAX];
	char title[SOUNDCLOUD_TITLE_MAX];
	char owner[SOUNDCLOUD_NAME_MAX];
	char cover[SOUNDCLOUD_URL_MAX];
	int track_count;
} soundcloud_playlist_t;

typedef struct {
	char id[SOUNDCLOUD_ID_MAX];
	char name[SOUNDCLOUD_NAME_MAX];
	char cover[SOUNDCLOUD_URL_MAX];
} soundcloud_user_t;

typedef struct {
	char url[SOUNDCLOUD_URL_MAX]; // signed URL for a progressive MP3
	char mime[32];
} soundcloud_stream_t;

// Read keys and the stored session. Call after config_init() and
// streamkeys_init().
void soundcloud_init(void);

// False when the keys file has no [soundcloud] section. The tile says so
// instead of failing during sign-in.
bool soundcloud_configured(void);
bool soundcloud_logged_in(void);
const char *soundcloud_display_name(void);
void soundcloud_logout(void);

// Why the last call failed, ready to display. Empty on success. Per thread.
const char *soundcloud_last_error(void);

// The live access token, for the first hop of a signed media URL. The cache
// uses it and never sends it to a CDN host on a redirect.
const char *soundcloud_access_token(void);

// --- sign-in ---------------------------------------------------------------

// Makes the verifier and challenge, then writes the URL for the browser.
// Blocks only on local work. False when the keys are missing.
bool soundcloud_login_begin(char *url, size_t url_size);

// Exchanges the code the person typed for tokens and saves the session.
// Blocks. False on error; soundcloud_last_error() says what happened.
bool soundcloud_login_finish(const char *code);

// Renews the access token with the refresh token. Blocks. The player calls
// this itself; pages do not need to.
bool soundcloud_refresh_token(void);

// --- reading ---------------------------------------------------------------

// Each returns the number of items written, 0 when the list ends, or -1 on
// error. `cursor` carries the API's next_href from one call to the next; pass
// an empty string for the first page. All of them block.
int soundcloud_search_tracks(const char *query, char *cursor, size_t cursor_size, soundcloud_track_t *out, int max);
int soundcloud_search_playlists(const char *query, char *cursor, size_t cursor_size, soundcloud_playlist_t *out,
								int max);
int soundcloud_liked_tracks(char *cursor, size_t cursor_size, soundcloud_track_t *out, int max);
int soundcloud_reposts(char *cursor, size_t cursor_size, soundcloud_track_t *out, int max);
int soundcloud_user_playlists(char *cursor, size_t cursor_size, soundcloud_playlist_t *out, int max);
int soundcloud_playlist_tracks(const char *playlist_id, char *cursor, size_t cursor_size, soundcloud_track_t *out,
							   int max);
int soundcloud_user_tracks(const char *user_id, char *cursor, size_t cursor_size, soundcloud_track_t *out, int max);

// The signed-in person's id and name. Blocks. False without a session.
bool soundcloud_me(char *id, size_t id_size, char *name, size_t name_size);

// Sets out->url to a progressive MP3 stream. SoundCloud's high-bitrate AAC
// streams are HLS fMP4, which this player cannot unwrap, so they are skipped.
// Tracks with access "blocked" have no stream and return false. Blocks.
bool soundcloud_track_stream(const char *id, soundcloud_stream_t *out);

// Likes or unlikes a track on the account. Blocks.
bool soundcloud_like(const char *id, bool want);

// Rewrites an artwork URL at the requested size (large -> t500x500).
void soundcloud_cover_url(const char *url, int size_px, char *out, size_t out_size);

#endif /* SOUNDCLOUD_H */
