#ifndef USBWAKE_H
#define USBWAKE_H

#include <stdbool.h>
#include <stdint.h>

// Pure helpers shared by the RTC alarm scheduler and the resume path. No
// hardware, no globals: the caller owns the state and the RTC writes.

// Why the RTC alarm woke the device. On ties the order is auto-off, battery,
// then USB check: a pending shutdown must not be read as a periodic check.
typedef enum {
	USBWAKE_NONE,
	USBWAKE_AUTO_OFF,
	USBWAKE_BATTERY,
	USBWAKE_USB_CHECK,
} usbwake_reason_t;

// How long to stay awake after a cable check that found nothing, so the
// watcher can poll VBUS and the battery gauge can take a reading.
#define USBWAKE_AWAKE_MS 6000u

// The guard after a suspend attempt that gave up. It must not be restamped on
// a cable-check wake, or the short awake window above is never reached.
#define USBWAKE_REATTEMPT_GUARD_MS 30000u

// The cable-check interval on a player whose PMIC is its charger (the R1).
// Elsewhere the MP2731 already wakes on cable insert, so the answer is off.
uint32_t usbwake_default_seconds(bool pmic_charger);

// Only 0 (off), 15, 30 and 60 seconds are offered by the settings slider.
bool usbwake_interval_valid(uint32_t seconds);

// Round a saved interval to the next offered one (0 for a negative value, 60
// for anything past the table). Settings and the startup path both use this,
// so the UI cannot show an interval that the scheduler did not arm.
uint32_t usbwake_normalize_seconds(long seconds);

// Pick the earliest of the three deadlines and report which check owns it.
// A zero input disables that check; out_s, when non-NULL, gets the interval.
usbwake_reason_t usbwake_pick(long auto_off_s, long battery_s, long usb_s,
							 long *out_s);

// The idle marker to use after a wake. A cable check with no cable shortens
// the marker so the device goes back to sleep after USBWAKE_AWAKE_MS instead
// of waiting out the full standby delay. Every other wake leaves it alone.
uint32_t usbwake_mem_active_after_wake(uint32_t now_ms, uint32_t mem_after_ms,
									   bool usb_check_wake, bool cable_found);

// The re-attempt marker to use after a wake. A real cable check slept and
// woke on its alarm, so the old marker stands; every other wake stamps now.
uint32_t usbwake_attempt_stamp_after_wake(uint32_t now_ms, uint32_t last_attempt_ms,
										 bool usb_check_wake);

#endif /* USBWAKE_H */
