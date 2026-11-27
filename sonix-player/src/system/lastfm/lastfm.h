#ifndef LASTFM_H
#define LASTFM_H

#include <stdbool.h>

#include "src/system/library/playcount.h"

// Scrobbling to Last.fm.
//
// playcount.c follows what is played and hands the play under way to
// lastfm_note_play() once a second while this is active; it decides when a
// play counts. This module only announces each play and queues it as a
// scrobble once it counts.
//
// A worker thread does the network and the files. "Now playing" goes out when
// there is Wi-Fi at the start of a track. Scrobbles go into a queue in .local
// on the card and are sent in batches whenever the Wi-Fi is up. Nothing
// here turns the Wi-Fi on, and the radio is held up only while a request is on
// the wire.
//
// The API account comes from the streaming keys ([lastfm]); without it the
// service is unavailable. Signing in fetches an auth token (auth.getToken),
// the user authorizes it at last.fm/api/auth, and auth.getSession trades it
// for a session key. Only the session key is kept, beside device_config.ini;
// no password is ever entered on the device.

typedef enum {
	LASTFM_STATE_UNAVAILABLE, // no API keys in the streaming keys
	LASTFM_STATE_OFF,
	LASTFM_STATE_SIGNED_OUT,
	LASTFM_STATE_SIGNING_IN,
	LASTFM_STATE_AWAITING_AUTH, // token fetched, waiting for the user
	LASTFM_STATE_CONNECTED,
	LASTFM_STATE_SESSION_EXPIRED, // Last.fm refused the session: sign in again
	LASTFM_STATE_REFUSED,		  // Last.fm refused a request; `detail` says why
} lastfm_state_t;

typedef enum {
	LASTFM_LOGIN_OK,
	LASTFM_LOGIN_NO_REPLY,
	LASTFM_LOGIN_REFUSED, // `detail` says why
	LASTFM_LOGIN_EXPIRED, // the token was not used within its 60 minutes
	LASTFM_LOGIN_NOT_AUTHORIZED, // error 14: the user has not approved it yet
} lastfm_login_result_t;

typedef struct {
	lastfm_state_t state;
	bool enabled;
	char user[128];
	char detail[160]; // Last.fm's own message or the transport error, in English
	int queued;		  // scrobbles not sent yet

	// Moves each time a sign-in attempt finishes; `login_result` is how it did.
	unsigned login_serial;
	lastfm_login_result_t login_result;

	// The authorization URL for the live token, and when it was last handed
	// out or cleared. `auth_seconds_left` counts down to the 60-minute limit.
	char auth_url[256];
	unsigned auth_serial;
	int auth_seconds_left;
} lastfm_status_t;

// Reads the session and the queue and starts the worker. Call once, after
// config_init() and streamkeys_init().
void lastfm_init(void);

bool lastfm_available(void);

// Stored in the configuration. GUI thread.
void lastfm_set_enabled(bool on);

// Switched on, with keys and an account, signed in or with a session waiting
// to be renewed: plays are being counted.
bool lastfm_active(void);

// Both return at once; the worker does the work. Begin fetches a token and
// publishes its authorization URL in the status; complete trades it for a
// session key after the user has approved it on last.fm.
void lastfm_begin_login(void);
void lastfm_complete_login(void);
void lastfm_logout(void);

void lastfm_get_status(lastfm_status_t *out);

// From playcount.c, GUI thread, about once a second while lastfm_active().
void lastfm_note_play(const playcount_play_t *now);

// True while a request is on the wire, so the Wi-Fi is not parked under it.
bool lastfm_network_wanted(void);

#endif /* LASTFM_H */
