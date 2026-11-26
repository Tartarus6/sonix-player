#include "coverfetch.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/utils.h"
#include "src/system/library/albumart.h"
#include "src/system/library/library.h"
#include "src/system/net/http.h"

#ifndef COVERFETCH_MB_API
#define COVERFETCH_MB_API "https://musicbrainz.org/ws/2/release"
#endif
#define COVERFETCH_CAA "https://coverartarchive.org/release"
// A MusicBrainz search can keep the connection quiet for a while, and the TLS
// handshake on this CPU takes a few more seconds.
#define COVERFETCH_TIMEOUT_SECS 25
// 404 answers the search; 429 and 5xx (and no status at all) are transient and
// get a few more tries with a growing pause between them.
#define COVERFETCH_ATTEMPTS 3
#ifdef COVERFETCH_TEST
#define COVERFETCH_RETRY_PAUSE_SECS 0
#else
#define COVERFETCH_RETRY_PAUSE_SECS 2
#endif
// The JSON is small; a cap keeps a broken server from eating the heap.
#define COVERFETCH_MAX_BYTES (256 * 1024)
// An image is capped at 4 MB. http_get_ex() keeps one byte of the limit for the
// trailing NUL, so the largest out_len is limit - 1: ask for MAX + 2 to get
// MAX + 1 back, which the len > MAX check below rejects. MAX + 1 would yield
// exactly MAX and let an oversized body through as a truncated JPEG.
#define COVERFETCH_MAX_IMAGE_BYTES (4 * 1024 * 1024)
#define COVERFETCH_IMAGE_NAME "cover.jpg"
// PID plus a per-save serial keeps concurrent writers in different processes
// and threads from sharing one temp path in the same album folder.
#define COVERFETCH_PART_FORMAT ".coverfetch.%ld.%lu.part"
#define COVERFETCH_MAX_RESULTS 5
#define COVERFETCH_MAX_KEYS 256
// MusicBrainz allows on average one request per second per source IP and
// answers excess traffic with 503. Leave a little margin over a second.
#define COVERFETCH_MB_INTERVAL_MS 1100

// One request at a time across every caller, held for the whole attempt loop.
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
// Guarded by http_lock. Last MusicBrainz request, in CLOCK_MONOTONIC ms; 0
// before the first. Only searches are paced -- CAA image downloads go through
// save_impl() and are a different host.
static int64_t mb_last_ms = 0;

static int64_t mono_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Sleep out the remainder of the interval since the last MusicBrainz request.
// Called under http_lock immediately before each attempt, so retry pauses count
// towards the spacing too.
static void mb_pace(void) {
	int64_t now = mono_ms();
	if (mb_last_ms) {
		int64_t wait = (int64_t)COVERFETCH_MB_INTERVAL_MS - (now - mb_last_ms);
		if (wait > 0) {
			struct timespec nap = {wait / 1000, (long)(wait % 1000) * 1000000L};
			nanosleep(&nap, NULL);
			now = mono_ms();
		}
	}
	mb_last_ms = now;
}
// Guarded by http_lock, already taken for the request. Only the name is
// serialised; the file write itself still happens after the unlock.
static unsigned long save_seq = 0;

// "not found" keys. A fixed table is enough for a library of a few hundred
// albums; when it fills, the oldest miss is dropped. The batch worker, the Now
// Playing search worker and the GUI retry clear can all touch this at once, so
// every access is guarded by its own mutex (http_lock covers HTTP only).
static pthread_mutex_t negative_lock = PTHREAD_MUTEX_INITIALIZER;
static char negative_keys[COVERFETCH_MAX_KEYS][640];
static int negative_count = 0;

// Live search/save operations, counted separately from the batch slot so a
// single foreground fetch also holds Wi-Fi and suspend off. Its own mutex keeps
// coverfetch_busy() from blocking behind a request that is sleeping on http_lock.
static pthread_mutex_t activity_lock = PTHREAD_MUTEX_INITIALIZER;
static int activity_count = 0;

static void activity_enter(void) {
	pthread_mutex_lock(&activity_lock);
	activity_count++;
	pthread_mutex_unlock(&activity_lock);
}

static void activity_leave(void) {
	pthread_mutex_lock(&activity_lock);
	activity_count--;
	pthread_mutex_unlock(&activity_lock);
}

#ifdef COVERFETCH_TEST
static int records_outstanding = 0;
static char test_save_dir[1024];
static char test_save_temp[1200];

int coverfetch_test_outstanding_records(void) { return records_outstanding; }

