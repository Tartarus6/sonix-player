// The AXP2101 charge state machine, on the host, against the register map
// axpcharge.c keeps under HOST_BUILD (it is the same file the player builds,
// with AXPCHARGE_TEST adding the hooks).
//
// Built and run by `make test-axpcharge`. Not part of host or target.

#include "src/system/device/axpcharge.h"
#include "src/system/device/axpcharge_test.h"

#include "src/system/device/sysinfo.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The rest of axpcharge.c's dependencies. A request reads one config key, and
// asks the model table, the cable and the gauge whether a stop is safe.
static int battery_mv = 3900;
static bool vbus_present = true;

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

// The model the stubbed sysinfo table reports. The host gate consults it when
// a model is named, so the real `model && model->pmic_charger` expression is
// exercised by these tests and not only on the device.
static sysinfo_model_t test_model;
static bool test_model_known;

static void set_test_model(bool known, bool pmic_charger) {
	memset(&test_model, 0, sizeof test_model);
	test_model.pmic_charger = pmic_charger;
	test_model_known = known;
}

const sysinfo_model_t *sysinfo_model(void) { return test_model_known ? &test_model : NULL; }

bool usb_vbus_present(void) { return vbus_present; }

int read_battery_millivolts(void) { return battery_mv; }

// The two hooks that belong with the stubs above: axpcharge_test_reset() calls
// them to put the defaults back.
void axpcharge_test_set_millivolts(int mv) { battery_mv = mv; }

void axpcharge_test_set_vbus(bool present) { vbus_present = present; }

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

#define REG18 0x18

// 1. The strict gate answers true on the model that has the PMIC as charger.
static void case_supported_r1(void) {
	axpcharge_test_reset();
	set_test_model(true, true);
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_charging_control_supported(), "supported_r1");
	set_test_model(false, false);
}

// 2. A stop clears bit 1 and nothing else.
static void case_stop_clears_only_bit1(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_set_charging(false), "stop_clears_only_bit1 returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0c, "stop_clears_only_bit1 clears only bit 1");
	CHECK(axpcharge_charging_blocked(), "stop_clears_only_bit1 reports blocked");
}

// 3. A resume puts the bit back, and clears the blocked report.
static void case_resume_sets_bit1(void) {
	CHECK(axpcharge_set_charging(true), "resume_sets_bit1 returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "resume_sets_bit1 sets bit 1");
	CHECK(!axpcharge_charging_blocked(), "resume_sets_bit1 reports unblocked");
}

// 4. A refused stop leaves the register alone and is not retried behind the UI.
//    charge_known is made true first, so this is the kind of stop the tick
//    would retry; the failure is cleared before the tick, so a retry would
//    really clear bit 1.
static void case_refused_off_reverts(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_set_charging(false), "refused_off_reverts stops first");
	CHECK(axpcharge_set_charging(true), "refused_off_reverts resumes first");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_set_charging(false), "refused_off_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "refused_off_reverts leaves the bit");
	CHECK(!axpcharge_charging_blocked(), "refused_off_reverts not blocked");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "refused_off_reverts not retried by the tick");
}

// 5. A refused resume leaves the stop in place and is not retried either.
static void case_refused_on_reverts(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "refused_on_reverts stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_set_charging(true), "refused_on_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts leaves the bit clear");
	CHECK(axpcharge_charging_blocked(), "refused_on_reverts still blocked");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts not retried by the tick");
}

// 6. Unlike a user request, a failed release stays queued and the tick retries
//    it until the PMIC takes it, on the usual five-second period.
static void case_release_retries(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_retries stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_retries starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_charging_release(), "release_retries immediate attempt fails");
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_retries still stopped after a failed tick");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_retries tick restores the bit");
	CHECK(!axpcharge_charging_blocked(), "release_retries tick clears blocked");
}

