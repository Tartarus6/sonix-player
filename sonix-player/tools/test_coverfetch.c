// The cover fetch client, on the host, against stubbed HTTP and the real JSON
// parser. Built and run by `make test-coverfetch`. Not part of host or target.

#include "src/system/library/coverfetch.h"
#include "src/system/library/coverfetch_test.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The HTTP layer, replaced here so no request leaves the process. The encoder
// mirrors the production one in http.c: every non-alphanumeric byte (spaces
// included) becomes %XX, so the query assertions test the real assembly.
static char stub_body[65536];
static char stub_url[2048];
static int stub_status = 200;
static int stub_calls = 0;
static char stub_last_error[256];
// Search requests are paced in coverfetch.c; record when the stub sees them so
// a case can check the spacing the real MusicBrainz API requires.
static int64_t stub_last_mb_ms = 0;
static int64_t stub_min_gap_ms = -1;

static int64_t stub_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// Downloads are binary and need an explicit length: a cover can contain NULs
// and can be far larger than the NUL-terminated JSON search response above.
static unsigned char *stub_download = NULL;
static size_t stub_download_len = 0;
static bool stub_download_mode = false;
// Batch testing: alternate a one-release answer with an empty one, so a run of
// jobs yields a found, a miss, a found, ... without driving each search by hand.
static bool stub_alternate = false;
static int stub_alt_search = 0;
static const char ALT_BODY_FOUND[] = "{\"releases\":[{\"id\":\"rel-alt\",\"title\":\"Album\"}]}";
static const char ALT_BODY_EMPTY[] = "{\"releases\":[]}";
static const unsigned char ALT_JPEG[16] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44};

// http.c reserves one byte of `limit` for the trailing NUL (body_reserve()
// stops growing at cap == limit and body_append() leaves room for it), so the
// largest out_len production can return is limit - 1. Mirror that here: a stub
// that always reports the full len hides the 4 MB cap off-by-one.
static size_t stub_clamp(size_t len, size_t limit) {
	if (limit == 0) {
		return 0;
	}
	size_t cap = limit - 1;
	return len < cap ? len : cap;
}

void coverfetch_test_set_alternating(bool on) {
	stub_alternate = on;
	stub_alt_search = 0;
}

bool http_get_ex(const char *url, const char *extra_headers, bool want_error_body, char **out,
				 size_t *out_len, size_t limit, int timeout_secs, int *status_out) {
	(void)extra_headers;
	(void)want_error_body;
	(void)timeout_secs;
	stub_calls++;
	snprintf(stub_url, sizeof(stub_url), "%s", url ? url : "");
	if (url && strstr(url, "musicbrainz.org") != NULL) {
		int64_t now = stub_now_ms();
		if (stub_last_mb_ms) {
			int64_t gap = now - stub_last_mb_ms;
			if (stub_min_gap_ms < 0 || gap < stub_min_gap_ms) {
				stub_min_gap_ms = gap;
			}
		}
		stub_last_mb_ms = now;
	}
	if (stub_alternate) {
		*status_out = 200;
		if (strstr(url, "coverartarchive.org") != NULL) {
			// Every image download is a valid JPEG.
			size_t len = stub_clamp(sizeof(ALT_JPEG), limit);
			*out = malloc(len + 1);
			if (!*out) {
				return false;
			}
			memcpy(*out, ALT_JPEG, len);
			(*out)[len] = '\0';
			if (out_len) {
				*out_len = len;
			}
			return true;
		}
		// Searches alternate: one release, then an empty "releases" array.
		bool found = (stub_alt_search++ % 2) == 0;
		const char *body = found ? ALT_BODY_FOUND : ALT_BODY_EMPTY;
		size_t len = stub_clamp(strlen(body), limit);
		*out = malloc(len + 1);
		if (!*out) {
			return false;
		}
		memcpy(*out, body, len);
		(*out)[len] = '\0';
		if (out_len) {
			*out_len = len;
		}
		return true;
	}
	*status_out = stub_status;
	if (stub_status < 200 || stub_status >= 300) {
		*out = NULL;
		return false;
	}
	if (stub_download_mode) {
		size_t len = stub_clamp(stub_download_len, limit);
		*out = malloc(len ? len : 1);
		if (!*out) {
			return false;
		}
		if (len) {
			memcpy(*out, stub_download, len);
		}
		if (out_len) {
			*out_len = len;
		}
		return true;
	}
	size_t len = stub_clamp(strlen(stub_body), limit);
	*out = malloc(len + 1);
	if (!*out) {
		return false;
	}
	memcpy(*out, stub_body, len);
	(*out)[len] = '\0';
	if (out_len) {
		*out_len = len;
	}
	return true;
}

