#include "podcastauto.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifitransfer.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastdl.h"
#include "src/system/streaming/podcaststate.h"
#include "src/system/streaming/podcastsubs.h"

// The options are one line per followed podcast in device_config.ini:
//
//   [podcast_auto]
//   9123456789012345 = 1 1 10
//
// config.h limits keys to 32 characters and values to 128; a 64-bit feed id
// and three small numbers fit both with room to spare. Keeping this out of
// podcast.db is the point: podcastsubs.c owns that database and other work is
// changing its schema, so a settings patch here must not touch it.

#define AUTO_SECTION "podcast_auto"
#define AUTO_EPISODES 32 // one page of a feed per sync
#define AUTO_BUSY_WAIT_MS 250
#define AUTO_BUSY_TRIES 2400 // ten minutes: an episode is an hour long at most
#define AUTO_CANCEL_TRIES 40 // ten seconds for a cancelled job to wind down

void podcastauto_get(long long feed_id, podcastauto_opts_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (feed_id <= 0) {
		return;
	}
	char key[32];
	char value[64];
	snprintf(key, sizeof(key), "%lld", feed_id);
	const char *stored = config_get(AUTO_SECTION, key, "");
	if (!stored) {
		return;
	}
	snprintf(value, sizeof(value), "%s", stored);
	int enabled = 0;
	int delete_played = 0;
	int limit = 0;
	if (sscanf(value, "%d %d %d", &enabled, &delete_played, &limit) == 3) {
		out->enabled = enabled != 0;
		out->delete_played = delete_played != 0;
		out->limit = limit > 0 ? limit : 0;
		if (out->limit > PODCASTSUBS_MAX) {
			out->limit = PODCASTSUBS_MAX;
		}
	}
}

void podcastauto_set(long long feed_id, const podcastauto_opts_t *opts) {
	if (feed_id <= 0 || !opts) {
		return;
	}
	char key[32];
	char value[64];
	snprintf(key, sizeof(key), "%lld", feed_id);
	snprintf(value, sizeof(value), "%d %d %d", opts->enabled ? 1 : 0, opts->delete_played ? 1 : 0,
			 opts->limit > 0 ? opts->limit : 0);
	config_set(AUTO_SECTION, key, value);
	config_save(); // a setting lost at the next power-off is no setting at all
}

// ---------------------------------------------------------------------------
// deleting a kept episode
// ---------------------------------------------------------------------------

// podcastdl marks the sidecar of every file it writes. A state row or a copied
// .tags file is not proof of ownership: a hand-copied episode in the same
// folder gains both as soon as it is played or copied with its sidecar.
static bool podcastdl_downloaded(const char *path) {
	char marker[32];
	return podcastcache_tag(path, "downloaded_by", marker, sizeof(marker)) && strcmp(marker, "podcastdl") == 0;
}

