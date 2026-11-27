// Tests for the nightly auto sleep timer. The firmware dependencies that
// reach the filesystem and the wall clock are replaced here; the timer logic
// under test is the same source the player builds.
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/playback/sleeptimer.h"

static bool stub_clock_set;
static long stub_manual_enabled;
static long stub_auto_enabled;
static long stub_auto_start;
static long stub_auto_end;
static long stub_minutes;
static char writes[16][96];
static int write_count;

#define CHECK(condition)                                                                                     \
	do {                                                                                                     \
		if (!(condition)) {                                                                                  \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                             \
			exit(1);                                                                                         \
		}                                                                                                    \
	} while (0)

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	if (!strcmp(key, "sleep_timer")) return stub_manual_enabled;
	if (!strcmp(key, "sleep_minutes")) return stub_minutes;
	if (!strcmp(key, "auto_sleep_timer")) return stub_auto_enabled;
	if (!strcmp(key, "auto_sleep_start")) return stub_auto_start;
	if (!strcmp(key, "auto_sleep_end")) return stub_auto_end;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	if (write_count < (int)(sizeof(writes) / sizeof(writes[0]))) {
		snprintf(writes[write_count], sizeof(writes[0]), "%s/%s=%ld", section, key, value);
	}
	write_count++;
}

bool config_save(void) { return true; }

bool clock_is_set(void) { return stub_clock_set; }

// Virtual monotonic clock. The activity thread stops inside clock_gettime so
// the expiry tick has to run against a countdown that a restart is still
// writing. Without a lock the restart loses its full length.
static long long virtual_ms = 1000;
static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clock_cv = PTHREAD_COND_INITIALIZER;
static bool hold_activity_clock;
static bool activity_clock_held;
static bool release_activity_clock;
static pthread_t activity_thread_id;

int __wrap_clock_gettime(clockid_t id, struct timespec *ts) {
	(void)id;
	pthread_mutex_lock(&clock_lock);
	if (hold_activity_clock && pthread_equal(pthread_self(), activity_thread_id)) {
		activity_clock_held = true;
		pthread_cond_broadcast(&clock_cv);
		while (!release_activity_clock) {
			pthread_cond_wait(&clock_cv, &clock_lock);
		}
	}
	long long ms = virtual_ms;
	pthread_mutex_unlock(&clock_lock);
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)((ms % 1000) * 1000000);
	return 0;
}

static void *run_activity(void *arg) {
	(void)arg;
	activity_thread_id = pthread_self();
	sleeptimer_note_activity();
	return NULL;
}

static void *release_clock(void *arg) {
	(void)arg;
	usleep(100000);
	pthread_mutex_lock(&clock_lock);
	release_activity_clock = true;
	pthread_cond_broadcast(&clock_cv);
	pthread_mutex_unlock(&clock_lock);
	return NULL;
}

static void test_span(void) {
	// 22:00 to 06:00 crosses midnight.
	CHECK(sleeptimer_span_contains(1380, 1320, 360)); // 23:00
	CHECK(sleeptimer_span_contains(359, 1320, 360));  // 05:59
	CHECK(!sleeptimer_span_contains(1319, 1320, 360)); // 21:59
	CHECK(!sleeptimer_span_contains(360, 1320, 360));  // 06:00

	// Equal endpoints mean every hour.
	CHECK(sleeptimer_span_contains(0, 720, 720));
	CHECK(sleeptimer_span_contains(1439, 720, 720));
}

static void test_no_arm_without_clock(void) {
	stub_clock_set = false;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 30;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleeptimer_check_auto();
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
}

static void test_auto_arms_and_restarts_on_activity(void) {
	stub_clock_set = true;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 30;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleeptimer_check_auto();
	CHECK(sleeptimer_running(SLEEPTIMER_MUSIC));

	sleeptimer_note_activity();
	CHECK(sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);
}

static void test_activity_without_playback(void) {
	stub_clock_set = true;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 30;
	sleeptimer_init();
	sleeptimer_note_activity();
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(!sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
}

static void test_activity_restart_survives_a_concurrent_expiry(void) {
	stub_clock_set = true;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 30;
	virtual_ms = 1000;
	activity_clock_held = false;
	release_activity_clock = false;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleeptimer_check_auto();
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);

	hold_activity_clock = true;
	pthread_t activity;
	pthread_create(&activity, NULL, run_activity, NULL);
	pthread_mutex_lock(&clock_lock);
	while (!activity_clock_held) {
		pthread_cond_wait(&clock_cv, &clock_lock);
	}
	virtual_ms = 3000; // two seconds pass while the restart is half-written
	pthread_mutex_unlock(&clock_lock);

	pthread_t releaser;
	pthread_create(&releaser, NULL, release_clock, NULL);
	sleeptimer_expired(SLEEPTIMER_MUSIC);
	pthread_join(releaser, NULL);
	pthread_join(activity, NULL);
	hold_activity_clock = false;

	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);
}

static void test_auto_set_leaves_manual_keys_alone(void) {
	stub_auto_enabled = 0;
	stub_auto_start = 0;
	stub_auto_end = 0;
	sleeptimer_init();
	write_count = 0;
	sleeptimer_auto_t value = {.enabled = true, .start = 1320, .end = 360};
	sleeptimer_auto_set(&value);
	CHECK(write_count == 3);
	for (int i = 0; i < write_count; i++) {
		CHECK(strncmp(writes[i], "player/auto_sleep_", 18) == 0);
	}
}

static void test_auto_arms_with_default_config(void) {
	// A fresh device has neither a manual switch nor a sleep_minutes value. The
	// nightly switch must still arm a stretch on its own.
	stub_clock_set = true;
	stub_manual_enabled = 0;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 0;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	sleeptimer_check_auto();
	CHECK(sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);

	sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(sleeptimer_running(SLEEPTIMER_AUDIOBOOK));
	CHECK(sleeptimer_remaining(SLEEPTIMER_AUDIOBOOK) == 30 * 60);
}