void coverfetch_test_set_dir(const char *dir) {
	snprintf(test_save_dir, sizeof(test_save_dir), "%s", dir ? dir : "");
}

const char *coverfetch_test_last_temp(void) { return test_save_temp; }
#endif

// Called with negative_lock held, so the add path can check for a duplicate
// without trying to take the same non-recursive mutex twice.
static bool negative_has_unlocked(const char *key) {
	for (int i = 0; i < negative_count; i++) {
		if (strcmp(negative_keys[i], key) == 0) {
			return true;
		}
	}
	return false;
}

bool coverfetch_negative_has(const char *key) {
	if (!key || !key[0]) {
		return false;
	}
	pthread_mutex_lock(&negative_lock);
	bool found = negative_has_unlocked(key);
	pthread_mutex_unlock(&negative_lock);
	return found;
}

void coverfetch_negative_add(const char *key) {
	if (!key || !key[0]) {
		return;
	}
	pthread_mutex_lock(&negative_lock);
	if (!negative_has_unlocked(key)) {
		if (negative_count >= COVERFETCH_MAX_KEYS) {
			memmove(negative_keys, negative_keys + 1, sizeof(negative_keys) - sizeof(negative_keys[0]));
			negative_count = COVERFETCH_MAX_KEYS - 1; // defensive clamp before the write
		}
		if (negative_count < COVERFETCH_MAX_KEYS) {
			snprintf(negative_keys[negative_count], sizeof(negative_keys[0]), "%s", key);
			negative_count++;
		}
	}
	pthread_mutex_unlock(&negative_lock);
}

void coverfetch_negative_clear(void) {
	pthread_mutex_lock(&negative_lock);
	negative_count = 0;
	pthread_mutex_unlock(&negative_lock);
}

void coverfetch_key(char *out, size_t size, const char *album, const char *artist) {
	if (!size) {
		return;
	}
	const char *parts[2] = {album, artist};
	size_t o = 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass) {
			if (o + 1 >= size) {
				break;
			}
			out[o++] = '|';
		}
		const char *s = parts[pass] ? parts[pass] : "";
		for (size_t i = 0; s[i] && o + 1 < size; i++) {
			out[o++] = (char)tolower((unsigned char)s[i]);
		}
	}
	out[o] = '\0';
}

void coverfetch_mb_query(char *out, size_t size, const char *album, const char *artist) {
	char a[768], r[768];
	http_url_encode(album ? album : "", a, sizeof(a));
	http_url_encode(artist ? artist : "", r, sizeof(r));
	// The two separator spaces are encoded too: http_get_ex() puts the URL
	// straight into the request line, where a literal space would split it.
	snprintf(out, size, "query=release:%s%%20AND%%20artist:%s", a, r);
}

static coverfetch_record_t *records_alloc(int n) {
	coverfetch_record_t *p = calloc((size_t)n, sizeof(*p));
#ifdef COVERFETCH_TEST
	if (p) {
		records_outstanding++;
	}
#endif
	return p;
}

// GET with the retry rules above. Called with the lock held by api_get().
static bool request(const char *url, char **body, int *status) {
	for (int attempt = 1; attempt <= COVERFETCH_ATTEMPTS; attempt++) {
		*status = 0;
		mb_pace();
		if (http_get_ex(url, NULL, false, body, NULL, COVERFETCH_MAX_BYTES, COVERFETCH_TIMEOUT_SECS, status)) {
			return true;
		}
		bool transient = *status <= 0 || *status == 429 || *status >= 500;
#ifndef COVERFETCH_TEST
		const char *why = http_last_error();
		fprintf(stderr, "coverfetch: attempt %d/%d failed (HTTP %d%s%s)\n", attempt, COVERFETCH_ATTEMPTS, *status,
				why ? ": " : "", why ? why : "");
#endif
		if (!transient || attempt == COVERFETCH_ATTEMPTS) {
			break;
		}
		sleep((unsigned)(COVERFETCH_RETRY_PAUSE_SECS * attempt));
	}
	return false;
}

static bool api_get(const char *url, char **body, int *status) {
	pthread_mutex_lock(&http_lock);
	bool ok = request(url, body, status);
	pthread_mutex_unlock(&http_lock);
	return ok;
}

