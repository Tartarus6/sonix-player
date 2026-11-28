#include "soundcloud.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/sha256.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamkeys.h"

// The two public endpoints. Configurable for the same reason Tidal's are: a
// fake server can stand in for both and exercise the whole flow offline.
#define SOUNDCLOUD_API_DEFAULT "https://api.soundcloud.com"
#define SOUNDCLOUD_AUTH_DEFAULT "https://secure.soundcloud.com"
#define SOUNDCLOUD_TOKEN_PATH "/oauth/token"

// A page under the project's own domain. SoundCloud sends the code to this
// address, but the person reads it off the browser and types it in on the
// device, so nothing has to be served there.
#define SOUNDCLOUD_REDIRECT_URI "https://tartarus6.github.io/sonix-player/soundcloud/callback"

#define SOUNDCLOUD_TIMEOUT_SECS 15
#define SOUNDCLOUD_BODY_LIMIT (2 * 1024 * 1024)
#define SOUNDCLOUD_PAGE_LIMIT 50

// The player draws covers larger than the 100 px "large" image SoundCloud puts
// in artwork_url. The same art is served at 500 px under another suffix.
#define SOUNDCLOUD_COVER_PX 500

// Access tokens last about an hour. Renew a minute early: a request that starts
// just before expiry can finish after it, and SoundCloud answers 401 rather
// than playing half a track.
#define TOKEN_MARGIN_SECS 60

static char api_url[256];
static char auth_url[256];
static char token_url[320];
static char client_id[128];
static char client_secret[256];
static char redirect_uri[320];

static char access_token[SOUNDCLOUD_TOKEN_MAX];
static char refresh_token[SOUNDCLOUD_TOKEN_MAX];
static long token_expires_at; // seconds since the epoch, 0 = unknown
static char user_id[SOUNDCLOUD_ID_MAX];
static char display_name[SOUNDCLOUD_NAME_MAX];

// PKCE. The verifier must survive the trip to the browser, so it is kept in the
// session file as well as here. The state is only in the URL: with the code
// typed in by hand there is no redirect to check it against.
static char pkce_verifier[64];
static char pkce_challenge[64];
static char login_state[32];

// Per-thread, like http_last_error(): a search and a playback request can be in
// flight at once and must not overwrite each other's reason for failing.
static __thread char last_error[256];

const char *soundcloud_last_error(void) { return last_error; }

const char *soundcloud_access_token(void) { return access_token; }

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, args);
	va_end(args);
}

static void clear_error(void) { last_error[0] = '\0'; }

// ---------------------------------------------------------------------------
// session
//
// A separate file, as with Tidal: device_config.ini holds short values in
// fixed-size fields, and an OAuth token does not fit. The verifier lives here
// too, so a sign-in begun before a reboot can still be finished after one.
// ---------------------------------------------------------------------------

#define SESSION_FILE "soundcloud_session.ini"

static void session_path(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !cfg[0]) {
		return;
	}
	const char *slash = strrchr(cfg, '/');
	if (slash) {
		snprintf(out, size, "%.*s/%s", (int)(slash - cfg), cfg, SESSION_FILE);
	} else {
		snprintf(out, size, "%s", SESSION_FILE);
	}
}

static void session_put(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=%s\n", key, value ? value : "");
}

static void save_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}

	// A temporary file and then rename(). Truncating the real file first would
	// leave an empty session behind if power went away mid-write.
	char tmp[640];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "soundcloud: cannot save the session to %s\n", tmp);
		return;
	}
	session_put(f, "access_token", access_token);
	session_put(f, "refresh_token", refresh_token);
	fprintf(f, "expires_at=%ld\n", token_expires_at);
	session_put(f, "user_id", user_id);
	session_put(f, "user", display_name);
	session_put(f, "verifier", pkce_verifier);
	bool ok = fclose(f) == 0;

	if (!ok || rename(tmp, path) != 0) {
		fprintf(stderr, "soundcloud: the session was not saved\n");
		remove(tmp);
	}
}

static void load_session(void) {
	char path[600];
	session_path(path, sizeof(path));
	if (!path[0]) {
		return;
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return; // nobody has signed in yet
	}

	static char line[SOUNDCLOUD_TOKEN_MAX + 128];
	while (fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *key = line;
		const char *value = eq + 1;

		if (strcmp(key, "access_token") == 0) {
			snprintf(access_token, sizeof(access_token), "%s", value);
		} else if (strcmp(key, "refresh_token") == 0) {
			snprintf(refresh_token, sizeof(refresh_token), "%s", value);
		} else if (strcmp(key, "expires_at") == 0) {
			token_expires_at = strtol(value, NULL, 10);
		} else if (strcmp(key, "user_id") == 0) {
			snprintf(user_id, sizeof(user_id), "%s", value);
		} else if (strcmp(key, "user") == 0) {
			snprintf(display_name, sizeof(display_name), "%s", value);
		} else if (strcmp(key, "verifier") == 0) {
			snprintf(pkce_verifier, sizeof(pkce_verifier), "%s", value);
		}
	}
	fclose(f);
}

