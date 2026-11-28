// Host test for the playback-start volume safety clamp.
//
// alsa-controls.c is compiled as-is; only the device/config services it calls
// are replaced here, so the test drives the real limit, override and line-out
// code rather than a copy of it.

#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/swvolume.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/utils.h"
#include "src/system/device/sysinfo.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// The limit is on at 60% for the 3.5 mm output from the first config read on.
static long cfg_limit_on = 1;
static long cfg_limit_phones = 60;
static int config_writes;
static int config_saves;

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	if (!strcmp(key, "volume_limit")) {
		return cfg_limit_on;
	}
	if (!strcmp(key, "volume_limit_phones")) {
		return cfg_limit_phones;
	}
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
	config_writes++;
}

bool config_save(void) {
	config_saves++;
	return true;
}

audio_status_t audio_get_status(void) { return AUDIO_STATUS_STOPPED; }
void bluetooth_notify_volume(int percent) { (void)percent; }
void swvolume_set_index(int percent) { (void)percent; }
void usbaudio_apply_volume(int percent) { (void)percent; }
const sysinfo_model_t *sysinfo_model(void) { return NULL; }

bool file_matches(const char *filename, const char *expected) {
	return strcmp(expected, "1") == 0 && strstr(filename, "headset") != NULL;
}

static int failures;
#define CHECK(condition)                                                                                             \
	do {                                                                                                             \
		if (!(condition)) {                                                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", "volume_safety_test.c", __LINE__, #condition);                     \
			failures++;                                                                                              \
		}                                                                                                            \
	} while (0)

static void reset_state(void) {
	// The limit config is cached on first use, so it stays the same throughout.
	lineout_set(false); // a no-op when line out is already off
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, false);
	volume_profile_init(20, -1); // remembered levels, including the clamped one
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, true);
	set_volume_percent(20); // get the hardware to the same 20%
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, false);
	volume_profile_persist(); // flush setup's dirty flag, so only the tested path writes
	config_writes = 0;
	config_saves = 0;
}

// An unexpected level above the limit is pulled down and remembered safely.
static void test_clamp_without_override(void) {
	reset_state();
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, true);
	set_volume_percent(80);
	CHECK(get_volume_percent() == 80);
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, false);

	CHECK(volume_safety_enforce_active() == 60);
	CHECK(get_volume_percent() == 60);
	CHECK(volume_profile_level(false) == 60);
	CHECK(config_writes > 0); // the clamp is persisted
}

// A raise the user confirmed is not an unexpected jump: starting a track must
// not silently revoke the permission while leaving the gate open.
static void test_confirmed_raise_survives_start(void) {
	reset_state();
	volume_limit_set_override_above(VOLUME_OUTPUT_PHONES, true);
	set_volume_percent(85);
	int writes = config_writes;

	CHECK(volume_safety_enforce_active() == 85);
	CHECK(get_volume_percent() == 85);
	CHECK(volume_limit_override_active(VOLUME_OUTPUT_PHONES));
	CHECK(config_writes == writes); // no clamp, nothing to persist
}

// Line out holds a fixed hardware index (100), not a user level. Starting a
// track must not mistake that index for an unsafe volume, rewrite the
// remembered level, or desync the DAC attenuation from the hardware index.
static void test_lineout_index_untouched(void) {
	reset_state();
	lineout_set(true);
	CHECK(lineout_is_active());
	CHECK(get_volume_percent() == 100);

	CHECK(volume_safety_enforce_active() == 100);
	CHECK(get_volume_percent() == 100);
	CHECK(volume_profile_level(false) == 20);
	CHECK(config_writes == 0);

	lineout_set(false);
	CHECK(get_volume_percent() == 20);
	CHECK(volume_profile_level(false) == 20);
}

// An attempted raise while line out is on cannot move the fixed index, so the
// confirmation gate is skipped. No above-limit permission is involved, and
// none may be left behind for after the mode is switched off.
static void test_lineout_raise_needs_no_override(void) {
	reset_state();
	lineout_set(true);
	CHECK(!volume_limit_override_active(VOLUME_OUTPUT_PHONES));

	set_volume_percent(80); // what a raise above the 60% limit would ask for
	CHECK(get_volume_percent() == 100);
	CHECK(volume_profile_level(false) == 20);
	CHECK(!volume_limit_override_active(VOLUME_OUTPUT_PHONES));

	lineout_set(false);
	CHECK(get_volume_percent() == 20);
	CHECK(!volume_limit_override_active(VOLUME_OUTPUT_PHONES));
}

int main(void) {
	test_clamp_without_override();
	test_confirmed_raise_survives_start();
	test_lineout_index_untouched();
	test_lineout_raise_needs_no_override();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("volume safety: all checks passed\n");
	return 0;
}