static coverfetch_result_t search_impl(const char *album, const char *artist, const char *title,
									   coverfetch_record_t **records, int *count) {
	(void)title; // kept for disambiguation when the result list is ranked
	*records = NULL;
	*count = 0;

	// The query needs both clauses: with only one of them it is not a valid
	// Lucene query, and the UI already promises a useful answer only when both
	// tags are present.
	if ((!album || !album[0]) || (!artist || !artist[0])) {
		return COVERFETCH_NO_TAGS;
	}

	char key[640];
	coverfetch_key(key, sizeof(key), album, artist);
	if (coverfetch_negative_has(key)) {
		return COVERFETCH_NOT_FOUND;
	}

	char query[2400];
	coverfetch_mb_query(query, sizeof(query), album, artist);
	char url[3200];
	int n = snprintf(url, sizeof(url), "%s/?%s&fmt=json&limit=%d", COVERFETCH_MB_API, query, COVERFETCH_MAX_RESULTS);
	if (n < 0 || (size_t)n >= sizeof(url)) {
		return COVERFETCH_NETWORK;
	}

	char *body = NULL;
	int status = 0;
	if (!api_get(url, &body, &status)) {
		free(body);
		if (status == 404) {
			coverfetch_negative_add(key);
			return COVERFETCH_NOT_FOUND;
		}
		return COVERFETCH_NETWORK;
	}

	coverfetch_result_t result = COVERFETCH_NETWORK;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int root = json_root(&doc);
		if (root >= 0 && doc.toks[root].type == JSON_OBJECT) {
			int releases = json_get(&doc, root, "releases");
			if (releases >= 0 && doc.toks[releases].type == JSON_ARRAY) {
				// A parsed releases array is an answer: no usable records in it
				// means "not found", not a network failure.
				result = COVERFETCH_NOT_FOUND;
				int total = json_len(&doc, releases);
				if (total > COVERFETCH_MAX_RESULTS) {
					total = COVERFETCH_MAX_RESULTS;
				}
				coverfetch_record_t *list = total > 0 ? records_alloc(total) : NULL;
				int found = 0;
				char id[128];
				for (int i = 0; list && i < total; i++) {
					int release = json_at(&doc, releases, i);
					if (release < 0 || doc.toks[release].type != JSON_OBJECT) {
						continue;
					}
					id[0] = '\0';
					json_obj_str(&doc, release, "id", id, sizeof(id));
					if (!id[0]) {
						continue;
					}
					coverfetch_record_t *r = &list[found];
					snprintf(r->url, sizeof(r->url), "%s/%s/front-500", COVERFETCH_CAA, id);
					if (!json_obj_str(&doc, release, "title", r->source, sizeof(r->source)) || !r->source[0]) {
						snprintf(r->source, sizeof(r->source), "%s", "MusicBrainz");
					}
					r->size = 500;
					found++;
				}
				if (found > 0) {
					*records = list;
					*count = found;
					result = COVERFETCH_OK;
				} else {
					coverfetch_free(list, total);
				}
			}
		}
		json_free(&doc);
	}
	free(body);

	if (result == COVERFETCH_NOT_FOUND) {
		coverfetch_negative_add(key);
	}
#ifndef COVERFETCH_TEST
	fprintf(stderr, "coverfetch: '%s' / '%s': %d\n", album ? album : "", artist ? artist : "", (int)result);
#endif
	return result;
}

coverfetch_result_t coverfetch_search(const char *album, const char *artist, const char *title,
									  coverfetch_record_t **records, int *count) {
	activity_enter();
	coverfetch_result_t result = search_impl(album, artist, title, records, count);
	activity_leave();
	return result;
}

void coverfetch_free(coverfetch_record_t *records, int count) {
	(void)count;
	if (!records) {
		return;
	}
#ifdef COVERFETCH_TEST
	records_outstanding--;
#endif
	free(records);
}

// Same rules as albumart.c:43-61, whose copy is file-local. Only these three
// families reach the image decoder, and a real image needs at least 12 bytes.
static bool is_supported_image(const uint8_t *data, size_t size) {
	if (!data || size < 12) {
		return false;
	}
	if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
		return true;
	}
	static const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(data, png_magic, sizeof(png_magic)) == 0) {
		return true;
	}
	if (memcmp(data, "GIF8", 4) == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
		return true;
	}
	return false;
}