void podcastauto_delete_file(const char *path) {
	if (!path || !path[0] || !podcastdl_owns(path) || !podcastdl_downloaded(path)) {
		return;
	}
	// The state row stays: an episode of the catalogue keeps its heard mark
	// and its place on the podcast page when its file goes away. A heard row
	// is not in "Continue listening" (that list takes unplayed positions).
	char side[900];
	remove(path);
	snprintf(side, sizeof(side), "%s.tags", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.jpg", path);
	remove(side);
	snprintf(side, sizeof(side), "%s.png", path);
	remove(side);
	sync();
	printf("podcastauto: removed %s\n", path);
}

// An episode is marked heard podcaststate_margin() seconds (30 by default)
// before its end, so deleting there would unlink a file the decoder still has
// open. Park such a delete and let podcastauto_on_stopped() release it from
// player.c when that episode really stops.
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static char pending_path[512];
static long long pending_feed;

static bool playing_now(const char *path) {
	podcaststate_t state;
	return podcaststate_get(path, &state) && state.touched == PODCASTSTATE_TOUCHED_NOW;
}

void podcastauto_on_played(const char *path, long long feed_id) {
	if (!path || !path[0] || feed_id <= 0 || !podcastdl_owns(path)) {
		return;
	}
	podcastauto_opts_t opts;
	podcastauto_get(feed_id, &opts);
	if (!opts.delete_played) {
		return;
	}
	if (playing_now(path)) {
		pthread_mutex_lock(&pending_lock);
		snprintf(pending_path, sizeof(pending_path), "%s", path);
		pending_feed = feed_id;
		pthread_mutex_unlock(&pending_lock);
		return;
	}
	podcastauto_delete_file(path);
}

void podcastauto_on_stopped(const char *path) {
	pthread_mutex_lock(&pending_lock);
	if (!pending_path[0]) {
		pthread_mutex_unlock(&pending_lock);
		return;
	}
	// An empty path means playback ended; a different one means another
	// episode took over, so the parked delete waits for its own stop.
	if (path && path[0] && strcmp(pending_path, path) != 0) {
		pthread_mutex_unlock(&pending_lock);
		return;
	}
	char to_delete[sizeof(pending_path)];
	snprintf(to_delete, sizeof(to_delete), "%s", pending_path);
	long long feed = pending_feed;
	pending_path[0] = '\0';
	pending_feed = 0;
	pthread_mutex_unlock(&pending_lock);

	podcastauto_opts_t opts;
	podcastauto_get(feed, &opts);
	if (opts.delete_played) {
		podcastauto_delete_file(to_delete);
	}
}

// ---------------------------------------------------------------------------
// the sweep
// ---------------------------------------------------------------------------

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static bool syncing;

typedef struct {
	char *path;
	bool played;
	long long touched;
	bool owned; // podcastdl wrote it: its sidecar carries the download marker
} kept_t;

typedef struct {
	kept_t *items;
	int count;
	int capacity;
} kept_list_t;

static void kept_free(kept_list_t *list) {
	for (int i = 0; i < list->count; i++) {
		free(list->items[i].path);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static void kept_add(kept_list_t *list, const char *path) {
	if (list->count == list->capacity) {
		int grown = list->capacity ? list->capacity * 2 : 32;
		kept_t *bigger = realloc(list->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return;
		}
		list->items = bigger;
		list->capacity = grown;
	}
	char *copy = strdup(path);
	if (!copy) {
		return;
	}
	list->items[list->count].path = copy;
	list->items[list->count].played = false;
	list->items[list->count].touched = 0;
	// Only files podcastdl actually downloaded may be trimmed. A state row or
	// an episode-id sidecar is not enough: a hand-copied file gains both once
	// it is played, so ownership is taken from the downloader's own marker.
	list->items[list->count].owned = podcastdl_downloaded(path);
	list->count++;
}

static bool kept_file(const char *name) {
	if (!name || !name[0] || name[0] == '.') {
		return false;
	}
	const char *dot = strrchr(name, '.');
	if (!dot) {
		return false;
	}
	return strcasecmp(dot, ".tags") != 0 && strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".png") != 0;
}

static void kept_read_folder(const char *folder, kept_list_t *list) {
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (!kept_file(de->d_name)) {
			continue;
		}
		char path[900];
		snprintf(path, sizeof(path), "%s/%s", folder, de->d_name);
		kept_add(list, path);
	}
	closedir(dir);
}

static void kept_cb(const podcaststate_t *state, void *user) {
	kept_list_t *list = user;
	if (!state->path[0]) {
		return;
	}
	for (int i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].path, state->path) == 0) {
			list->items[i].played = state->played;
			list->items[i].touched = state->touched;
			// A state row only says the file was played, not who put it here.
			return;
		}
	}
}

static void kept_drop_unowned(kept_list_t *list) {
	int kept = 0;
	for (int i = 0; i < list->count; i++) {
		if (!list->items[i].owned) {
			free(list->items[i].path);
			list->items[i].path = NULL;
			continue;
		}
		if (kept != i) {
			list->items[kept] = list->items[i];
		}
		kept++;
	}
	list->count = kept;
}

static int kept_cmp(const void *a, const void *b) {
	const kept_t *x = a;
	const kept_t *y = b;
	// Heard episodes go first; among those, the oldest. Files without a state
	// row have already been dropped as hand-copied and never reach here.
	if (x->played != y->played) {
		return x->played ? -1 : 1;
	}
	if (x->touched != y->touched) {
		return x->touched < y->touched ? -1 : 1;
	}
	return strcmp(x->path, y->path);
}

// `protect` is the file a successful download just added: it must not be the
// one removed to make room for itself.
static void kept_trim(kept_list_t *list, int limit, const char *protect) {
	if (list->count <= 0) {
		return;
	}
	// A negative limit means "leave that many fewer than there are": -1 frees
	// one slot at the cap so the sweep can bring in a new episode.
	int target = limit < 0 ? list->count + limit : limit;
	if (target < 0) {
		target = 0;
	}
	if (list->count <= target) {
		return;
	}
	// The episode playing now carries PODCASTSTATE_TOUCHED_NOW. Removing that
	// file under the decoder is not worth the risk, so it is left for later.
	qsort(list->items, (size_t)list->count, sizeof(*list->items), kept_cmp);
	int removed = 0;
	for (int i = 0; i < list->count && list->count - removed > target; i++) {
		kept_t *item = &list->items[i];
		if (item->touched == PODCASTSTATE_TOUCHED_NOW || (protect && strcmp(item->path, protect) == 0)) {
			continue;
		}
		podcastauto_delete_file(item->path);
		free(item->path);
		item->path = NULL;
		removed++;
	}
	int kept = 0;
	for (int i = 0; i < list->count; i++) {
		if (!list->items[i].path) {
			continue;
		}
		if (kept != i) {
			list->items[kept] = list->items[i];
		}
		kept++;
	}
	list->count = kept;
}

