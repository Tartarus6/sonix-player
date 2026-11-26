// The pure USB wake helpers, on the host. No hardware, no globals: just the
// config defaults, the RTC alarm arbitration and the awake-window calculation.
//
// Built and run by `make test-usb-wake`. Not part of host or target.

#include "src/system/device/usbwake.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
	assert(usbwake_default_seconds(true) == 30);
	assert(usbwake_default_seconds(false) == 0);
	assert(usbwake_interval_valid(0) && usbwake_interval_valid(15));
	assert(usbwake_interval_valid(30) && usbwake_interval_valid(60));
	assert(!usbwake_interval_valid(10));

	long out = -1;
	assert(usbwake_pick(1200, 3600, 30, &out) == USBWAKE_USB_CHECK && out == 30);
	assert(usbwake_pick(20, 3600, 30, &out) == USBWAKE_AUTO_OFF && out == 20);
	assert(usbwake_pick(3600, 1200, 0, &out) == USBWAKE_BATTERY && out == 1200);
	assert(usbwake_pick(1200, 3600, 0, &out) == USBWAKE_AUTO_OFF && out == 1200);
	assert(usbwake_pick(0, 0, 0, &out) == USBWAKE_NONE);
	assert(usbwake_pick(30, 3600, 30, &out) == USBWAKE_AUTO_OFF);

	assert(usbwake_mem_active_after_wake(100000, 60000, true, false) == 46000);
	assert(usbwake_mem_active_after_wake(100000, 60000, true, true) == 100000);
	assert(usbwake_mem_active_after_wake(100000, 5000, true, false) == 100000);
	assert(usbwake_mem_active_after_wake(200000, 60000, false, false) == 200000);
	assert(usbwake_mem_active_after_wake(200000, 60000, true, false) == 146000);

	// A hand-edited interval is rounded to the same value Settings shows, so
	// the UI cannot claim 30 s while the scheduler armed nothing.
	assert(usbwake_normalize_seconds(-5) == 0);
	assert(usbwake_normalize_seconds(0) == 0);
	assert(usbwake_normalize_seconds(1) == 15);
	assert(usbwake_normalize_seconds(20) == 30);
	assert(usbwake_normalize_seconds(30) == 30);
	assert(usbwake_normalize_seconds(61) == 60);

	// The 30 s re-attempt guard is for attempts that gave up. A cable check
	// really slept: restamping on that wake would keep the 6 s window behind
	// the guard and stretch the check period to the guard instead.
	uint32_t last_attempt = 1000;
	uint32_t resume = 100000;
	uint32_t mem_after = 60000;
	uint32_t active = usbwake_mem_active_after_wake(resume, mem_after, true, false);
	// suspend_if_idle() sleeps once the backdated marker is mem_after old,
	// i.e. USBWAKE_AWAKE_MS after this wake.
	assert(active + mem_after - resume == USBWAKE_AWAKE_MS);
	assert(usbwake_attempt_stamp_after_wake(resume, last_attempt, true) == last_attempt);
	assert(resume + USBWAKE_AWAKE_MS - last_attempt >= USBWAKE_REATTEMPT_GUARD_MS);
	// Restamping on the wake would leave only the short window before the
	// guard, so the device would stay up for the full guard instead.
	assert(resume + USBWAKE_AWAKE_MS - usbwake_attempt_stamp_after_wake(resume, last_attempt, false)
		   < USBWAKE_REATTEMPT_GUARD_MS);

	return 0;
}