const char *http_last_error(void) { return stub_last_error; }

char *http_url_encode(const char *in, char *out, size_t out_size) {
	static const char HEX[] = "0123456789ABCDEF";
	size_t o = 0;
	for (size_t i = 0; in && in[i] && o + 4 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];
		bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
					c == '_' || c == '.' || c == '~';
		if (safe) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = HEX[c >> 4];
			out[o++] = HEX[c & 0x0f];
		}
	}
	if (out_size) {
		out[o < out_size ? o : out_size - 1] = '\0';
	}
	return out;
}

// The hooks that set those stubs up. Implemented here beside the state they
// drive; coverfetch_test.h declares them.
void coverfetch_test_reset(void) {
	stub_body[0] = '\0';
	stub_url[0] = '\0';
	stub_status = 200;
	stub_calls = 0;
	stub_last_error[0] = '\0';
	free(stub_download);
	stub_download = NULL;
	stub_download_len = 0;
	stub_download_mode = false;
	stub_alternate = false;
	stub_alt_search = 0;
	stub_last_mb_ms = 0;
	stub_min_gap_ms = -1;
	coverfetch_negative_clear();
}

void coverfetch_test_set_body(const char *body) { snprintf(stub_body, sizeof(stub_body), "%s", body ? body : ""); }

void coverfetch_test_set_download(const void *data, size_t len) {
	free(stub_download);
	stub_download = NULL;
	stub_download_len = len;
	stub_download_mode = true;
	if (len) {
		stub_download = malloc(len);
		if (!stub_download) {
			printf("FAIL coverfetch_test_set_download: out of memory\n");
			exit(1);
		}
		memcpy(stub_download, data, len);
	}
}

void coverfetch_test_set_status(int status) { stub_status = status; }

int coverfetch_test_calls(void) { return stub_calls; }

int coverfetch_test_min_request_gap_ms(void) { return (int)stub_min_gap_ms; }

const char *coverfetch_test_url(void) { return stub_url; }

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

// Two MusicBrainz releases, enough to check record building and freeing.
static const char *MB_BODY =
	"{\"releases\":["
	"{\"id\":\"rel-aaa\",\"title\":\"Abbey Road\"},"
	"{\"id\":\"rel-bbb\",\"title\":\"Let It Be\"}"
	"]}";

// 1. The built query encodes every space, and is what search sends.
static void case_query_encodes_album_and_artist(void) {
	char q[512];
	coverfetch_mb_query(q, sizeof(q), "Abbey Road", "The Beatles");
	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	coverfetch_free(records, count);
	const char *pinned = "query=release:Abbey%20Road%20AND%20artist:The%20Beatles";
	CHECK(strcmp(q, pinned) == 0 && strstr(coverfetch_test_url(), pinned) != NULL,
		  "query_encodes_album_and_artist");
}

// 2. A 404 is an answer: one request, not found, nothing handed back.
static void case_four_xx_not_retried(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(404);
	coverfetch_record_t *records = (coverfetch_record_t *)1;
	int count = -1;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 1 && records == NULL && count == 0,
		  "four_xx_not_retried");
}

// 3. A 5xx is transient: three requests in all, then a network error.
static void case_five_xx_retries_thrice(void) {
	coverfetch_test_reset();
	coverfetch_test_set_status(500);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && coverfetch_test_calls() == 3, "five_xx_retries_thrice");
}

