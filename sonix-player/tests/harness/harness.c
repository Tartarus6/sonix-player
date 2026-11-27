#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/bluetooth/bluetoothd_fallback.h"

#define PATCHED "/usr/libexec/bluetooth/browse/bluetoothd"
#define STOCK "/usr/libexec/bluetooth/bluetoothd"

static int checks;
static int failures;

#define CHECK(cond)                                                                                                \
	do {                                                                                                           \
		checks++;                                                                                                  \
		if (!(cond)) {                                                                                             \
			failures++;                                                                                            \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
		}                                                                                                          \
	} while (0)

typedef struct {
	int spawn_calls;
	char paths[4][256];
	char argv0[4][64];
	bool spawn_ok[4];
	int wait_calls;
	int timeouts[4];
	bool wait_ok[4];
	int sleep_ms;
	int kill_calls;
	bool kill_ok;
} fake_ctx_t;

static fake_ctx_t ctx;
static char disable_flag[512];

static bool fake_spawn(const char *path, char *const argv[], void *user) {
	(void)user;
	int i = ctx.spawn_calls++;
	snprintf(ctx.paths[i], sizeof(ctx.paths[i]), "%s", path);
	snprintf(ctx.argv0[i], sizeof(ctx.argv0[i]), "%s", argv[0]);
	return ctx.spawn_ok[i];
}

static bool fake_wait(int timeout_ms, void *user) {
	(void)user;
	int i = ctx.wait_calls++;
	ctx.timeouts[i] = timeout_ms;
	if (ctx.sleep_ms) {
		usleep((unsigned int)ctx.sleep_ms * 1000);
	}
	return ctx.wait_ok[i];
}

static bool fake_kill(void *user) {
	(void)user;
	ctx.kill_calls++;
	return ctx.kill_ok;
}

static void reset_ctx(void) {
	memset(&ctx, 0, sizeof(ctx));
	ctx.kill_ok = true;
	for (int i = 0; i < 4; i++) {
		ctx.spawn_ok[i] = true;
		ctx.wait_ok[i] = true;
	}
}

static bt_daemon_fallback_t config(int deadline_ms) {
	bt_daemon_fallback_t f = {
		.patched_path = PATCHED,
		.stock_path = STOCK,
		.disable_flag = disable_flag,
		.debug = false,
		.deadline_ms = deadline_ms,
		.spawn = fake_spawn,
		.wait = fake_wait,
		.kill = fake_kill,
		.user = &ctx,
	};
	return f;
}

static void set_flag(bool present) {
	unlink(disable_flag);
	if (present) {
		FILE *f = fopen(disable_flag, "w");
		if (f) {
			fclose(f);
		}
	}
}

// 1: the patched daemon answers on the first try.
static void case_patched_ok(void) {
	reset_ctx();
	set_flag(false);
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_PATCHED);
	CHECK(ctx.spawn_calls == 1);
	CHECK(strcmp(ctx.paths[0], PATCHED) == 0);
	CHECK(strcmp(ctx.argv0[0], "bluetoothd") == 0);
	CHECK(ctx.wait_calls == 1);
	CHECK(ctx.kill_calls == 0);
}

// 2: the opt-out flag sends a unit straight to the stock daemon.
static void case_disabled(void) {
	reset_ctx();
	set_flag(true);
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_STOCK);
	CHECK(ctx.spawn_calls == 1);
	CHECK(strcmp(ctx.paths[0], STOCK) == 0);
	CHECK(strcmp(ctx.argv0[0], "bluetoothd") == 0);
	CHECK(ctx.kill_calls == 0);
}

// 3: a failed patched daemon is killed before stock gets a turn.
static void case_fallback(void) {
	reset_ctx();
	set_flag(false);
	ctx.wait_ok[0] = false;
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_STOCK);
	CHECK(ctx.spawn_calls == 2);
	CHECK(strcmp(ctx.paths[0], PATCHED) == 0);
	CHECK(strcmp(ctx.paths[1], STOCK) == 0);
	CHECK(ctx.kill_calls == 1);
	CHECK(ctx.timeouts[1] <= ctx.timeouts[0]);
}

// 4: a kill that is refused means no second bluetoothd, ever.
static void case_kill_refused(void) {
	reset_ctx();
	set_flag(false);
	ctx.wait_ok[0] = false;
	ctx.kill_ok = false;
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_NONE);
	CHECK(ctx.spawn_calls == 1);
	CHECK(ctx.kill_calls == 1);
	CHECK(ctx.wait_calls == 1);
	CHECK(ctx.paths[1][0] == '\0');
}

// 5: if the patched binary is not there, nothing has to be killed.
static void case_spawn_failed(void) {
	reset_ctx();
	set_flag(false);
	ctx.spawn_ok[0] = false;
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_STOCK);
	CHECK(ctx.spawn_calls == 2);
	CHECK(ctx.kill_calls == 0);
	CHECK(strcmp(ctx.paths[1], STOCK) == 0);
}

// 6: the second wait gets only what the first one left of the deadline.
static void case_shared_deadline(void) {
	reset_ctx();
	set_flag(false);
	ctx.wait_ok[0] = false;
	ctx.sleep_ms = 200;
	bt_daemon_fallback_t f = config(300);
	CHECK(ctx.timeouts[0] == 0); // not yet called
	CHECK(bt_daemon_bring_up(&f) == BT_UP_STOCK);
	CHECK(ctx.timeouts[1] > 0 && ctx.timeouts[1] <= 100);
	CHECK(ctx.spawn_calls == 2);
}

// 7: neither daemon comes up.
static void case_both_fail(void) {
	reset_ctx();
	set_flag(false);
	ctx.wait_ok[0] = false;
	ctx.wait_ok[1] = false;
	bt_daemon_fallback_t f = config(8000);
	CHECK(bt_daemon_bring_up(&f) == BT_UP_NONE);
	CHECK(ctx.spawn_calls == 2);
	CHECK(ctx.wait_calls == 2);
	CHECK(ctx.kill_calls == 1);
	CHECK(ctx.timeouts[1] <= ctx.timeouts[0]);
}

int main(void) {
	const char *tmp = getenv("TMPDIR");
	snprintf(disable_flag, sizeof(disable_flag), "%s/sonix-bt-browse-disable-%d", tmp ? tmp : "/tmp", getpid());

	case_patched_ok();
	case_disabled();
	case_fallback();
	case_kill_refused();
	case_spawn_failed();
	case_shared_deadline();
	case_both_fail();

	unlink(disable_flag);
	printf("fallback_harness: %d checks, %d failures\n", checks, failures);
	if (checks != 35) {
		fprintf(stderr, "fallback_harness: expected 35 assertions, got %d\n", checks);
		return 1;
	}
	return failures ? 1 : 0;
}
