#include "sleeptimer.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/device/clock.h"

// A whole day, which is the most the two wheels can be set to and well past
// anything anybody means by a sleep timer. The clamp is here so a hand-edited
// config cannot arm something that never expires.
#define SLEEP_MAX_MINUTES (23 * 60 + 59)

// The window defaults to the ordinary night: ten at night to six in the morning.
#define AUTO_START_DEFAULT (22 * 60)
#define AUTO_END_DEFAULT (6 * 60)
#define AUTO_MAX_MINUTE (23 * 60 + 59)

// The nightly stretch has its own length. The Settings page offers only the
// switch and the window, and a fresh device has no manual sleep_minutes at all,
// so borrowing the manual duration would leave the new switch arming nothing.
#define AUTO_MINUTES 30

// Where each one is written down. The audiobook pair are the keys the audiobook
// timer has always used, so an existing config keeps its switch and its length.
// A length of nought there means no time chosen: "at the end of the chapter" is
// a switch of its own on the audiobook page, not a timer length.
static const struct {
	const char *section;
	const char *key_on;
	const char *key_minutes;
	const char *name;
} PROFILES[SLEEPTIMER_COUNT] = {
	[SLEEPTIMER_MUSIC] = {"player", "sleep_timer", "sleep_minutes", "music"},
	[SLEEPTIMER_AUDIOBOOK] = {"audiobook", "sleep_timer", "sleep_minutes", "audiobook"},
	[SLEEPTIMER_PODCAST] = {"podcast", "sleep_timer", "sleep_minutes", "podcast"},
};

typedef struct {
	bool enabled;
	int minutes;
	bool counting;
	long long remaining_ms;
	long long last_ms;
} timer_state_t;

static timer_state_t timers[SLEEPTIMER_COUNT];

// Buttons arrive on input threads while the countdown is read on the UI
// thread. One lock keeps a restart from losing an expiry and the other way
// round. It also covers `playing_kind` and `auto_active`: both are read when a
// restart decides what to arm, so an input thread must not see a half-written
// transition from the UI tick.
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static sleeptimer_auto_t auto_settings;
static bool auto_active;

// Which kind is playing, kept apart from the countdowns: "something is playing"
// and "a stretch is running" are not the same thing, and folding them into one
// flag is how switching the option off and on again while the music played
// armed nothing at all -- the first call cleared the flag the second one needed
// to read.
static sleeptimer_kind_t playing_kind = SLEEPTIMER_COUNT;

static bool in_range(sleeptimer_kind_t kind) { return kind >= 0 && kind < SLEEPTIMER_COUNT; }

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int clamp_minutes(int minutes) {
	if (minutes < 0) {
		return 0;
	}
	return minutes > SLEEP_MAX_MINUTES ? SLEEP_MAX_MINUTES : minutes;
}

static int clamp_time(int minute) {
	if (minute < 0) {
		return 0;
	}
	return minute > AUTO_MAX_MINUTE ? AUTO_MAX_MINUTE : minute;
}

bool sleeptimer_span_contains(int now, int start, int end) {
	if (start == end) {
		return true;
	}
	if (start < end) {
		return now >= start && now < end;
	}
	return now >= start || now < end;
}

void sleeptimer_init(void) {
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		timers[i].enabled = config_get_int(PROFILES[i].section, PROFILES[i].key_on, 0) != 0;
		timers[i].minutes = clamp_minutes((int)config_get_int(PROFILES[i].section, PROFILES[i].key_minutes, 0));
		timers[i].counting = false;
		timers[i].remaining_ms = 0;
	}
	playing_kind = SLEEPTIMER_COUNT;
	auto_settings.enabled = config_get_int("player", "auto_sleep_timer", 0) != 0;
	auto_settings.start = clamp_time((int)config_get_int("player", "auto_sleep_start", AUTO_START_DEFAULT));
	auto_settings.end = clamp_time((int)config_get_int("player", "auto_sleep_end", AUTO_END_DEFAULT));
	auto_active = false;
	sleeptimer_check_auto();
}

bool sleeptimer_enabled(sleeptimer_kind_t kind) {
	if (!in_range(kind)) {
		return false;
	}
	pthread_mutex_lock(&state_lock);
	bool enabled = timers[kind].enabled;
	pthread_mutex_unlock(&state_lock);
	return enabled;
}

