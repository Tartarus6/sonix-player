#include "bluetoothd_fallback.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// The firmware ships a bluez built with browsing fixes. It is used by default
// now; the flag exists only to put a unit back on the stock daemon when the
// patched one misbehaves in the field.
#define DEFAULT_DEADLINE_MS 8000

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

static bool disabled(const bt_daemon_fallback_t *f) {
	return f->disable_flag && access(f->disable_flag, F_OK) == 0;
}

static int remaining_ms(uint32_t began, int deadline_ms) {
	uint32_t elapsed = now_ms() - began;
	if ((int)elapsed >= deadline_ms) {
		return 0;
	}
	return deadline_ms - (int)elapsed;
}

static bool start(const bt_daemon_fallback_t *f, const char *path) {
	// Both binaries are launched as "bluetoothd": the rest of the player
	// finds and stops the daemon by that name, and bluez's own re-exec keeps
	// argv[0]. A different basename for the patched build would escape both.
	char *argv[7] = {(char *)"bluetoothd", (char *)"-E", (char *)"-C", NULL, NULL, NULL, NULL};
	int at = 3;
	if (f->debug) {
		argv[at++] = (char *)"-d";
	}
	if (f->conf) {
		argv[at++] = (char *)"-f";
		argv[at++] = (char *)f->conf;
	}
	return f->spawn(path, argv, f->user);
}

bt_up_t bt_daemon_bring_up(const bt_daemon_fallback_t *f) {
	if (!f || !f->spawn || !f->wait || !f->kill) {
		return BT_UP_NONE;
	}

	uint32_t began = now_ms();
	int deadline = f->deadline_ms > 0 ? f->deadline_ms : DEFAULT_DEADLINE_MS;

	if (!disabled(f)) {
		if (start(f, f->patched_path)) {
			int left = remaining_ms(began, deadline);
			if (left > 0 && f->wait(left, f->user)) {
				return BT_UP_PATCHED;
			}
			// The process was started but did not become bluez. Nothing may
			// take its place until it is gone.
			if (!f->kill(f->user)) {
				fprintf(stderr, "bluetooth: patched bluetoothd refused to stop; stock not started\n");
				return BT_UP_NONE;
			}
		}
	}

	if (!start(f, f->stock_path)) {
		return BT_UP_NONE;
	}
	int left = remaining_ms(began, deadline);
	if (left > 0 && f->wait(left, f->user)) {
		return BT_UP_STOCK;
	}
	return BT_UP_NONE;
}