// 6b. A failed release waits the five seconds; only a requested stop earns the
//     one-second period.
static void case_release_waits_5s(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_waits_5s stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_waits_5s starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_charging_release(), "release_waits_5s immediate attempt fails");
	// Clear the failure before the first tick: with a one-second retry the tick
	// would now succeed and set the bit, so this check can fail if the cadence
	// regresses.
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(2000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_waits_5s not retried after two seconds");
	axpcharge_test_advance_ms(4000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_waits_5s retried after five seconds");
	CHECK(!axpcharge_charging_blocked(), "release_waits_5s clears blocked");
}

// 6c. A release that succeeds at once re-enables charging and reports it.
static void case_release_enables(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_enables stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_enables starts stopped");
	CHECK(axpcharge_charging_release(), "release_enables returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_enables restores the bit");
	CHECK(!axpcharge_charging_blocked(), "release_enables reports unblocked");
}

// 7. A nearly flat cell with VBUS in is the one case a stop is refused.
static void case_low_cell_refuses(void) {
	axpcharge_test_reset();
	axpcharge_test_set_millivolts(3200);
	axpcharge_test_set_vbus(true);
	CHECK(!axpcharge_set_charging(false), "low_cell_refuses returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "low_cell_refuses leaves the register");
}

// 8. A known model charged by the MP2731 is never written: the strict gate is
//    the model table, not an absent MP2731 node. host_applies is reset true, so
//    this case runs the same model expression the device build runs.
static void case_mp2731_model_no_write(void) {
	axpcharge_test_reset();
	set_test_model(true, false);
	CHECK(!axpcharge_set_charging(false), "mp2731_model_no_write returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "mp2731_model_no_write leaves the register");
	CHECK(!axpcharge_charging_control_supported(), "mp2731_model_no_write gate is false");
	set_test_model(false, false);
}

// 8b. The host stub can close the gate too, as it does when the simulator has
//     no model named and the test asks for an unsupported player.
static void case_host_gate_closed(void) {
	axpcharge_test_reset();
	axpcharge_test_set_applies(false);
	CHECK(!axpcharge_set_charging(false), "host_gate_closed returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "host_gate_closed leaves the register");
	CHECK(!axpcharge_charging_control_supported(), "host_gate_closed gate is false");
}

// 9. The driver can set the bit again after a gauge reset; the tick bounds it.
static void case_tick_reclears_driver_write(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "tick_reclears_driver_write stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "tick_reclears_driver_write starts stopped");
	axpcharge_test_set_reg(REG18, 0x0e);
	axpcharge_test_advance_ms(2000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0c, "tick_reclears_driver_write clears bit 1 again");
}

// 10. A register that cannot be read is not written blind.
static void case_read_failure_reverts(void) {
	axpcharge_test_reset();
	axpcharge_test_fail_read(true);
	CHECK(!axpcharge_set_charging(false), "read_failure_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "read_failure_reverts leaves the register");
	axpcharge_test_fail_read(false);
}

// 11. A write that lands but whose confirm read fails is recovered: the last
//     confirmed state is written back, so the PMIC and the icon agree.
static void case_confirm_read_failure_restores(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	axpcharge_test_fail_next_confirm_read();
	CHECK(!axpcharge_set_charging(false), "confirm_read_failure_restores returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "confirm_read_failure_restores puts the bit back");
	CHECK(!axpcharge_charging_blocked(), "confirm_read_failure_restores reports allowed");
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "confirm_read_failure_restores stays allowed at the tick");
}

// 12. A first stop that finds the bit already clear still remembers an allowed
//     cell, so a release can put it back.
static void case_saved_bit_already_clear(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x08);
	CHECK(axpcharge_set_charging(false), "saved_bit_already_clear stops");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "saved_bit_already_clear leaves the bit clear");
	CHECK(axpcharge_charging_blocked(), "saved_bit_already_clear reports blocked");
	CHECK(axpcharge_charging_release(), "saved_bit_already_clear releases");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "saved_bit_already_clear restores the bit on release");
}

// 13. A new stop cancels a release the tick had queued after a failed write.
static void case_stop_cancels_pending_release(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "stop_cancels_pending_release stops first");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_charging_release(), "stop_cancels_pending_release leaves a pending release");
	axpcharge_test_fail_write(false);
	CHECK(axpcharge_set_charging(false), "stop_cancels_pending_release takes the new stop");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "stop_cancels_pending_release keeps the bit clear");
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "stop_cancels_pending_release is not undone by the tick");
}

// 14. The crash handler's quiet release: it cannot take a held lock, and it
//     restores the bit when it can.
static void case_quiet_release(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "quiet_release stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "quiet_release starts stopped");
	axpcharge_test_hold_lock();
	CHECK(!axpcharge_charging_release_quiet(), "quiet_release trylock fails");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "quiet_release leaves the bit when locked");
	axpcharge_test_release_lock();
	CHECK(axpcharge_charging_release_quiet(), "quiet_release restores the bit");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "quiet_release sets bit 1");
	CHECK(!axpcharge_charging_blocked(), "quiet_release reports unblocked");
}

// 15. A confirmed release is not re-asserted by the tick: a later write by the
//     stock driver stays put until a new stop is asked for.
static void case_release_stops_reasserting(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_stops_reasserting stops first");
	CHECK(axpcharge_charging_release(), "release_stops_reasserting releases");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_stops_reasserting restores the bit");
	axpcharge_test_set_reg(REG18, 0x08);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_stops_reasserting leaves a later driver write alone");
}

int main(void) {
	case_supported_r1();
	case_stop_clears_only_bit1();
	case_resume_sets_bit1();
	case_refused_off_reverts();
	case_refused_on_reverts();
	case_release_retries();
	case_release_waits_5s();
	case_release_enables();
	case_low_cell_refuses();
	case_mp2731_model_no_write();
	case_host_gate_closed();
	case_tick_reclears_driver_write();
	case_read_failure_reverts();
	case_confirm_read_failure_restores();
	case_saved_bit_already_clear();
	case_stop_cancels_pending_release();
	case_quiet_release();
	case_release_stops_reasserting();
	printf("all axpcharge cases passed\n");
	return 0;
}