int sleeptimer_minutes(sleeptimer_kind_t kind) {
	if (!in_range(kind)) {
		return 0;
	}
	pthread_mutex_lock(&state_lock);
	int minutes = timers[kind].minutes;
	pthread_mutex_unlock(&state_lock);
	return minutes;
}

// A manually configured stretch takes precedence; the nightly one supplies its
// own length so it works with a default config. All of these helpers read state
// that lives under `state_lock` and must be called with it held.
static bool manual_armed_locked(sleeptimer_kind_t kind) {
	return timers[kind].enabled && timers[kind].minutes > 0;
}

static int armed_minutes_locked(sleeptimer_kind_t kind) {
	if (manual_armed_locked(kind)) {
		return timers[kind].minutes;
	}
	if (auto_active && (kind == SLEEPTIMER_MUSIC || kind == SLEEPTIMER_AUDIOBOOK)) {
		return AUTO_MINUTES;
	}
	return 0;
}

// Starts a full stretch, or stops counting, from the state as it stands. Every
// change goes through here, so there is one place that decides and one place
// where `remaining_ms` is ever set.
static void restart_locked(sleeptimer_kind_t kind) {
	timer_state_t *t = &timers[kind];
	int minutes = armed_minutes_locked(kind);
	t->counting = playing_kind == kind && minutes > 0;
	t->remaining_ms = t->counting ? (long long)minutes * 60000 : 0;
	t->last_ms = now_ms();
}

static void restart(sleeptimer_kind_t kind) {
	pthread_mutex_lock(&state_lock);
	restart_locked(kind);
	pthread_mutex_unlock(&state_lock);
}

void sleeptimer_auto_get(sleeptimer_auto_t *out) {
	if (out) {
		*out = auto_settings;
	}
}

void sleeptimer_check_auto(void) {
	bool active = false;
	if (auto_settings.enabled && clock_is_set()) {
		time_t now = time(NULL);
		struct tm local;
		if (localtime_r(&now, &local)) {
			active = sleeptimer_span_contains(local.tm_hour * 60 + local.tm_min, auto_settings.start,
											  auto_settings.end);
		}
	}
	pthread_mutex_lock(&state_lock);
	if (active == auto_active) {
		pthread_mutex_unlock(&state_lock);
		return;
	}
	auto_active = active;
	// A window boundary changes only the auto-armed stretches. A timer the
	// listener switched on manually keeps counting from where it was instead of
	// being handed a fresh full length on entering and leaving the window.
	if (!manual_armed_locked(SLEEPTIMER_MUSIC)) {
		restart_locked(SLEEPTIMER_MUSIC);
	}
	if (!manual_armed_locked(SLEEPTIMER_AUDIOBOOK)) {
		restart_locked(SLEEPTIMER_AUDIOBOOK);
	}
	pthread_mutex_unlock(&state_lock);
}

void sleeptimer_auto_set(const sleeptimer_auto_t *value) {
	if (!value) {
		return;
	}
	bool changed = false;
	if (value->enabled != auto_settings.enabled) {
		auto_settings.enabled = value->enabled;
		config_set_int("player", "auto_sleep_timer", value->enabled ? 1 : 0);
		changed = true;
	}
	int start = clamp_time(value->start);
	if (start != auto_settings.start) {
		auto_settings.start = start;
		config_set_int("player", "auto_sleep_start", start);
		changed = true;
	}
	int end = clamp_time(value->end);
	if (end != auto_settings.end) {
		auto_settings.end = end;
		config_set_int("player", "auto_sleep_end", end);
		changed = true;
	}
	if (changed) {
		config_save();
	}
	sleeptimer_check_auto();
}

void sleeptimer_note_activity(void) {
	pthread_mutex_lock(&state_lock);
	if (playing_kind != SLEEPTIMER_COUNT) {
		restart_locked(playing_kind);
	}
	pthread_mutex_unlock(&state_lock);
}

void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) {
	if (!in_range(kind)) {
		return;
	}
	pthread_mutex_lock(&state_lock);
	if (on == timers[kind].enabled) {
		pthread_mutex_unlock(&state_lock);
		return;
	}
	timers[kind].enabled = on;
	pthread_mutex_unlock(&state_lock);
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_on, on ? 1 : 0);
	config_save();
	// Switching it off in the middle of a stretch disarms it now, and switching
	// it on while something is already playing starts one: otherwise the setting
	// would only take effect at the next pause.
	restart(kind);
}