void soundcloud_init(void) {
	snprintf(api_url, sizeof(api_url), "%s", config_get("soundcloud", "base_url", SOUNDCLOUD_API_DEFAULT));
	snprintf(auth_url, sizeof(auth_url), "%s", config_get("soundcloud", "auth_url", SOUNDCLOUD_AUTH_DEFAULT));
	snprintf(token_url, sizeof(token_url), "%s%s", auth_url, SOUNDCLOUD_TOKEN_PATH);

	const char *id = streamkeys_soundcloud_client_id();
	const char *secret = streamkeys_soundcloud_client_secret();
	snprintf(client_id, sizeof(client_id), "%s", id ? id : "");
	snprintf(client_secret, sizeof(client_secret), "%s", secret ? secret : "");
	const char *redirect = streamkeys_soundcloud_redirect_uri();
	snprintf(redirect_uri, sizeof(redirect_uri), "%s",
			 redirect && redirect[0] ? redirect : SOUNDCLOUD_REDIRECT_URI);

	load_session();
	printf("soundcloud: %s, %s\n", soundcloud_configured() ? "configured" : "no application keys",
		   soundcloud_logged_in() ? "saved session" : "no session");
}

bool soundcloud_configured(void) { return client_id[0] && client_secret[0]; }

// The refresh token, not the access token: an expired access token is normal,
// since it renews itself on the next request.
bool soundcloud_logged_in(void) { return soundcloud_configured() && refresh_token[0]; }

const char *soundcloud_display_name(void) { return display_name; }

void soundcloud_logout(void) {
	access_token[0] = '\0';
	refresh_token[0] = '\0';
	token_expires_at = 0;
	user_id[0] = '\0';
	display_name[0] = '\0';
	pkce_verifier[0] = '\0';
	pkce_challenge[0] = '\0';
	login_state[0] = '\0';
	save_session();
	printf("soundcloud: session closed\n");
}

// ---------------------------------------------------------------------------
// base64 and PKCE
//
// base64_decode() only goes one way. PKCE needs an encoder for the verifier and
// its challenge, and the client-credentials grant needs one for HTTP Basic.
// ---------------------------------------------------------------------------

static const char B64_STD[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void b64_encode(const unsigned char *in, size_t len, const char *alphabet, bool padding, char *out,
					   size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';

	size_t i = 0;
	size_t k = 0;
	for (; i + 2 < len; i += 3) {
		if (k + 4 >= size) {
			out[k] = '\0';
			return;
		}
		unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
		out[k++] = alphabet[(v >> 18) & 63];
		out[k++] = alphabet[(v >> 12) & 63];
		out[k++] = alphabet[(v >> 6) & 63];
		out[k++] = alphabet[v & 63];
	}
	if (k + 4 >= size) {
		out[k] = '\0';
		return;
	}

	// PKCE drops padding; HTTP Basic keeps it.
	size_t rem = len - i;
	if (rem == 1) {
		unsigned v = (unsigned)in[i] << 16;
		out[k++] = alphabet[(v >> 18) & 63];
		out[k++] = alphabet[(v >> 12) & 63];
		if (padding) {
			out[k++] = '=';
			out[k++] = '=';
		}
	} else if (rem == 2) {
		unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
		out[k++] = alphabet[(v >> 18) & 63];
		out[k++] = alphabet[(v >> 12) & 63];
		out[k++] = alphabet[(v >> 6) & 63];
		if (padding) {
			out[k++] = '=';
		}
	}
	out[k] = '\0';
}

static void random_bytes(unsigned char *out, size_t len) {
	FILE *f = fopen("/dev/urandom", "rb");
	if (f) {
		size_t got = fread(out, 1, len, f);
		fclose(f);
		if (got == len) {
			return;
		}
	}
	// The simulator has no entropy device. Time and pid still keep the verifier
	// from being a constant that anyone can read out of the source.
	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (size_t i = 0; i < len; i++) {
		out[i] = (unsigned char)(rand() & 0xff);
	}
}

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return 0;
}