static bool save_impl(const char *track_path, const char *dir_path, const coverfetch_record_t *record, bool replace) {
	if (!record || !record->url[0]) {
		return false;
	}

	char derived[1024];
	const char *dir = dir_path;
#ifdef COVERFETCH_TEST
	if (test_save_dir[0]) {
		dir = test_save_dir;
	}
#endif
	if (!dir || !dir[0]) {
		if (!track_path || !track_path[0]) {
			return false;
		}
		const char *slash = strrchr(track_path, '/');
		if (!slash) {
			dir = ".";
		} else {
			size_t n = (size_t)(slash - track_path);
			if (n >= sizeof(derived)) {
				return false;
			}
			memcpy(derived, track_path, n);
			derived[n] = '\0';
			dir = derived;
		}
	}

	char target[1200];
	int n = snprintf(target, sizeof(target), "%s/%s", dir, COVERFETCH_IMAGE_NAME);
	if (n < 0 || (size_t)n >= sizeof(target)) {
		return false;
	}

	// Gap protection first: no request at all when there is nothing to fill.
	struct stat st;
	if (!replace && stat(target, &st) == 0) {
		return false;
	}

	// The same one-request-at-a-time rule as the search, held only for the
	// request itself -- writing the file must not block another caller.
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	unsigned long seq;
	pthread_mutex_lock(&http_lock);
	seq = ++save_seq;
	bool ok = http_get_ex(record->url, NULL, false, &body, &len, COVERFETCH_MAX_IMAGE_BYTES + 2,
						  COVERFETCH_TIMEOUT_SECS, &status);
	pthread_mutex_unlock(&http_lock);
	if (!ok || !body || len < 12 || len > COVERFETCH_MAX_IMAGE_BYTES ||
		!is_supported_image((const uint8_t *)body, len)) {
		free(body);
		return false;
	}

	char temp[1200];
	n = snprintf(temp, sizeof(temp), "%s" COVERFETCH_PART_FORMAT, target, (long)getpid(), seq);
	if (n < 0 || (size_t)n >= sizeof(temp)) {
		free(body);
		return false;
	}
#ifdef COVERFETCH_TEST
	snprintf(test_save_temp, sizeof(test_save_temp), "%s", temp);
#endif

	FILE *f = fopen(temp, "wb");
	if (!f) {
		free(body);
		return false;
	}
	bool good = fwrite(body, 1, len, f) == len;
	if (good) {
		good = fflush(f) == 0;
	}
	if (good) {
		good = fsync(fileno(f)) == 0;
	}
	if (fclose(f) != 0) {
		good = false;
	}
	free(body);

	if (!good || rename(temp, target) != 0) {
		remove(temp);
		return false;
	}
	return true;
}

bool coverfetch_save(const char *track_path, const char *dir_path, const coverfetch_record_t *record, bool replace) {
	activity_enter();
	bool saved = save_impl(track_path, dir_path, record, replace);
	activity_leave();
	return saved;
}

// ---------------------------------------------------------------------------
// Batch download
//
// One worker walks the jobs in order, sharing the single request lock with the
// other callers. It copies the jobs so a page that started the batch can go
// away without leaving the worker holding freed memory. Progress goes through
// the caller's callback, which posts it to the interface thread.
// ---------------------------------------------------------------------------

typedef struct {
	coverfetch_job_t *jobs;
	int count;
	coverfetch_progress_cb cb;
	void *user;
	volatile bool cancel; // this run's token; a new run never resets it
} batch_ctx_t;

// One batch at a time. The pointer is both the running guard and the way cancel
// reaches the live run. A cancelled worker may still be inside a request for
// seconds; until it exits it owns this slot, so no second worker can start.
static pthread_mutex_t batch_lock = PTHREAD_MUTEX_INITIALIZER;
static batch_ctx_t *batch_current; // guarded by batch_lock
// Optional UI-supplied guard, consulted immediately before each batch save.
static bool (*batch_save_gate)(void); // guarded by batch_lock

void coverfetch_batch_set_save_gate(bool (*gate)(void)) {
	pthread_mutex_lock(&batch_lock);
	batch_save_gate = gate;
	pthread_mutex_unlock(&batch_lock);
}

bool coverfetch_batch_running(void) {
	pthread_mutex_lock(&batch_lock);
	bool running = batch_current != NULL;
	pthread_mutex_unlock(&batch_lock);
	return running;
}

bool coverfetch_busy(void) {
	if (coverfetch_batch_running()) {
		return true;
	}
	pthread_mutex_lock(&activity_lock);
	bool busy = activity_count > 0;
	pthread_mutex_unlock(&activity_lock);
	return busy;
}

void coverfetch_batch_cancel(void) {
	pthread_mutex_lock(&batch_lock);
	if (batch_current) {
		batch_current->cancel = true;
	}
	pthread_mutex_unlock(&batch_lock);
}

#ifndef COVERFETCH_TEST
// An album row already carries one representative track in `path` (see
// library.c:2482). Use it directly: library_for_each() holds the index lock
// while calling back, so a nested walk here would deadlock.
static bool missing_album_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	int *missing = user;
	if (!path || !path[0]) {
		return true;
	}
	albumart_t art;
	memset(&art, 0, sizeof(art));
	bool has_art = albumart_load_for_file(path, &art);
	albumart_free(&art);
	if (!has_art) {
		(*missing)++;
	}
	return true;
}