void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes) {
	if (!in_range(kind)) {
		return;
	}
	minutes = clamp_minutes(minutes);
	pthread_mutex_lock(&state_lock);
	if (minutes == timers[kind].minutes) {
		pthread_mutex_unlock(&state_lock);
		return;
	}
	timers[kind].minutes = minutes;
	pthread_mutex_unlock(&state_lock);
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_minutes, minutes);
	config_save();
	// Moving the wheel is choosing a new stretch, not editing the one running:
	// whoever just set thirty minutes means thirty minutes from now.
	restart(kind);
}

void sleeptimer_note_playing(sleeptimer_kind_t kind) {
	if (!in_range(kind)) {
		kind = SLEEPTIMER_COUNT; // nothing any of them counts
	}
	bool report[SLEEPTIMER_COUNT];
	int minutes[SLEEPTIMER_COUNT];
	pthread_mutex_lock(&state_lock);
	if (kind == playing_kind) {
		pthread_mutex_unlock(&state_lock);
		return;
	}
	playing_kind = kind;
	// All of them, not only the one named: whatever stopped playing has a
	// countdown to put away, and going from an album to a podcast is both at
	// once.
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		restart_locked((sleeptimer_kind_t)i);
		report[i] = timers[i].counting;
		minutes[i] = armed_minutes_locked((sleeptimer_kind_t)i);
	}
	pthread_mutex_unlock(&state_lock);
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		if (report[i]) {
			fprintf(stderr, "sleeptimer: %s, %d min\n", PROFILES[i].name, minutes[i]);
		}
	}
}

static bool running_unlocked(sleeptimer_kind_t kind) { return in_range(kind) && timers[kind].counting; }

bool sleeptimer_running(sleeptimer_kind_t kind) {
	pthread_mutex_lock(&state_lock);
	bool running = running_unlocked(kind);
	pthread_mutex_unlock(&state_lock);
	return running;
}

bool sleeptimer_any_running(void) {
	pthread_mutex_lock(&state_lock);
	bool any = false;
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		if (timers[i].counting) {
			any = true;
			break;
		}
	}
	pthread_mutex_unlock(&state_lock);
	return any;
}

bool sleeptimer_tick_needed(bool playing) {
	pthread_mutex_lock(&state_lock);
	bool any = false;
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		if (timers[i].counting) {
			any = true;
			break;
		}
	}
	bool auto_enabled = auto_settings.enabled;
	// Only music and audiobooks can be armed by the automatic option, and only
	// with a set clock. Podcasts, radio and the other external sources are none
	// of them, so waking every second for those would be all night with nothing
	// to check. `playing_kind` is the one last told by the player under this
	// lock, so it says which of them is actually on.
	bool auto_eligible = playing && (playing_kind == SLEEPTIMER_MUSIC || playing_kind == SLEEPTIMER_AUDIOBOOK);
	pthread_mutex_unlock(&state_lock);
	// An automatic option that is on while an eligible kind plays needs the tick
	// even before a stretch is armed: the window may open mid-track. A manual
	// timer paused with playback, or a source the automatic option can never
	// arm, is not a reason to keep the tick alive.
	return any || (auto_enabled && auto_eligible && clock_is_set());
}

bool sleeptimer_expired(sleeptimer_kind_t kind) {
	pthread_mutex_lock(&state_lock);
	if (!running_unlocked(kind)) {
		pthread_mutex_unlock(&state_lock);
		return false;
	}
	timer_state_t *t = &timers[kind];

	long long now = now_ms();
	long long elapsed = now - t->last_ms;
	t->last_ms = now;
	if (elapsed < 0) {
		elapsed = 0; // the clock is monotonic, but do not trust a negative step
	}

	if (elapsed >= t->remaining_ms) {
		t->counting = false;
		t->remaining_ms = 0;
		pthread_mutex_unlock(&state_lock);
		fprintf(stderr, "sleeptimer: %s, time is up; stopping\n", PROFILES[kind].name);
		return true;
	}
	t->remaining_ms -= elapsed;
	pthread_mutex_unlock(&state_lock);
	return false;
}

int sleeptimer_remaining(sleeptimer_kind_t kind) {
	pthread_mutex_lock(&state_lock);
	int remaining = 0;
	if (running_unlocked(kind)) {
		remaining = (int)((timers[kind].remaining_ms + 999) / 1000);
	}
	pthread_mutex_unlock(&state_lock);
	return remaining;
}