static bool dl_idle(void) {
	// podcastdl has one global slot. A finished job whose result the GUI queue
	// has not acknowledged yet is still the GUI's: starting over it would
	// replace DONE with RUNNING and steal its completion toast.
	return podcastdl_state(NULL, NULL) == PODCASTDL_IDLE;
}

// Wait for the single download slot. `ours` means the slot holds this sweep's
// job `job_id`: a Wi-Fi transfer switched on mid-download must stop that job at
// once, not let it keep writing to the card for the rest of a ten-minute wait.
// Cancellation is asynchronous -- download_main() sets PODCASTDL_CANCELLED only
// when its thread winds up -- so after cancelling we wait briefly for that
// terminal state. The caller then acknowledges a terminal result; acknowledging
// a still-RUNNING slot is a no-op that would leave it stuck and stall later
// sweeps. A foreign slot is never cancelled: it is left to its owner and only
// waited on until it goes IDLE (or the transfer comes on). Returns the last
// state seen.
static podcastdl_state_t wait_for_download(bool ours, long long job_id) {
	for (int i = 0; i < AUTO_BUSY_TRIES; i++) {
		if (wifitransfer_get_enabled()) {
			if (ours && podcastdl_job() == job_id) {
				podcastdl_cancel();
				for (int j = 0; j < AUTO_CANCEL_TRIES && podcastdl_job() == job_id &&
						 podcastdl_state(NULL, NULL) == PODCASTDL_RUNNING;
					 j++) {
					usleep(AUTO_BUSY_WAIT_MS * 1000);
				}
			}
			break;
		}
		podcastdl_state_t state = podcastdl_state(NULL, NULL);
		if (ours ? (podcastdl_job() != job_id || state != PODCASTDL_RUNNING) : state == PODCASTDL_IDLE) {
			break;
		}
		usleep(AUTO_BUSY_WAIT_MS * 1000);
	}
	return podcastdl_state(NULL, NULL);
}

static void feed_folder(long long feed_id, char *out, size_t size, char *title, size_t title_size) {
	podcast_feed_t feeds[PODCASTSUBS_MAX];
	podcast_feed_t feed;
	memset(&feed, 0, sizeof(feed));
	int n = podcastsubs_list(feeds, PODCASTSUBS_MAX);
	for (int i = 0; i < n; i++) {
		if (feeds[i].id == feed_id) {
			feed = feeds[i];
			break;
		}
	}
	char fallback[32];
	snprintf(fallback, sizeof(fallback), "%lld", feed_id);
	snprintf(title, title_size, "%s", feed.title[0] ? feed.title : fallback);
	char name[160];
	podcastdl_safe_name(title, "Podcast", name, sizeof(name));
	snprintf(out, size, "%s/%s", podcastdl_root(), name);
}

// The options are gathered on the GUI thread before the sweep thread starts:
// config_store has no lock and must not be read while a switch writes it.
typedef struct {
	long long feed_id;
	podcastauto_opts_t opts;
	char author[PODCAST_AUTHOR_MAX];
	char image[PODCAST_URL_MAX];
} auto_job_t;

typedef struct {
	auto_job_t *jobs;
	int count;
} auto_jobs_t;