static void make_pkce(void) {
	unsigned char raw[32];
	random_bytes(raw, sizeof(raw));
	b64_encode(raw, sizeof(raw), B64_URL, false, pkce_verifier, sizeof(pkce_verifier));
	random_bytes(raw, 16);
	b64_encode(raw, 16, B64_URL, false, login_state, sizeof(login_state));

	sha256_ctx sha;
	sha256_init(&sha);
	sha256_update(&sha, pkce_verifier, strlen(pkce_verifier));
	char hex[SHA256_HEX_LEN];
	sha256_final_hex(&sha, hex);

	// sha256.h hands out hex, PKCE wants base64url of the same 32 bytes.
	unsigned char digest[SHA256_DIGEST_LEN];
	for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
		digest[i] = (unsigned char)((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
	}
	b64_encode(digest, sizeof(digest), B64_URL, false, pkce_challenge, sizeof(pkce_challenge));
}

// ---------------------------------------------------------------------------
// requests
// ---------------------------------------------------------------------------

static void api_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

static bool url_ok(int n, size_t size) {
	if (n <= 0 || (size_t)n >= size) {
		set_error("%s", tr("SoundCloud request is too large"));
		return false;
	}
	return true;
}

// SoundCloud says why it refused in "message"; the token endpoint uses the
// OAuth fields instead. Both are written for a person, so show them as-is.
static void note_api_error(const json_doc_t *doc, int status) {
	int root = json_root(doc);
	static const char *FIELDS[] = {"message", "error_description", "error"};
	char message[256] = "";
	for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
		if (json_obj_str(doc, root, FIELDS[i], message, sizeof(message)) && message[0]) {
			set_error("%s", message);
			return;
		}
	}
	set_error("SoundCloud answered with %d", status);
}

