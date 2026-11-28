#include "soundcloudcache.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

#define FETCH_TIMEOUT_SECS 20
#define FETCH_CHUNK 32768
#define PRUNE_MAX_ENTRIES 512

static char cache_dir[512];

// The reason a start failed, for the page to show. Per thread, like
// http_last_error(): a prefetch on one worker must not overwrite the reason a
// tap on another worker is about to display.
static __thread char last_error[192];

const char *soundcloudcache_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

static void soundcloudcache_abandon_all(void);

// One registration is enough. From then on a SoundCloud download knows how to
// let go when a Tidal or Qobuz track is requested, and the other way around.
static void register_abandon_once(void) {
	static bool done;
	if (!done) {
		done = true;
		streamturn_register_abandon(soundcloudcache_abandon_all);
	}
}

void soundcloudcache_set_root(const char *sd_root) {
	register_abandon_once();
	if (!sd_root || !*sd_root) {
		cache_dir[0] = '\0';
		return;
	}
	snprintf(cache_dir, sizeof(cache_dir), "%s/%s", sd_root, SOUNDCLOUDCACHE_DIR);
}

bool soundcloudcache_ready(void) { return cache_dir[0] != '\0'; }

// The id is a URN, whose last component is the track number. Using just that
// component keeps the name safe and short; a full "soundcloud:tracks:12345"
// would put colons in a file name and waste the directory entry. Anything not
// a letter, digit, '-' or '_' becomes '_' so an unexpected id cannot escape
// the cache directory through a '/' or "..".
static void id_stem(const char *id, char *out, size_t size) {
	out[0] = '\0';
	if (!id || !*id || size == 0) {
		return;
	}
	const char *tail = strrchr(id, ':');
	tail = tail ? tail + 1 : id;
	if (!*tail) {
		return;
	}

	size_t n = 0;
	for (const char *p = tail; *p && n + 1 < size; p++) {
		char c = *p;
		bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
					c == '_';
		out[n++] = safe ? c : '_';
	}
	out[n] = '\0';
}

void soundcloudcache_path(const char *id, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!soundcloudcache_ready()) {
		out[0] = '\0';
		return;
	}
	char stem[64];
	id_stem(id, stem, sizeof(stem));
	if (!stem[0]) {
		out[0] = '\0';
		return;
	}
	// The directory is bounded so a long card path cannot truncate the file
	// name away: a shortened path could point at something else entirely.
	snprintf(out, size, "%.400s/%.63s.mp3", cache_dir, stem);
}

// The file is written to its final name while the decoder reads it, so an
// empty sidecar is what says "this one is not finished yet".
static void marker_path(const char *path, char *out, size_t size) {
	snprintf(out, size, "%.500s.incompleto", path);
}

bool soundcloudcache_has(const char *id, char *out, size_t size) {
	char path[512];
	soundcloudcache_path(id, path, sizeof(path));
	if (!path[0]) {
		return false;
	}

	char marker[544];
	marker_path(path, marker, sizeof(marker));
	struct stat unused;
	if (stat(marker, &unused) == 0) {
		return false; // interrupted: fetch it again
	}

	struct stat st;
	if (stat(path, &st) != 0 || st.st_size == 0) {
		return false;
	}
	if (out && size) {
		snprintf(out, size, "%s", path);
	}
	return true;
}

// mkdir -p over the two levels needed (.local, then the cache directory).
static bool ensure_dir(void) {
	if (!soundcloudcache_ready()) {
		return false;
	}

	char path[512];
	snprintf(path, sizeof(path), "%s", cache_dir);
	for (char *p = path + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(path, 0777);
		*p = '/';
	}
	if (mkdir(path, 0777) != 0 && errno != EEXIST) {
		fprintf(stderr, "soundcloudcache: cannot create %s: %s\n", path, strerror(errno));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// measuring and pruning
// ---------------------------------------------------------------------------

typedef struct {
	char name[128];
	long size;
	long atime; // last access says which tracks are actually listened to
} entry_t;

static int scan(entry_t *out, int max, long *total_out) {
	long total = 0;
	int count = 0;

	DIR *d = opendir(cache_dir);
	if (!d) {
		if (total_out) {
			*total_out = 0;
		}
		return 0;
	}

	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		total += st.st_size;

		// A track coming down now is not prunable, and neither is its marker:
		// both share the stem, so compare on the name without extension.
		char stem[640];
		snprintf(stem, sizeof(stem), "%.500s/%.127s", cache_dir, e->d_name);
		char *dot = strrchr(stem + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (growfile_prefix_is_growing(stem)) {
			continue;
		}
		if (out && count < max && strlen(e->d_name) < sizeof(out[0].name)) {
			snprintf(out[count].name, sizeof(out[count].name), "%.127s", e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(d);

	if (total_out) {
		*total_out = total;
	}
	return count;
}

// Drops the least recently used tracks until the cache fits again. Access time
// rather than creation time: a track replayed yesterday stays, one downloaded
// yesterday and never played can go.
static void prune(long keep_room_for) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return; // nothing deleted; the ceiling is checked again next time
	}

	long total = 0;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total);
	if (total + keep_room_for <= SOUNDCLOUDCACHE_MAX_BYTES) {
		free(entries);
		return;
	}

	// At most 512 entries, and only when a download would overflow the cache.
	for (int i = 0; i < count; i++) {
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
	}

	for (int i = 0; i < count && total + keep_room_for > SOUNDCLOUDCACHE_MAX_BYTES; i++) {
		char path[640];
		snprintf(path, sizeof(path), "%.500s/%.127s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("soundcloudcache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}

	free(entries);
}

static bool clear_files(void) {
	if (!soundcloudcache_ready()) {
		return false;
	}
	DIR *d = opendir(cache_dir);
	if (!d) {
		return false;
	}
	int removed = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[640];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name) >= sizeof(path)) {
			continue;
		}
		if (remove(path) == 0) {
			removed++;
		}
	}
	closedir(d);
	printf("soundcloudcache: cleared (%d files)\n", removed);
	return true;
}