// 4. With neither album nor artist there is nothing to ask for.
static void case_no_tags_short_circuits(void) {
	coverfetch_test_reset();
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("", "", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NO_TAGS && coverfetch_test_calls() == 0, "no_tags_short_circuits");
}

// 5. An album already marked not found is skipped without a request.
static void case_negative_cache_skips_request(void) {
	coverfetch_test_reset();
	char key[640];
	coverfetch_key(key, sizeof(key), "Abbey Road", "The Beatles");
	coverfetch_negative_add(key);
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(coverfetch_negative_has(key) && r == COVERFETCH_NOT_FOUND && coverfetch_test_calls() == 0,
		  "negative_cache_skips_request");
}

// 6. A response that is not JSON is a network failure, not an empty result.
static void case_malformed_json_is_network(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{ this is not JSON");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	CHECK(r == COVERFETCH_NETWORK && records == NULL && count == 0, "malformed_json_is_network");
}

// 7. No result path frees the array, and a successful one frees through the API.
static void case_search_frees_records(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body("{\"releases\":[{\"title\":\"no id\"},{\"title\":\"also no id\"}]}");
	coverfetch_record_t *records = NULL;
	int count = 0;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	bool clean = r == COVERFETCH_NOT_FOUND && records == NULL && count == 0 &&
				 coverfetch_test_outstanding_records() == 0;

	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	r = coverfetch_search("Abbey Road", "The Beatles", "Come Together", &records, &count);
	clean = clean && r == COVERFETCH_OK && count == 2 && records != NULL &&
			strstr(records[1].url, "/rel-bbb/front-500") != NULL && records[0].size == 500;
	coverfetch_free(records, count);
	CHECK(clean && coverfetch_test_outstanding_records() == 0, "search_frees_records");
}

// ---------------------------------------------------------------------------
// Saving: a temp folder keeps every write out of the source tree. The dir hook
// in coverfetch.c sends all saves there, whatever track path the case passes.
// ---------------------------------------------------------------------------
#define SAVE_MAX_BYTES (4u * 1024u * 1024u)

static char save_dir[512];
static const unsigned char JPEG_BYTES[16] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0, 0, 0, 0, 0xAA, 0xBB, 0xCC, 0xDD};
static const unsigned char EXISTING_BYTES[20] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
											   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};

static void save_dir_setup(void) {
	snprintf(save_dir, sizeof(save_dir), "build_host/coverfetch_XXXXXX");
	if (!mkdtemp(save_dir)) {
		printf("FAIL could not create save dir\n");
		exit(1);
	}
}

static void save_target(char *out, size_t size, const char *suffix) {
	snprintf(out, size, "%s/cover.jpg%s", save_dir, suffix);
}

static bool save_file_exists(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	struct stat st;
	return stat(path, &st) == 0;
}

static bool save_part_exists(void) {
	const char *path = coverfetch_test_last_temp();
	struct stat st;
	return path && path[0] && stat(path, &st) == 0;
}

static bool save_file_equals(const unsigned char *want, size_t len) {
	char path[600];
	save_target(path, sizeof(path), "");
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	unsigned char got[64];
	size_t n = fread(got, 1, sizeof(got), f);
	bool closed = fclose(f) == 0;
	return closed && n == len && memcmp(got, want, len) == 0;
}

static void save_put_existing(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(EXISTING_BYTES, 1, sizeof(EXISTING_BYTES), f) != sizeof(EXISTING_BYTES) || fclose(f) != 0) {
		printf("FAIL could not seed existing cover\n");
		exit(1);
	}
}

static void save_record(coverfetch_record_t *r) {
	memset(r, 0, sizeof(*r));
	snprintf(r->url, sizeof(r->url), "https://coverartarchive.org/release/rel-aaa/front-500");
	snprintf(r->source, sizeof(r->source), "Abbey Road");
	r->size = 500;
}

