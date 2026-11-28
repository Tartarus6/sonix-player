#include "soundcloudsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

// How many likes fit in the local mirror. Past this the oldest show an empty
// star until the list is refreshed: the price of not asking SoundCloud on
// every track change, which would be one extra request per song.
#define FAVORITES_MAX 500
#define FAVORITES_PAGE 16

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static char favorites[FAVORITES_MAX][SOUNDCLOUD_ID_MAX];
static int favorites_count;
static bool favorites_loaded;

// ---------------------------------------------------------------------------
// the mirror
// ---------------------------------------------------------------------------

bool soundcloudsync_favorites_known(void) {
	pthread_mutex_lock(&lock);
	bool known = favorites_loaded;
	pthread_mutex_unlock(&lock);
	return known;
}

static bool same_id(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

bool soundcloudsync_is_favorite(const char *id) {
	if (!id || !*id) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool found = false;
	for (int i = 0; i < favorites_count && !found; i++) {
		found = same_id(favorites[i], id);
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// Call with the lock held.
static void mirror_set_locked(const char *id, bool on) {
	for (int i = 0; i < favorites_count; i++) {
		if (!same_id(favorites[i], id)) {
			continue;
		}
		if (!on) {
			favorites_count--;
			if (i != favorites_count) {
				memcpy(favorites[i], favorites[favorites_count], SOUNDCLOUD_ID_MAX);
			}
		}
		return;
	}
	if (on && favorites_count < FAVORITES_MAX) {
		snprintf(favorites[favorites_count++], SOUNDCLOUD_ID_MAX, "%s", id);
	}
}

static void mirror_set(const char *id, bool on) {
	pthread_mutex_lock(&lock);
	mirror_set_locked(id, on);
	pthread_mutex_unlock(&lock);
}

void soundcloudsync_forget(void) {
	pthread_mutex_lock(&lock);
	favorites_count = 0;
	favorites_loaded = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// the worker
//
// One thread, one queue slot. Submitting while a job is already waiting
// replaces it: these are user taps, and the last one wins. A job already
// running is always finished -- abandoning a request that writes to the
// account halfway through would leave its state unknown.
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_FAVORITES_LOAD,
	JOB_FAVORITE_TOGGLE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char id[SOUNDCLOUD_ID_MAX];
	bool want;
	soundcloudsync_done_cb done;
	void *user;
	void (*changed)(void);
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

static void do_favorites_load(const job_t *job) {
	char cursor[SOUNDCLOUD_URL_MAX] = "";
	soundcloud_track_t batch[FAVORITES_PAGE];
	char ids[FAVORITES_MAX][SOUNDCLOUD_ID_MAX];
	int count = 0;

	for (;;) {
		char previous[SOUNDCLOUD_URL_MAX];
		snprintf(previous, sizeof(previous), "%s", cursor);

		int n = soundcloud_liked_tracks(cursor, sizeof(cursor), batch, FAVORITES_PAGE);
		if (n < 0) {
			// Keep the old mirror: a failed read is not an empty account.
			printf("soundcloudsync: favorites not read: %s\n", soundcloud_last_error());
			return;
		}
		if (n == 0) {
			break;
		}
		for (int i = 0; i < n && count < FAVORITES_MAX; i++) {
			snprintf(ids[count++], SOUNDCLOUD_ID_MAX, "%s", batch[i].id);
		}
		// The backend sets cursor to the next page's href. An empty or
		// unchanged cursor with more items would spin forever.
		if (!cursor[0] || strcmp(cursor, previous) == 0) {
			break;
		}
	}

	pthread_mutex_lock(&lock);
	for (int i = 0; i < count; i++) {
		memcpy(favorites[i], ids[i], SOUNDCLOUD_ID_MAX);
	}
	favorites_count = count;
	favorites_loaded = true;
	pthread_mutex_unlock(&lock);

	printf("soundcloudsync: %d liked tracks\n", count);
	if (job->changed) {
		job->changed();
	}
}

static void do_favorite_toggle(const job_t *job) {
	bool ok = soundcloud_like(job->id, job->want);
	if (!ok) {
		// The mirror was flipped before the request so the star could answer
		// the tap immediately; on failure it must agree with the account again.
		mirror_set(job->id, !job->want);
	}
	if (job->done) {
		job->done(ok, ok ? NULL : soundcloud_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("soundcloud sync");

	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		switch (job.kind) {
		case JOB_FAVORITES_LOAD:
			do_favorites_load(&job);
			break;
		case JOB_FAVORITE_TOGGLE:
			do_favorite_toggle(&job);
			break;
		default:
			break;
		}
	}
	return NULL;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	if (!worker_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		} else {
			pthread_mutex_unlock(&job_lock);
			if (job->done) {
				job->done(false, tr("cannot_start_the_request"), job->user);
			}
			return;
		}
	}
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

// ---------------------------------------------------------------------------
// public face
// ---------------------------------------------------------------------------

void soundcloudsync_favorites_refresh(void (*done)(void)) {
	if (!soundcloud_logged_in() || soundcloudsync_favorites_known()) {
		return;
	}
	job_t job = {.kind = JOB_FAVORITES_LOAD, .changed = done};
	submit(&job);
}

void soundcloudsync_favorite_toggle(const char *id, bool want, soundcloudsync_done_cb done, void *user) {
	if (!id || !*id) {
		if (done) {
			done(false, tr("cannot_start_the_request"), user);
		}
		return;
	}

	// Mirror first, network after: the star fills under the finger instead of
	// a second and a half later.
	mirror_set(id, want);

	job_t job = {.kind = JOB_FAVORITE_TOGGLE, .want = want, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", id);
	submit(&job);
}