bool soundcloudcache_clear(void) { return clear_files(); }

bool soundcloudcache_is_track(const char *path) {
	if (!path || !*path || !cache_dir[0]) {
		return false;
	}
	size_t n = strlen(cache_dir);
	// The separator check keeps a sibling directory with the same prefix
	// ("soundcloud-cache-old") from being mistaken for this cache.
	return strncmp(path, cache_dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

// ---------------------------------------------------------------------------
// downloading
// ---------------------------------------------------------------------------
//
// The file goes straight to its final name: the decoder opens it while it
// grows, and a rename halfway through would swap it under the reader. One
// download at a time, since the turn belongs to the device and not to a
// service (see streamturn.h).
// ---------------------------------------------------------------------------

typedef struct {
	char id[SOUNDCLOUD_ID_MAX];
	char path[512];
	char marker[544];
	http_stream_t stream;
	FILE *f;
	long done;
	long total;
	unsigned generation;
} download_t;

// Changing track bumps this counter; a worker fetching the previous one stops
// at the next chunk and hands the bandwidth to what is playing now.
static unsigned generation = 1;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *active_stream;
static char active_id[SOUNDCLOUD_ID_MAX];

// Abandon is shared: another service taking the turn calls every registered
// abandon function. Bumping the counter alone is not enough when the socket is
// idle in recv(), which would hold the turn for the whole receive timeout.
// Shutting it down makes that recv() return at once.
static void soundcloudcache_abandon_all(void) {
	pthread_mutex_lock(&gen_lock);
	generation++;
	pthread_mutex_unlock(&gen_lock);

	pthread_mutex_lock(&active_lock);
	if (active_stream) {
		http_stream_wake(active_stream);
	}
	pthread_mutex_unlock(&active_lock);
}

static unsigned current_generation(void) {
	pthread_mutex_lock(&gen_lock);
	unsigned value = generation;
	pthread_mutex_unlock(&gen_lock);
	return value;
}

// Called on every path that gives up before a download exists: the id is
// published as soon as the turn is taken, so a failed open must not leave a
// track advertised as downloading.
static void clear_active(const char *id) {
	pthread_mutex_lock(&active_lock);
	if (strcmp(active_id, id) == 0) {
		active_id[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);
}

static void download_end(download_t *d, bool ok) {
	// Unpublish before closing: a wake on freed memory is worse than a wake on
	// a stream that is about to die.
	pthread_mutex_lock(&active_lock);
	if (active_stream == &d->stream) {
		active_stream = NULL;
	}
	if (strcmp(active_id, d->id) == 0) {
		active_id[0] = '\0';
	}
	pthread_mutex_unlock(&active_lock);

	if (d->f) {
		if (fclose(d->f) != 0) {
			ok = false; // the card may have refused the last flush
		}
		d->f = NULL;
	}
	http_stream_close(&d->stream);

	if (ok && d->total > 0 && d->done != d->total) {
		fprintf(stderr, "soundcloudcache: %s cut short at %ld of %ld bytes\n", d->id, d->done, d->total);
		ok = false;
	}
	if (ok && d->done == 0) {
		ok = false;
	}

	growfile_finish(d->path, ok);
	if (ok) {
		remove(d->marker);
		printf("soundcloudcache: %s complete (%ld KB)\n", d->id, d->done / 1024);
	} else {
		// Marker stays: soundcloudcache_has() will say no and the track will
		// be fetched again rather than played half through.
		printf("soundcloudcache: %s not completed\n", d->id);
	}

	// Release last. While the marker is there the track counts as incomplete,
	// so a waiter taking the turn here would start fetching it a second time.
	streamturn_release();
	free(d);
}

// Writes until `until` bytes are on disk, or to the end when until == 0.
static bool download_pump(download_t *d, long until, bool *finished) {
	char buf[FETCH_CHUNK];
	*finished = false;

	while (d->done < until || until == 0) {
		if (current_generation() != d->generation) {
			return false;
		}

		int n = http_stream_read(&d->stream, buf, (int)sizeof(buf));
		if (n < 0) {
			return false;
		}
		if (n == 0) {
			*finished = true;
			return true;
		}
		if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
			fprintf(stderr, "soundcloudcache: write failed (card full?)\n");
			return false;
		}

		// Flush before announcing: a reader must not look on disk for bytes
		// still sitting in the stdio buffer.
		fflush(d->f);
		d->done += n;
		growfile_progress(d->path, d->done);
	}
	return true;
}

// soundcloud.c keeps the access token to itself. The cache needs it for the
// one request to the API host before the CDN redirect, so read the same
// session file the backend writes. It saves that file with a rename on every
// renewal, so a read here sees a whole session, old or new, never half of one.
#define SESSION_FILE "soundcloud_session.ini"

static void session_token(char *out, size_t size) {
	out[0] = '\0';
	const char *cfg = config_path();
	if (!cfg || !*cfg) {
		return;
	}

	char path[640];
	const char *slash = strrchr(cfg, '/');
	if (slash) {
		snprintf(path, sizeof(path), "%.*s/%s", (int)(slash - cfg), cfg, SESSION_FILE);
	} else {
		snprintf(path, sizeof(path), "%s", SESSION_FILE);
	}

	FILE *f = fopen(path, "r");
	if (!f) {
		return; // no sign-in yet
	}
	char line[SOUNDCLOUD_TOKEN_MAX + 128];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq || strncmp(line, "access_token", (size_t)(eq - line)) != 0) {
			continue;
		}
		char *end = eq + 1;
		char *nl = strchr(end, '\n');
		if (nl) {
			*nl = '\0';
		}
		char *cr = strchr(end, '\r');
		if (cr) {
			*cr = '\0';
		}
		snprintf(out, size, "%s", end);
		break;
	}
	fclose(f);
}