static void save_reset(void) {
	char path[600];
	save_target(path, sizeof(path), "");
	remove(path);
	save_target(path, sizeof(path), ".coverfetch.part");
	remove(path);
	coverfetch_test_reset();
	coverfetch_test_set_dir(save_dir);
}

// 8. A short GIF8 pile fails the length check; HTML fails the magic check.
static void case_save_rejects_non_image(void) {
	coverfetch_record_t r;
	save_record(&r);

	save_reset();
	coverfetch_test_set_download("GIF8junk", 8);
	bool short_rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
						  !save_part_exists();

	save_reset();
	const char html[] = "<html><body>no cover here</body></html>";
	coverfetch_test_set_download(html, sizeof(html) - 1);
	bool html_rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
						 !save_part_exists();
	CHECK(short_rejected && html_rejected, "save_rejects_non_image");
}

// 9. Four megabytes plus one is too big, even with valid JPEG magic.
static void case_save_rejects_oversized_body(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();

	unsigned char *big = malloc(SAVE_MAX_BYTES + 1);
	if (!big) {
		printf("FAIL could not allocate oversized body\n");
		exit(1);
	}
	big[0] = 0xFF;
	big[1] = 0xD8;
	big[2] = 0xFF;
	memset(big + 3, 0x5A, SAVE_MAX_BYTES - 2);
	coverfetch_test_set_download(big, SAVE_MAX_BYTES + 1);
	free(big);

	bool rejected = !coverfetch_save("music/Album/track.mp3", "", &r, false) && !save_file_exists() &&
					!save_part_exists();
	CHECK(rejected, "save_rejects_oversized_body");
}

// 10. With replace = false an existing cover is left byte for byte alone.
static void case_save_preserves_existing_art(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();
	save_put_existing();
	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	coverfetch_save("music/Album/track.mp3", "", &r, false);
	CHECK(save_file_equals(EXISTING_BYTES, sizeof(EXISTING_BYTES)), "save_preserves_existing_art");
}

// 11. With replace = true the new image wins and no temp file is left behind.
static void case_save_replaces_when_asked(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();
	save_put_existing();
	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool saved = coverfetch_save("music/Album/track.mp3", "", &r, true);
	CHECK(saved && save_file_equals(JPEG_BYTES, sizeof(JPEG_BYTES)) && !save_part_exists(),
		  "save_replaces_when_asked");
}

// 12. Two saves to one folder pick different temp paths, so neither can
// rename the other's half-written bytes into cover.jpg.
static void case_save_temp_paths_are_unique(void) {
	coverfetch_record_t r;
	save_record(&r);
	save_reset();

	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool first = coverfetch_save("music/Album/track.mp3", "", &r, true);
	char first_temp[1200];
	snprintf(first_temp, sizeof(first_temp), "%s", coverfetch_test_last_temp());
	bool first_clean = first && first_temp[0] && !save_part_exists();

	coverfetch_test_set_download(JPEG_BYTES, sizeof(JPEG_BYTES));
	bool second = coverfetch_save("music/Album/track.mp3", "", &r, true);
	const char *second_temp = coverfetch_test_last_temp();

	CHECK(first_clean && second && second_temp[0] && strcmp(first_temp, second_temp) != 0 && !save_part_exists(),
		  "save_temp_paths_are_unique");
}

// 13. Cancelling from the progress callback stops after the job that reported
// done == 2, keeps the cover already saved, and makes no more HTTP calls.
typedef struct {
	int callbacks;
	coverfetch_progress_t last;
	int calls_at_cancel;
} batch_ctx_t;

static void batch_progress_cb(const coverfetch_progress_t *p, void *user) {
	batch_ctx_t *ctx = user;
	ctx->callbacks++;
	ctx->last = *p;
	if (p->done == 2) {
		ctx->calls_at_cancel = coverfetch_test_calls();
		coverfetch_batch_cancel();
	}
}