static bool token_request(const char *form, const char *headers, json_doc_t *doc, char **body, int *status_out) {
	http_req_t req = {
		.method = "POST",
		.extra_headers = headers,
		.body = form,
		.want_error_body = true,
		.allow_empty_body = true,
	};
	int status = 0;
	size_t len = 0;
	if (!http_request(token_url, &req, body, &len, SOUNDCLOUD_BODY_LIMIT, SOUNDCLOUD_TIMEOUT_SECS, &status)) {
		const char *why = http_last_error();
		set_error("%s", why && why[0] ? why : tr("SoundCloud did not answer"));
		return false;
	}
	if (status_out) {
		*status_out = status;
	}
	if (!json_parse(*body, doc)) {
		free(*body);
		*body = NULL;
		set_error("%s", tr("SoundCloud sent an unreadable reply"));
		return false;
	}
	if (status != 200) {
		note_api_error(doc, status);
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

// Keeps a user's refresh token when an app-token reply arrives without one.
static void store_tokens(const json_doc_t *doc) {
	int root = json_root(doc);
	char access[sizeof(access_token)] = "";
	char refresh[sizeof(refresh_token)] = "";
	json_obj_str(doc, root, "access_token", access, sizeof(access));
	json_obj_str(doc, root, "refresh_token", refresh, sizeof(refresh));

	long long expires = json_obj_llong(doc, root, "expires_in", 0);
	if (expires < 0) {
		expires = 0;
	}
	// A nonsensical expiry must not overflow the addition below.
	if (expires > 31536000LL) {
		expires = 31536000LL;
	}

	snprintf(access_token, sizeof(access_token), "%s", access);
	if (refresh[0]) {
		snprintf(refresh_token, sizeof(refresh_token), "%s", refresh);
	}
	token_expires_at = expires > 0 ? (long)time(NULL) + (long)expires : 0;
	save_session();
}

// An app token is enough for search and catalogue pages. It carries no user,
// so likes playlists and /me still require the authorization-code session.
static bool token_client_credentials(void) {
	clear_error();
	if (!soundcloud_configured()) {
		set_error("%s", tr("SoundCloud application keys are unavailable"));
		return false;
	}

	char pair[sizeof(client_id) + sizeof(client_secret) + 2];
	char basic[sizeof(pair) * 2];
	char headers[sizeof(basic) + 32];
	int n = snprintf(pair, sizeof(pair), "%s:%s", client_id, client_secret);
	if (!url_ok(n, sizeof(pair))) {
		return false;
	}
	b64_encode((const unsigned char *)pair, strlen(pair), B64_STD, true, basic, sizeof(basic));
	n = snprintf(headers, sizeof(headers), "Authorization: Basic %s\r\n", basic);
	if (!url_ok(n, sizeof(headers))) {
		return false;
	}

	char *body = NULL;
	json_doc_t doc;
	if (!token_request("grant_type=client_credentials", headers, &doc, &body, NULL)) {
		return false;
	}
	store_tokens(&doc);
	api_done(body, &doc);
	printf("soundcloud: app token acquired\n");
	return true;
}

bool soundcloud_refresh_token(void) {
	clear_error();
	if (!soundcloud_configured()) {
		set_error("%s", tr("SoundCloud application keys are unavailable"));
		return false;
	}
	if (!refresh_token[0]) {
		set_error("%s", tr("SoundCloud has no session to refresh"));
		return false;
	}

	// The refresh grant sends the credentials in the form, not in a Basic
	// header. The buffer must be as wide as the token it carries.
	char form[SOUNDCLOUD_TOKEN_MAX + 1024];
	int n = snprintf(form, sizeof(form), "grant_type=refresh_token&refresh_token=%s&client_id=%s&client_secret=%s",
					 refresh_token, client_id, client_secret);
	if (!url_ok(n, sizeof(form))) {
		return false;
	}

	char *body = NULL;
	json_doc_t doc;
	int status = 0;
	if (!token_request(form, NULL, &doc, &body, &status)) {
		// A refused refresh is a dead session, not a network error. Retrying it
		// on every page would never recover.
		if (status >= 400 && status < 500) {
			soundcloud_logout();
		}
		return false;
	}
	store_tokens(&doc);
	api_done(body, &doc);
	printf("soundcloud: token refreshed\n");
	return true;
}

static bool token_is_fresh(void) {
	if (!access_token[0]) {
		return false;
	}
	if (token_expires_at <= 0) {
		return true; // expiry unknown: try it, and a 401 will sort it out
	}
	return (long)time(NULL) + TOKEN_MARGIN_SECS < token_expires_at;
}

static bool ensure_token(void) {
	if (!soundcloud_configured()) {
		set_error("%s", tr("SoundCloud application keys are unavailable"));
		return false;
	}
	if (token_is_fresh()) {
		return true;
	}
	if (refresh_token[0]) {
		return soundcloud_refresh_token();
	}
	return token_client_credentials();
}

static bool build_headers(char *out, size_t size) {
	// Every SoundCloud API call says OAuth, not Bearer.
	int n = snprintf(out, size, "Authorization: OAuth %s\r\n", access_token);
	if (!url_ok(n, size)) {
		return false;
	}
	return true;
}

// One GET, with renewal in the one place every caller shares. A 401 from a
// token that died before its recorded expiry is retried once after a refresh;
// twice would be a loop with one request per millisecond.
static bool api_call(const char *url, char **body, json_doc_t *doc) {
	clear_error();
	if (!ensure_token()) {
		if (!last_error[0]) {
			set_error("%s", tr("SoundCloud session renewal failed"));
		}
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char headers[SOUNDCLOUD_TOKEN_MAX + 64];
		if (!build_headers(headers, sizeof(headers))) {
			return false;
		}
		http_req_t req = {
			.extra_headers = headers,
			.want_error_body = true,
			// Album art and likes can answer with no body at all; without this
			// a 401 would read as "no reply" and never reach the refresh.
			.allow_empty_body = true,
		};

		int status = 0;
		size_t len = 0;
		if (!http_request(url, &req, body, &len, SOUNDCLOUD_BODY_LIMIT, SOUNDCLOUD_TIMEOUT_SECS, &status)) {
			const char *why = http_last_error();
			set_error("%s", why && why[0] ? why : tr("SoundCloud did not answer"));
			return false;
		}

		if (status == 401 && attempt == 0) {
			free(*body);
			*body = NULL;
			bool renewed = refresh_token[0] ? soundcloud_refresh_token() : token_client_credentials();
			if (renewed) {
				continue;
			}
			if (!last_error[0]) {
				set_error("%s", tr("SoundCloud session expired"));
			}
			return false;
		}

		if (!json_parse(*body, doc)) {
			free(*body);
			*body = NULL;
			set_error("%s", tr("SoundCloud sent an unreadable reply"));
			return false;
		}
		if (status != 200) {
			note_api_error(doc, status);
			json_free(doc);
			free(*body);
			*body = NULL;
			return false;
		}
		return true;
	}
	return false;
}

// PUT and DELETE answer 200 or 204 with nothing useful in the body, so success
// comes from the status code.
static bool api_write(const char *method, const char *url, const char *form) {
	clear_error();
	if (!soundcloud_logged_in()) {
		set_error("%s", tr("SoundCloud is not signed in"));
		return false;
	}
	if (!ensure_token()) {
		if (!last_error[0]) {
			set_error("%s", tr("SoundCloud session renewal failed"));
		}
		return false;
	}

	for (int attempt = 0; attempt < 2; attempt++) {
		char headers[SOUNDCLOUD_TOKEN_MAX + 64];
		if (!build_headers(headers, sizeof(headers))) {
			return false;
		}
		http_req_t req = {
			.method = method,
			.extra_headers = headers,
			.body = form,
			.want_error_body = true,
			.allow_empty_body = true,
		};

		char *body = NULL;
		int status = 0;
		if (!http_request(url, &req, &body, NULL, SOUNDCLOUD_BODY_LIMIT, SOUNDCLOUD_TIMEOUT_SECS, &status)) {
			const char *why = http_last_error();
			set_error("%s", why && why[0] ? why : tr("SoundCloud did not answer"));
			return false;
		}

		if (status == 401 && attempt == 0) {
			free(body);
			if (soundcloud_refresh_token()) {
				continue;
			}
			if (!last_error[0]) {
				set_error("%s", tr("SoundCloud session expired"));
			}
			return false;
		}

		bool ok = status >= 200 && status < 300;
		if (!ok) {
			json_doc_t doc;
			if (body && body[0] && json_parse(body, &doc)) {
				note_api_error(&doc, status);
				json_free(&doc);
			} else {
				set_error("SoundCloud answered with %d", status);
			}
			fprintf(stderr, "soundcloud: %s %s -> %d: %s\n", method, url, status, last_error);
		}
		free(body);
		return ok;
	}
	return false;
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

bool soundcloud_login_begin(char *url, size_t url_size) {
	clear_error();
	if (!url || url_size == 0) {
		return false;
	}
	url[0] = '\0';
	if (!soundcloud_configured()) {
		set_error("%s", tr("SoundCloud application keys are unavailable"));
		return false;
	}

	make_pkce();
	save_session(); // keep the verifier in case the device reboots mid-sign-in

	char cid[sizeof(client_id) * 4];
	char redirect[400];
	char challenge[64];
	char state[64];
	http_url_encode(client_id, cid, sizeof(cid));
	http_url_encode(redirect_uri, redirect, sizeof(redirect));
	http_url_encode(pkce_challenge, challenge, sizeof(challenge));
	http_url_encode(login_state, state, sizeof(state));

	int n = snprintf(url, url_size,
					 "%s/authorize?client_id=%s&redirect_uri=%s&response_type=code&code_challenge=%s"
					 "&code_challenge_method=S256&state=%s",
					 auth_url, cid, redirect, challenge, state);
	if (!url_ok(n, url_size)) {
		return false;
	}
	return true;
}

bool soundcloud_login_finish(const char *code) {
	clear_error();
	if (!soundcloud_configured()) {
		set_error("%s", tr("SoundCloud application keys are unavailable"));
		return false;
	}
	if (!code || !code[0]) {
		set_error("%s", tr("SoundCloud gave no sign-in code"));
		return false;
	}
	if (!pkce_verifier[0]) {
		set_error("%s", tr("Start the sign-in again"));
		return false;
	}

	char encoded_code[512];
	char cid[sizeof(client_id) * 4];
	char secret[sizeof(client_secret) * 4];
	char redirect[400];
	http_url_encode(code, encoded_code, sizeof(encoded_code));
	http_url_encode(client_id, cid, sizeof(cid));
	http_url_encode(client_secret, secret, sizeof(secret));
	http_url_encode(redirect_uri, redirect, sizeof(redirect));

	// The code is bound to this verifier: a code meant for another player cannot
	// be exchanged with it, even if someone reads it off the callback page.
	char form[sizeof(encoded_code) + sizeof(cid) + sizeof(secret) + 512];
	int n = snprintf(form, sizeof(form),
					 "grant_type=authorization_code&code=%s&redirect_uri=%s"
					 "&client_id=%s&client_secret=%s&code_verifier=%s",
					 encoded_code, redirect, cid, secret, pkce_verifier);
	if (!url_ok(n, sizeof(form))) {
		return false;
	}

	char *body = NULL;
	json_doc_t doc;
	if (!token_request(form, NULL, &doc, &body, NULL)) {
		return false;
	}
	char access[sizeof(access_token)] = "";
	json_obj_str(&doc, json_root(&doc), "access_token", access, sizeof(access));
	if (!access[0]) {
		api_done(body, &doc);
		set_error("%s", tr("SoundCloud gave no session token"));
		return false;
	}
	store_tokens(&doc);
	api_done(body, &doc);

	// The token reply has no account in it. Fetch /me so likes and playlists
	// know whose URN to use. A failure there does not undo a good sign-in.
	if (!soundcloud_me(NULL, 0, NULL, 0)) {
		clear_error();
	}
	printf("soundcloud: logged in as %s\n", display_name[0] ? display_name : user_id);
	return true;
}

// ---------------------------------------------------------------------------
// covers
// ---------------------------------------------------------------------------

void soundcloud_cover_url(const char *url, int size_px, char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	out[0] = '\0';
	if (!url || !url[0]) {
		return;
	}

	// artwork_url ends in "-large.jpg", a 100 px image. The same art is served
	// at 500 px as "-t500x500.jpg". Other suffixes have no known larger form and
	// are passed through untouched.
	static const char SUFFIX[] = "-large.jpg";
	size_t len = strlen(url);
	int n = 0;
	if (size_px >= SOUNDCLOUD_COVER_PX && len > sizeof(SUFFIX) - 1 &&
		strcmp(url + len - (sizeof(SUFFIX) - 1), SUFFIX) == 0) {
		n = snprintf(out, out_size, "%.*s-t500x500.jpg", (int)(len - (sizeof(SUFFIX) - 1)), url);
	} else {
		n = snprintf(out, out_size, "%s", url);
	}
	if (n <= 0 || (size_t)n >= out_size) {
		out[0] = '\0';
	}
}

// ---------------------------------------------------------------------------
// JSON into structures
// ---------------------------------------------------------------------------

// SoundCloud now sends URNs ("soundcloud:tracks:12345"), but some responses
// still carry a plain number. Turning that number into the same URN keeps the
// ids in these structs one kind of string.
static void read_id(const json_doc_t *doc, int obj, char *out, size_t size, const char *kind) {
	out[0] = '\0';
	if (json_obj_str(doc, obj, "id", out, size) && out[0]) {
		return;
	}
	long long n = json_obj_llong(doc, obj, "id", 0);
	if (n > 0) {
		snprintf(out, size, "soundcloud:%s:%lld", kind, n);
	}
}

static void read_track(const json_doc_t *doc, int obj, soundcloud_track_t *t) {
	memset(t, 0, sizeof(*t));
	if (obj < 0) {
		return;
	}

	read_id(doc, obj, t->id, sizeof(t->id), "tracks");
	json_obj_str(doc, obj, "title", t->title, sizeof(t->title));
	int user = json_get(doc, obj, "user");
	if (!json_obj_str(doc, user, "username", t->artist, sizeof(t->artist)) || !t->artist[0]) {
		json_str(doc, json_path(doc, obj, "publisher_metadata.artist"), t->artist, sizeof(t->artist));
	}

	char art[SOUNDCLOUD_URL_MAX] = "";
	if (json_obj_str(doc, obj, "artwork_url", art, sizeof(art)) && art[0]) {
		soundcloud_cover_url(art, SOUNDCLOUD_COVER_PX, t->cover, sizeof(t->cover));
	} else if (json_obj_str(doc, user, "avatar_url", art, sizeof(art)) && art[0]) {
		soundcloud_cover_url(art, SOUNDCLOUD_COVER_PX, t->cover, sizeof(t->cover));
	}

	// duration is milliseconds here, not seconds.
	long long ms = json_obj_llong(doc, obj, "duration", 0);
	if (ms > 2147483647LL) {
		ms = 2147483647LL;
	}
	t->duration = ms > 0 ? (int)(ms / 1000) : 0;

	// "blocked" has no stream at all; "preview" only offers a snippet, which
	// should not be queued as a whole track.
	char access[32] = "";
	json_obj_str(doc, obj, "access", access, sizeof(access));
	t->streamable = json_obj_bool(doc, obj, "streamable", true) && strcmp(access, "blocked") != 0 &&
					strcmp(access, "preview") != 0;
}

static void read_playlist(const json_doc_t *doc, int obj, soundcloud_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	if (obj < 0) {
		return;
	}

	read_id(doc, obj, p->id, sizeof(p->id), "playlists");
	json_obj_str(doc, obj, "title", p->title, sizeof(p->title));
	json_obj_str(doc, json_get(doc, obj, "user"), "username", p->owner, sizeof(p->owner));
	int tracks = json_get(doc, obj, "tracks");
	p->track_count = (int)json_obj_llong(doc, obj, "track_count", json_len(doc, tracks));

	char art[SOUNDCLOUD_URL_MAX] = "";
	if (json_obj_str(doc, obj, "artwork_url", art, sizeof(art)) && art[0]) {
		soundcloud_cover_url(art, SOUNDCLOUD_COVER_PX, p->cover, sizeof(p->cover));
	}
}

static void read_user(const json_doc_t *doc, int obj, soundcloud_user_t *u) {
	memset(u, 0, sizeof(*u));
	if (obj < 0) {
		return;
	}

	read_id(doc, obj, u->id, sizeof(u->id), "users");
	if (!json_obj_str(doc, obj, "username", u->name, sizeof(u->name)) || !u->name[0]) {
		json_obj_str(doc, obj, "full_name", u->name, sizeof(u->name));
	}

	char art[SOUNDCLOUD_URL_MAX] = "";
	if (json_obj_str(doc, obj, "avatar_url", art, sizeof(art)) && art[0]) {
		soundcloud_cover_url(art, SOUNDCLOUD_COVER_PX, u->cover, sizeof(u->cover));
	}
}

// ---------------------------------------------------------------------------
// walking a list
//
// linked_partitioning responses are {"collection":[...],"next_href":"..."}.
// The whole next_href is passed back in `cursor`. It is a full URL, so the next
// page is fetched as-is rather than rebuilt from the path and a page number.
// ---------------------------------------------------------------------------

typedef void (*reader_fn)(const json_doc_t *doc, int obj, void *out, int index);

static void track_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_track(doc, obj, &((soundcloud_track_t *)out)[index]);
}

static void playlist_reader(const json_doc_t *doc, int obj, void *out, int index) {
	read_playlist(doc, obj, &((soundcloud_playlist_t *)out)[index]);
}

static int fetch_page(const char *url, reader_fn read, void *out, int max, char *cursor, size_t cursor_size) {
	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return -1;
	}

	int root = json_root(&doc);
	int items = json_get(&doc, root, "collection");
	if (items < 0) {
		items = json_get(&doc, root, "items");
	}
	if (items < 0 && root >= 0 && doc.toks[root].type == JSON_ARRAY) {
		items = root;
	}
	int count = json_len(&doc, items);
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		read(&doc, json_at(&doc, items, i), out, i);
	}

	if (cursor && cursor_size) {
		char next[SOUNDCLOUD_URL_MAX] = "";
		json_obj_str(&doc, root, "next_href", next, sizeof(next));
		snprintf(cursor, cursor_size, "%s", next);
	}
	api_done(body, &doc);
	return count;
}