static void test_window_boundary_keeps_manual_timer(void) {
	stub_clock_set = true;
	stub_manual_enabled = 1;
	stub_auto_enabled = 0;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 30;
	virtual_ms = 1000;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);

	virtual_ms = 1000 + 2 * 60000;
	sleeptimer_expired(SLEEPTIMER_MUSIC);
	int mid = sleeptimer_remaining(SLEEPTIMER_MUSIC);
	CHECK(mid <= 28 * 60);

	// Entering and leaving the window must not hand a manual timer that is
	// already counting a fresh full length.
	sleeptimer_auto_t value = {.enabled = true, .start = 720, .end = 720};
	sleeptimer_auto_set(&value);
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == mid);
	value.enabled = false;
	sleeptimer_auto_set(&value);
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == mid);
	stub_manual_enabled = 0;
}

static void test_enabling_auto_mid_playback_arms_and_keeps_tick(void) {
	stub_clock_set = true;
	stub_manual_enabled = 0;
	stub_auto_enabled = 0;
	stub_minutes = 0;
	virtual_ms = 1000;
	sleeptimer_init();

	// Music started while the switch was off: nothing is armed and the tick
	// has no reason to run yet.
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(!sleeptimer_tick_needed(true));

	// The Settings callback flips the switch while the track is still playing.
	// It must arm the stretch and leave the tick alive without waiting for a
	// playback-status change or for a tick that was never resumed.
	sleeptimer_auto_t value = {.enabled = true, .start = 720, .end = 720};
	sleeptimer_auto_set(&value);
	CHECK(sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(sleeptimer_remaining(SLEEPTIMER_MUSIC) == 30 * 60);
	CHECK(sleeptimer_tick_needed(true));

	value.enabled = false;
	sleeptimer_auto_set(&value);
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(!sleeptimer_tick_needed(true));
}

static void test_daytime_enable_keeps_tick_for_the_window(void) {
	stub_clock_set = true;
	stub_manual_enabled = 0;
	stub_auto_enabled = 0;
	stub_minutes = 0;
	virtual_ms = 1000;
	sleeptimer_init();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);

	// A window that starts two minutes from now: nothing is armed yet, but a
	// paused tick would never notice the window opening and a button press
	// could not arm the stretch either.
	time_t now = time(NULL);
	struct tm local;
	CHECK(localtime_r(&now, &local) != NULL);
	int minute_now = local.tm_hour * 60 + local.tm_min;
	sleeptimer_auto_t value = {.enabled = true,
							   .start = (minute_now + 2) % 1440,
							   .end = (minute_now + 3) % 1440};
	sleeptimer_auto_set(&value);
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(sleeptimer_tick_needed(true));

	// Playback stopping puts the tick back to sleep despite the option being on.
	sleeptimer_note_playing(SLEEPTIMER_COUNT);
	CHECK(!sleeptimer_tick_needed(false));
}

static void test_paused_manual_timer_does_not_keep_tick(void) {
	stub_clock_set = true;
	stub_manual_enabled = 1;
	stub_auto_enabled = 0;
	stub_minutes = 30;
	sleeptimer_init();
	CHECK(!sleeptimer_any_running());
	CHECK(!sleeptimer_tick_needed(false));
	sleeptimer_note_activity();
	CHECK(!sleeptimer_tick_needed(false));
	stub_manual_enabled = 0;
}

static void test_tick_not_needed_for_ineligible_playback_or_unset_clock(void) {
	stub_clock_set = true;
	stub_manual_enabled = 0;
	stub_auto_enabled = 1;
	stub_auto_start = 0;
	stub_auto_end = 0;
	stub_minutes = 0;
	virtual_ms = 1000;
	sleeptimer_init();
	sleeptimer_check_auto();

	// The two kinds the option can arm keep the tick alive even before the
	// stretch is armed, so a window opening mid-track is noticed.
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	CHECK(sleeptimer_tick_needed(true));
	sleeptimer_note_playing(SLEEPTIMER_AUDIOBOOK);
	CHECK(sleeptimer_tick_needed(true));

	// A podcast can never be auto-armed: keeping the tick alive for it would
	// wake the player every second all night with nothing to check.
	sleeptimer_note_playing(SLEEPTIMER_PODCAST);
	CHECK(!sleeptimer_any_running());
	CHECK(!sleeptimer_tick_needed(true));

	// Nor can anything arm without a clock, even for an otherwise eligible
	// kind. The radio and the other external sources report no kind at all.
	stub_clock_set = false;
	sleeptimer_check_auto();
	sleeptimer_note_playing(SLEEPTIMER_MUSIC);
	CHECK(!sleeptimer_running(SLEEPTIMER_MUSIC));
	CHECK(!sleeptimer_tick_needed(true));
	sleeptimer_note_playing(SLEEPTIMER_COUNT);
	CHECK(!sleeptimer_tick_needed(true));
}

int main(void) {
	test_span();
	test_no_arm_without_clock();
	test_auto_arms_and_restarts_on_activity();
	test_activity_without_playback();
	test_activity_restart_survives_a_concurrent_expiry();
	test_auto_set_leaves_manual_keys_alone();
	test_auto_arms_with_default_config();
	test_window_boundary_keeps_manual_timer();
	test_enabling_auto_mid_playback_arms_and_keeps_tick();
	test_daytime_enable_keeps_tick_for_the_window();
	test_paused_manual_timer_does_not_keep_tick();
	test_tick_not_needed_for_ineligible_playback_or_unset_clock();
	puts("auto sleep timer tests passed");
	return 0;
}