static void batch_cancel_stops_and_keeps_saved(void) {
	coverfetch_job_t jobs[6];
	memset(jobs, 0, sizeof(jobs));
	char albums[6][24];
	for (int i = 0; i < 6; i++) {
		snprintf(albums[i], sizeof(albums[i]), "Album %d", i);
		snprintf(jobs[i].album, sizeof(jobs[i].album), "%s", albums[i]);
		snprintf(jobs[i].artist, sizeof(jobs[i].artist), "Artist");
		snprintf(jobs[i].path, sizeof(jobs[i].path), "/tmp/music/Album %d/track.mp3", i);
	}

	save_reset();
	coverfetch_test_set_alternating(true);

	batch_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	coverfetch_batch_run(jobs, 6, batch_progress_cb, &ctx);

	// Job 1 (found) costs a search + a download, job 2 (miss) one search: three
	// calls by the time the callback cancels. Nothing may follow them.
	bool stopped = ctx.last.done == 2 && ctx.last.found == 1 && ctx.last.not_found == 1 && ctx.callbacks == 2;
	bool quiet = ctx.calls_at_cancel == 3 && coverfetch_test_calls() == 3;
	CHECK(stopped && quiet && save_file_exists(), "batch_cancel_stops_and_keeps_saved");
}

// 14. A second run attempted from inside a live worker's progress callback is
// refused: it must add no callback and no HTTP call, and the running guard must
// still describe the original run. This pins the overlap guard without threads.
typedef struct {
	int callbacks;
	int nested_callbacks;
	int calls_at_nested;
	bool running_at_nested;
} batch_guard_ctx_t;

static void batch_nested_cb(const coverfetch_progress_t *p, void *user) {
	(void)p;
	batch_guard_ctx_t *g = user;
	g->nested_callbacks++;
}

static void batch_guard_cb(const coverfetch_progress_t *p, void *user) {
	batch_guard_ctx_t *g = user;
	g->callbacks++;
	if (p->done != 1) {
		return;
	}
	coverfetch_job_t nested[2];
	memset(nested, 0, sizeof(nested));
	for (int i = 0; i < 2; i++) {
		snprintf(nested[i].album, sizeof(nested[i].album), "Nested %d", i);
		snprintf(nested[i].artist, sizeof(nested[i].artist), "Artist");
		snprintf(nested[i].path, sizeof(nested[i].path), "/tmp/music/Nested %d/track.mp3", i);
	}
	g->calls_at_nested = coverfetch_test_calls();
	g->running_at_nested = coverfetch_batch_running();
	coverfetch_batch_run(nested, 2, batch_nested_cb, g);
}

static void batch_overlap_is_refused(void) {
	coverfetch_job_t jobs[2];
	memset(jobs, 0, sizeof(jobs));
	for (int i = 0; i < 2; i++) {
		snprintf(jobs[i].album, sizeof(jobs[i].album), "Overlap %d", i);
		snprintf(jobs[i].artist, sizeof(jobs[i].artist), "Artist");
		snprintf(jobs[i].path, sizeof(jobs[i].path), "/tmp/music/Overlap %d/track.mp3", i);
	}

	save_reset();
	coverfetch_test_set_alternating(true);

	batch_guard_ctx_t g;
	memset(&g, 0, sizeof(g));
	coverfetch_batch_run(jobs, 2, batch_guard_cb, &g);

	// Job 1 costs a search + a download, job 2 only a search: three calls.
	// The nested run must add none of its own.
	bool refused = g.callbacks == 2 && g.nested_callbacks == 0 && g.running_at_nested &&
				   g.calls_at_nested == 2 && coverfetch_test_calls() == 3;
	CHECK(refused && !coverfetch_batch_running(), "batch_overlap_is_refused");
}

// 15. The negative cache is shared by the batch worker, the Now Playing search
// worker and the GUI retry clear. Concurrent adds must not lose keys or push
// negative_count past COVERFETCH_MAX_KEYS (which used to write past the array).
#define NEG_THREADS 8
#define NEG_PER_THREAD 31 // 248 keys, just below the table size

static void *negative_add_thread(void *arg) {
	int base = *(int *)arg;
	char key[640];
	for (int i = 0; i < NEG_PER_THREAD; i++) {
		snprintf(key, sizeof(key), "album-%d-%d|artist", base, i);
		coverfetch_negative_add(key);
	}
	return NULL;
}