int coverfetch_batch_missing(void) {
	int missing = 0;
	library_for_each(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, missing_album_cb, &missing);
	return missing;
}
#else
// The host test drives jobs directly and does not pull in the library.
int coverfetch_batch_missing(void) { return 0; }
#endif

// Give the slot back on every exit path, then drop this run's memory.
static void batch_release(batch_ctx_t *ctx) {
	pthread_mutex_lock(&batch_lock);
	if (batch_current == ctx) {
		batch_current = NULL;
	}
	pthread_mutex_unlock(&batch_lock);
	free(ctx->jobs);
	free(ctx);
}

static void *batch_worker(void *arg) {
	batch_ctx_t *ctx = arg;
#ifndef COVERFETCH_TEST
	thread_be_background("cover fetch");
#endif

	coverfetch_progress_t p;
	memset(&p, 0, sizeof(p));
	p.total = ctx->count;

	for (int i = 0; i < ctx->count; i++) {
		if (ctx->cancel) {
			break;
		}
		const coverfetch_job_t *job = &ctx->jobs[i];
		char key[640];
		coverfetch_key(key, sizeof(key), job->album, job->artist);
		if (coverfetch_negative_has(key)) {
			// An earlier run already asked about this album and got nothing.
			p.not_found++;
		} else {
			coverfetch_record_t *records = NULL;
			int count = 0;
			coverfetch_result_t r = coverfetch_search(job->album, job->artist, "", &records, &count);
			bool saved = false;
			if (r == COVERFETCH_OK && count > 0) {
				pthread_mutex_lock(&batch_lock);
				bool (*gate)(void) = batch_save_gate;
				pthread_mutex_unlock(&batch_lock);
				if (gate && !gate()) {
					// The card went away or was exported mid-run: writing now
					// would drop cover.jpg on internal storage. Abort the rest.
					p.aborted = true;
				} else {
					saved = coverfetch_save(job->path, job->dir, &records[0], false);
				}
			}
			if (p.aborted) {
				coverfetch_free(records, count);
				break;
			}
			if (saved) {
				p.found++;
			} else {
				p.not_found++;
			}
			coverfetch_free(records, count);
		}
		p.done++;

		// Check again after the blocking work: a cancel during the request
		// stops here rather than posting an answer nobody wants.
		if (ctx->cancel) {
			break;
		}
		if (ctx->cb) {
			p.cancelled = ctx->cancel;
			ctx->cb(&p, ctx->user);
		}
	}

	// A gate refusal breaks the loop above without a normal progress post, so
	// send one final post carrying the abort reason for the UI to act on.
	if (p.aborted && ctx->cb) {
		p.cancelled = false;
		ctx->cb(&p, ctx->user);
	}

	batch_release(ctx);
	return NULL;
}

bool coverfetch_batch_run(const coverfetch_job_t *jobs, int count, coverfetch_progress_cb cb, void *user) {
	if (!jobs || count <= 0) {
		return false;
	}

	batch_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return false;
	}
	ctx->jobs = calloc((size_t)count, sizeof(*ctx->jobs));
	if (!ctx->jobs) {
		free(ctx);
		return false;
	}
	memcpy(ctx->jobs, jobs, (size_t)count * sizeof(*jobs));
	ctx->count = count;
	ctx->cb = cb;
	ctx->user = user;

	// Claim the slot only if the previous run has really exited. Rejecting here
	// keeps a late cancel from reviving a worker that is still winding down.
	pthread_mutex_lock(&batch_lock);
	if (batch_current) {
		pthread_mutex_unlock(&batch_lock);
		free(ctx->jobs);
		free(ctx);
		return false;
	}
	batch_current = ctx;
	pthread_mutex_unlock(&batch_lock);

#ifdef COVERFETCH_TEST
	// Deterministic for the host test: run to completion on this thread so the
	// case can inspect the final progress and the stub's call count directly.
	batch_worker(ctx); // releases the slot through batch_release()
#else
	pthread_t thread;
	if (pthread_create(&thread, NULL, batch_worker, ctx) != 0) {
		// No worker will run to release the slot, so do it here.
		pthread_mutex_lock(&batch_lock);
		if (batch_current == ctx) {
			batch_current = NULL;
		}
		pthread_mutex_unlock(&batch_lock);
		free(ctx->jobs);
		free(ctx);
		return false;
	}
	pthread_detach(thread);
#endif
	return true;
}