static void sync_feed(const auto_job_t *job) {
	const podcastauto_opts_t *opts = &job->opts;
	if (!opts->enabled || wifitransfer_get_enabled()) {
		return;
	}

	char folder[700];
	char title[PODCAST_TITLE_MAX];
	feed_folder(job->feed_id, folder, sizeof(folder), title, sizeof(title));

	kept_list_t kept = {0};
	kept_read_folder(folder, &kept);
	podcaststate_in_folder(folder, kept_cb, &kept);
	kept_drop_unowned(&kept);
	if (wifitransfer_get_enabled()) {
		kept_free(&kept);
		return;
	}

	podcast_episode_t episodes[AUTO_EPISODES];
	int n = podcast_episodes(job->feed_id, episodes, AUTO_EPISODES);
	bool have_new = false;
	for (int i = 0; i < n; i++) {
		const podcast_episode_t *e = &episodes[i];
		podcaststate_t state;
		if (!e->enclosure[0] || podcastdl_exists(e, title)) {
			continue;
		}
		if (podcaststate_get_episode(e->id, &state) && state.played) {
			continue;
		}
		have_new = true;
		break;
	}

	int slots = opts->limit > 0 ? opts->limit - kept.count : PODCAST_AUTO_MAX_PER_SYNC;
	if (opts->limit > 0 && slots <= 0) {
		if (!have_new) {
			kept_free(&kept);
			return;
		}
		// At the cap, try one slot for the new episode. The old file is not
		// removed until a download has actually succeeded: the trim after the
		// download is the cap enforcer, so an abandoned attempt loses nothing.
		slots = 1;
	}
	if (slots <= 0) {
		kept_free(&kept);
		return;
	}
	if (slots > PODCAST_AUTO_MAX_PER_SYNC) {
		slots = PODCAST_AUTO_MAX_PER_SYNC;
	}
	if (wifitransfer_get_enabled()) {
		kept_free(&kept);
		return;
	}

	int attempted = 0;
	for (int i = 0; i < n && attempted < slots && attempted < PODCAST_AUTO_MAX_PER_SYNC; i++) {
		// The transfer owns the card and the network while it is on: give up
		// the sweep at once, even if it was switched on after the last check.
		if (wifitransfer_get_enabled()) {
			break;
		}
		const podcast_episode_t *e = &episodes[i];
		if (!e->enclosure[0] || podcastdl_exists(e, title)) {
			continue;
		}
		podcaststate_t state;
		if (podcaststate_get_episode(e->id, &state) && state.played) {
			continue;
		}
		wait_for_download(false, 0);
		if (wifitransfer_get_enabled()) {
			break;
		}
		if (!dl_idle()) {
			break; // somebody else owns the slot; leave the radio to them
		}
		bool started = podcastdl_start(e, title, job->author, job->image);
		attempted++; // every try counts, successful or not
		long long job_id = started ? podcastdl_job() : 0;
		if (!job_id) {
			continue;
		}
		// A transfer switched on here cancels our job only while the slot is
		// still ours, and waits for its terminal state so the acknowledge below
		// actually returns the slot to IDLE before we break.
		podcastdl_state_t result = wait_for_download(true, job_id);
		if (podcastdl_job() != job_id) {
			continue; // a different owner used the slot; do not ack its result
		}
		podcastdl_acknowledge();
		if (wifitransfer_get_enabled()) {
			break; // cancelled, but the slot has been returned to IDLE
		}
		if (result != PODCASTDL_DONE) {
			continue;
		}
		if (opts->limit > 0) {
			kept_free(&kept);
			memset(&kept, 0, sizeof(kept));
			kept_read_folder(folder, &kept);
			podcaststate_in_folder(folder, kept_cb, &kept);
			kept_drop_unowned(&kept);
			if (wifitransfer_get_enabled()) {
				break;
			}
			char fresh[900];
			bool have_fresh = podcastdl_path(e, title, fresh, sizeof(fresh));
			kept_trim(&kept, opts->limit, have_fresh ? fresh : NULL);
			slots = opts->limit - kept.count;
		}
	}
	kept_free(&kept);
}

static void *sync_main(void *arg) {
	thread_be_background("podcast auto");
	auto_jobs_t *jobs = arg;
	for (int i = 0; i < jobs->count; i++) {
		sync_feed(&jobs->jobs[i]);
	}
	free(jobs->jobs);
	free(jobs);
	pthread_mutex_lock(&sync_lock);
	syncing = false;
	pthread_mutex_unlock(&sync_lock);
	return NULL;
}

void podcastauto_sync(void) {
	pthread_mutex_lock(&sync_lock);
	if (syncing) {
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	syncing = true;
	pthread_mutex_unlock(&sync_lock);

	podcast_feed_t feeds[PODCASTSUBS_MAX];
	int n = podcastsubs_list(feeds, PODCASTSUBS_MAX);
	auto_jobs_t *jobs = calloc(1, sizeof(*jobs));
	if (!jobs) {
		pthread_mutex_lock(&sync_lock);
		syncing = false;
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	jobs->jobs = calloc((size_t)(n > 0 ? n : 1), sizeof(*jobs->jobs));
	if (!jobs->jobs) {
		free(jobs);
		pthread_mutex_lock(&sync_lock);
		syncing = false;
		pthread_mutex_unlock(&sync_lock);
		return;
	}
	for (int i = 0; i < n; i++) {
		jobs->jobs[i].feed_id = feeds[i].id;
		podcastauto_get(feeds[i].id, &jobs->jobs[i].opts);
		snprintf(jobs->jobs[i].author, sizeof(jobs->jobs[i].author), "%s", feeds[i].author);
		snprintf(jobs->jobs[i].image, sizeof(jobs->jobs[i].image), "%s", feeds[i].image);
		jobs->count++;
	}

	pthread_t thread;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	if (pthread_create(&thread, &attr, sync_main, jobs) != 0) {
		free(jobs->jobs);
		free(jobs);
		pthread_mutex_lock(&sync_lock);
		syncing = false;
		pthread_mutex_unlock(&sync_lock);
	}
	pthread_attr_destroy(&attr);
}