static void case_negative_cache_concurrent_adds(void) {
	coverfetch_test_reset();
	pthread_t threads[NEG_THREADS];
	int bases[NEG_THREADS];
	for (int t = 0; t < NEG_THREADS; t++) {
		bases[t] = t;
		if (pthread_create(&threads[t], NULL, negative_add_thread, &bases[t]) != 0) {
			printf("FAIL could not spawn negative-cache thread\n");
			exit(1);
		}
	}
	for (int t = 0; t < NEG_THREADS; t++) {
		pthread_join(threads[t], NULL);
	}
	bool all = true;
	char key[640];
	for (int t = 0; t < NEG_THREADS && all; t++) {
		for (int i = 0; i < NEG_PER_THREAD; i++) {
			snprintf(key, sizeof(key), "album-%d-%d|artist", t, i);
			if (!coverfetch_negative_has(key)) {
				all = false;
				break;
			}
		}
	}
	// A clear after the storm and a fresh add must still leave a usable cache.
	coverfetch_negative_clear();
	coverfetch_negative_add("after|clear");
	CHECK(all && coverfetch_negative_has("after|clear") && !coverfetch_negative_has("album-0-0|artist"),
		  "negative_cache_concurrent_adds");
}

// 16. One tag alone is not a valid MusicBrainz query: it must short-circuit
// just like the no-tags case, with no request sent.
static void case_partial_tags_short_circuits(void) {
	coverfetch_test_reset();
	coverfetch_record_t *records = (coverfetch_record_t *)1;
	int count = -1;
	coverfetch_result_t r = coverfetch_search("Abbey Road", "", "Come Together", &records, &count);
	bool album_only = r == COVERFETCH_NO_TAGS && coverfetch_test_calls() == 0 && records == NULL && count == 0;

	coverfetch_test_reset();
	records = (coverfetch_record_t *)1;
	count = -1;
	r = coverfetch_search("", "The Beatles", "Come Together", &records, &count);
	bool artist_only = r == COVERFETCH_NO_TAGS && coverfetch_test_calls() == 0 && records == NULL && count == 0;
	CHECK(album_only && artist_only, "partial_tags_short_circuits");
}

// 17. Consecutive searches, including a batch's loop, leave at least a second
// between MusicBrainz requests -- the API's per-IP rate limit.
static void case_mb_requests_are_paced(void) {
	coverfetch_test_reset();
	coverfetch_test_set_body(MB_BODY);
	for (int i = 0; i < 3; i++) {
		char album[32];
		snprintf(album, sizeof(album), "Paced %d", i);
		coverfetch_record_t *records = NULL;
		int count = 0;
		coverfetch_search(album, "Artist", "", &records, &count);
		coverfetch_free(records, count);
	}
	CHECK(coverfetch_test_calls() == 3 && coverfetch_test_min_request_gap_ms() >= 1000, "mb_requests_are_paced");
}

int main(void) {
	save_dir_setup();
	case_query_encodes_album_and_artist();
	case_four_xx_not_retried();
	case_five_xx_retries_thrice();
	case_no_tags_short_circuits();
	case_partial_tags_short_circuits();
	case_negative_cache_skips_request();
	case_malformed_json_is_network();
	case_search_frees_records();
	case_save_rejects_non_image();
	case_save_rejects_oversized_body();
	case_save_preserves_existing_art();
	case_save_replaces_when_asked();
	case_save_temp_paths_are_unique();
	batch_cancel_stops_and_keeps_saved();
	batch_overlap_is_refused();
	case_negative_cache_concurrent_adds();
	case_mb_requests_are_paced();

	char path[600];
	save_target(path, sizeof(path), "");
	remove(path);
	save_target(path, sizeof(path), ".coverfetch.part");
	remove(path);
	rmdir(save_dir);
	coverfetch_test_reset();
	printf("all coverfetch cases passed\n");
	return 0;
}