static int list_call(char *cursor, size_t cursor_size, const char *path, const char *query, reader_fn read, void *out,
					 int max) {
	char url[SOUNDCLOUD_URL_MAX + 128];
	int n = 0;
	if (cursor && cursor[0]) {
		if (strncmp(cursor, "http://", 7) == 0 || strncmp(cursor, "https://", 8) == 0) {
			n = snprintf(url, sizeof(url), "%s", cursor);
		} else {
			n = snprintf(url, sizeof(url), "%s%s%s", api_url, cursor[0] == '/' ? "" : "/", cursor);
		}
	} else {
		n = snprintf(url, sizeof(url), "%s%s?limit=%d&linked_partitioning=true%s%s", api_url, path,
					 SOUNDCLOUD_PAGE_LIMIT, query && query[0] ? "&" : "", query ? query : "");
	}
	if (!url_ok(n, sizeof(url))) {
		return -1;
	}
	return fetch_page(url, read, out, max, cursor, cursor_size);
}

int soundcloud_search_tracks(const char *query, char *cursor, size_t cursor_size, soundcloud_track_t *out, int max) {
	if (!query || !query[0] || !out || max <= 0) {
		return 0;
	}
	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));
	char q[600];
	snprintf(q, sizeof(q), "q=%s&access=playable", encoded);
	return list_call(cursor, cursor_size, "/tracks", q, track_reader, out, max);
}