static bool auth_headers(char *out, size_t size) {
	out[0] = '\0';
	char token[SOUNDCLOUD_TOKEN_MAX];
	session_token(token, sizeof(token));
	if (!token[0]) {
		return false;
	}
	int n = snprintf(out, size, "Authorization: OAuth %s\r\n", token);
	return n > 0 && (size_t)n < size;
}

// Rest of the download, after playback has started.
static void *download_thread(void *arg) {
	thread_be_background("soundcloud download");

	download_t *d = arg;
	bool finished = false;
	bool ok = download_pump(d, 0, &finished);
	download_end(d, ok && finished);
	return NULL;
}

bool soundcloudcache_start(const char *id, const soundcloud_stream_t *stream, int duration_secs, char *out,
						   size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	set_error("");

	if (!soundcloudcache_ready()) {
		set_error(tr("download_no_card"));
		return false;
	}
	if (!id || !*id || !stream || !stream->url[0]) {
		set_error(tr("download_failed"));
		return false;
	}

	// Already complete: no network and no wait.
	if (soundcloudcache_has(id, out, size)) {
		return true;
	}
	if (!ensure_dir()) {
		set_error(tr("cache_folder_failed"));
		return false;
	}

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error(tr("out_of_memory"));
		return false;
	}
	snprintf(d->id, sizeof(d->id), "%s", id);
	soundcloudcache_path(id, d->path, sizeof(d->path));
	marker_path(d->path, d->marker, sizeof(d->marker));

	// Tapping the playing track again means wanting to hear it, not restarting
	// it under the decoder that is already reading the file.
	if (growfile_is_growing(d->path)) {
		snprintf(out, size, "%.500s", d->path);
		free(d);
		return true;
	}

	// Prefetch calls this too. It must wait for the turn rather than abandon
	// the download feeding the track being played: only a user command abandons.
	streamturn_acquire();

	// Take the generation after the wait: a value read before it would be stale
	// by the time the turn arrives, and a track change during the wait would
	// cancel this download as soon as it starts.
	d->generation = current_generation();

	// Publish before the connection attempt. DNS, TCP and TLS can take seconds;
	// until the id is out, a caller asking "is this downloading?" gets no.
	pthread_mutex_lock(&active_lock);
	snprintf(active_id, sizeof(active_id), "%s", d->id);
	pthread_mutex_unlock(&active_lock);

	char headers[SOUNDCLOUD_TOKEN_MAX + 32];
	auth_headers(headers, sizeof(headers));
	if (!http_stream_open_ex(&d->stream, stream->url, headers[0] ? headers : NULL, FETCH_TIMEOUT_SECS)) {
		const char *why = http_last_error();
		fprintf(stderr, "soundcloudcache: %s does not open%s%s\n", id, why && why[0] ? ": " : "",
				why && why[0] ? why : "");
		set_error(why && why[0] ? why : tr("download_failed"));
		clear_active(d->id);
		streamturn_release();
		free(d);
		return false;
	}

	d->total = d->stream.content_length;
	prune(d->total > 0 ? d->total : 0);

	// Marker before the file: an orphan marker does nothing, while an
	// incomplete file that looks complete is much worse after a power cut.
	FILE *marker = fopen(d->marker, "wb");
	if (marker) {
		fclose(marker);
	}

	d->f = fopen(d->path, "wb");
	if (!d->f) {
		fprintf(stderr, "soundcloudcache: %s does not open for writing: %s\n", d->path, strerror(errno));
		set_error(errno == ENOSPC ? tr("card_full") : tr("card_write_failed"));
		http_stream_close(&d->stream);
		clear_active(d->id);
		streamturn_release();
		free(d);
		return false;
	}

	// From here an abandon must be able to shut this socket down.
	pthread_mutex_lock(&active_lock);
	active_stream = &d->stream;
	snprintf(active_id, sizeof(active_id), "%s", d->id);
	pthread_mutex_unlock(&active_lock);

	growfile_announce(d->path, d->total);

	// --- prebuffer, measured rather than guessed -----------------------------
	//
	// A fixed size cannot work: half a megabyte is a few seconds of a 128 kbit
	// MP3. What matters is whether the download keeps up with playback. Time
	// the first chunk to measure the network rate: with H bytes in hand and a
	// measured rate M against the track's rate R, the reserve is
	// H + (M - R)t, so not running out needs H >= total * (1 - M/R).
	long want = STREAM_PREBUFFER;
	if (d->total > 0 && d->total < want) {
		want = d->total;
	}

	long probe = want < STREAM_PROBE ? want : STREAM_PROBE;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	bool finished = false;
	if (!download_pump(d, probe, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (!finished && d->total > 0 && duration_secs > 0) {
		struct timespec t1;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

		// Under a third of a second the first chunk may have come straight out
		// of the socket buffer and says nothing about the network's real rate.
		double measured = elapsed > 0.3 ? d->done / elapsed : 0.0;
		double required = (double)d->total / duration_secs;

		// A few seconds of music in hand regardless of the measurement, so
		// the network's first hiccup does not stop playback right after it starts.
		if (required > 0) {
			long floor_bytes = (long)(required * STREAM_HEAD_SECS);
			if (floor_bytes > want) {
				want = floor_bytes;
			}
			if (want > d->total) {
				want = d->total;
			}
		}

		if (measured > 0 && required > 0) {
			// A fifth of margin: one chunk is a noisy measurement. Capped at a
			// few seconds, because on a slow network the formula asks for
			// nearly the whole file and that is minutes before the first note.
			double head = (double)d->total * (1.0 - measured / (required * 1.2));
			double cap = required * STREAM_HEAD_MAX_SECS;
			if (head > cap) {
				head = cap;
			}
			if (head > want) {
				want = (long)head;
			}
			if (want > d->total) {
				want = d->total;
			}
			printf("soundcloudcache: %s needs %.0f KB/s, the network gives %.0f -> starting with %ld KB of %ld\n", id,
				   required / 1024, measured / 1024, want / 1024, d->total / 1024);
		}
	}

	if (!finished && d->done < want && !download_pump(d, want, &finished)) {
		set_error(current_generation() != d->generation ? tr("download_cancelled") : tr("download_interrupted"));
		download_end(d, false);
		return false;
	}

	if (finished) {
		snprintf(out, size, "%.500s", d->path);
		download_end(d, true);
		return true;
	}

	snprintf(out, size, "%.500s", d->path);

	// Read both numbers before the thread starts: it can finish and free `d`
	// before the next line would run.
	long started_done = d->done;
	long started_total = d->total;

	pthread_t thread;
	if (pthread_create(&thread, NULL, download_thread, d) != 0) {
		set_error(tr("cannot_start_the_download"));
		download_end(d, false);
		out[0] = '\0';
		return false;
	}
	pthread_detach(thread);

	printf("soundcloudcache: %s starts at %ld KB of %ld KB\n", id, started_done / 1024,
		   started_total > 0 ? started_total / 1024 : 0);
	return true;
}