int soundcloud_search_playlists(const char *query, char *cursor, size_t cursor_size, soundcloud_playlist_t *out,
								int max) {
	if (!query || !query[0] || !out || max <= 0) {
		return 0;
	}
	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));
	char q[600];
	snprintf(q, sizeof(q), "q=%s", encoded);
	return list_call(cursor, cursor_size, "/playlists", q, playlist_reader, out, max);
}

// Likes, reposts and the person's own content all hang off their URN. Fetch
// /me once if the session file does not have it yet.
static bool user_urn(char *out, size_t size) {
	if (!soundcloud_logged_in()) {
		set_error("%s", tr("SoundCloud is not signed in"));
		return false;
	}
	if (!user_id[0] && !soundcloud_me(NULL, 0, NULL, 0)) {
		return false;
	}
	if (!user_id[0]) {
		set_error("%s", tr("SoundCloud account is unknown"));
		return false;
	}
	snprintf(out, size, "%s", user_id);
	return true;
}

// The URN is encoded as a path segment; the colon reaches the server as %3A and
// is decoded there, the same as any other reserved character in a URL.
static bool user_path(char *out, size_t size, const char *tail) {
	char urn[SOUNDCLOUD_ID_MAX];
	if (!user_urn(urn, sizeof(urn))) {
		return false;
	}
	char encoded[SOUNDCLOUD_ID_MAX * 4];
	http_url_encode(urn, encoded, sizeof(encoded));
	return url_ok(snprintf(out, size, "/users/%s/%s", encoded, tail), size);
}

int soundcloud_liked_tracks(char *cursor, size_t cursor_size, soundcloud_track_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char path[SOUNDCLOUD_ID_MAX * 4 + 32];
	if (!user_path(path, sizeof(path), "likes/tracks")) {
		return -1;
	}
	return list_call(cursor, cursor_size, path, NULL, track_reader, out, max);
}

int soundcloud_reposts(char *cursor, size_t cursor_size, soundcloud_track_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char path[SOUNDCLOUD_ID_MAX * 4 + 32];
	if (!user_path(path, sizeof(path), "reposts")) {
		return -1;
	}
	return list_call(cursor, cursor_size, path, NULL, track_reader, out, max);
}

int soundcloud_user_playlists(char *cursor, size_t cursor_size, soundcloud_playlist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char path[SOUNDCLOUD_ID_MAX * 4 + 32];
	if (!user_path(path, sizeof(path), "playlists")) {
		return -1;
	}
	return list_call(cursor, cursor_size, path, NULL, playlist_reader, out, max);
}

int soundcloud_user_tracks(const char *user_id_arg, char *cursor, size_t cursor_size, soundcloud_track_t *out,
						   int max) {
	if (!user_id_arg || !user_id_arg[0] || !out || max <= 0) {
		return 0;
	}
	char encoded[SOUNDCLOUD_ID_MAX * 4];
	http_url_encode(user_id_arg, encoded, sizeof(encoded));
	char path[SOUNDCLOUD_ID_MAX * 8];
	snprintf(path, sizeof(path), "/users/%s/tracks", encoded);
	return list_call(cursor, cursor_size, path, NULL, track_reader, out, max);
}

int soundcloud_playlist_tracks(const char *playlist_id, char *cursor, size_t cursor_size, soundcloud_track_t *out,
							   int max) {
	if (!playlist_id || !playlist_id[0] || !out || max <= 0) {
		return 0;
	}
	// A playlist carries its tracks in one object. There is no next_href, so
	// clearing the cursor is what tells the caller the list has ended.
	if (cursor && cursor_size) {
		cursor[0] = '\0';
	}

	char encoded[SOUNDCLOUD_ID_MAX * 4];
	http_url_encode(playlist_id, encoded, sizeof(encoded));
	char url[SOUNDCLOUD_URL_MAX];
	int n = snprintf(url, sizeof(url), "%s/playlists/%s", api_url, encoded);
	if (!url_ok(n, sizeof(url))) {
		return -1;
	}

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return -1;
	}
	int root = json_root(&doc);
	int tracks = json_get(&doc, root, "tracks");
	int count = json_len(&doc, tracks);
	if (count > max) {
		count = max;
	}
	for (int i = 0; i < count; i++) {
		read_track(&doc, json_at(&doc, tracks, i), &out[i]);
	}
	api_done(body, &doc);
	return count;
}

bool soundcloud_me(char *id, size_t id_size, char *name, size_t name_size) {
	clear_error();
	if (id && id_size) {
		id[0] = '\0';
	}
	if (name && name_size) {
		name[0] = '\0';
	}
	if (!soundcloud_logged_in()) {
		set_error("%s", tr("SoundCloud is not signed in"));
		return false;
	}

	char url[512];
	int n = snprintf(url, sizeof(url), "%s/me", api_url);
	if (!url_ok(n, sizeof(url))) {
		return false;
	}

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return false;
	}
	soundcloud_user_t user;
	read_user(&doc, json_root(&doc), &user);
	api_done(body, &doc);

	if (!user.id[0]) {
		set_error("%s", tr("SoundCloud account is unknown"));
		return false;
	}
	snprintf(user_id, sizeof(user_id), "%s", user.id);
	snprintf(display_name, sizeof(display_name), "%s", user.name[0] ? user.name : "SoundCloud");
	save_session();

	if (id && id_size) {
		snprintf(id, id_size, "%s", user_id);
	}
	if (name && name_size) {
		snprintf(name, name_size, "%s", display_name);
	}
	return true;
}

// ---------------------------------------------------------------------------
// where the audio lives
// ---------------------------------------------------------------------------

bool soundcloud_track_stream(const char *id, soundcloud_stream_t *out) {
	clear_error();
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!id || !id[0]) {
		set_error("%s", tr("SoundCloud track is unknown"));
		return false;
	}

	char encoded[SOUNDCLOUD_ID_MAX * 4];
	http_url_encode(id, encoded, sizeof(encoded));
	char url[SOUNDCLOUD_URL_MAX];
	int n = snprintf(url, sizeof(url), "%s/tracks/%s/streams", api_url, encoded);
	if (!url_ok(n, sizeof(url))) {
		return false;
	}

	char *body = NULL;
	json_doc_t doc;
	if (!api_call(url, &body, &doc)) {
		return false;
	}

	int root = json_root(&doc);
	if (root < 0) {
		api_done(body, &doc);
		set_error("%s", tr("SoundCloud sent no stream list"));
		return false;
	}
	int list = root;
	if (doc.toks[root].type != JSON_ARRAY) {
		list = json_get(&doc, root, "collection");
	}
	int count = json_len(&doc, list);
	bool found = false;
	for (int i = 0; i < count && !found; i++) {
		int transcoding = json_at(&doc, list, i);
		int format = json_get(&doc, transcoding, "format");
		char mime[32] = "";
		char protocol[32] = "";
		json_obj_str(&doc, format, "mime_type", mime, sizeof(mime));
		json_obj_str(&doc, format, "protocol", protocol, sizeof(protocol));

		// HLS is AAC in fragmented MP4, which this player cannot unwrap. Only
		// the progressive MP3 is wanted, whatever order the transcodings come
		// in and however many of them there are.
		if (strcmp(mime, "audio/mpeg") != 0 || strcmp(protocol, "progressive") != 0) {
			continue;
		}
		if (json_obj_str(&doc, transcoding, "url", out->url, sizeof(out->url)) && out->url[0]) {
			snprintf(out->mime, sizeof(out->mime), "audio/mpeg");
			found = true;
		}
	}
	api_done(body, &doc);

	if (!found) {
		set_error("%s", tr("SoundCloud has no playable MP3 stream for this track"));
	}
	return found;
}

bool soundcloud_like(const char *id, bool want) {
	if (!id || !id[0]) {
		return false;
	}
	char encoded[SOUNDCLOUD_ID_MAX * 4];
	http_url_encode(id, encoded, sizeof(encoded));
	char url[SOUNDCLOUD_URL_MAX];
	int n = snprintf(url, sizeof(url), "%s/likes/tracks/%s", api_url, encoded);
	if (!url_ok(n, sizeof(url))) {
		return false;
	}
	return api_write(want ? "PUT" : "DELETE", url, NULL);
}
