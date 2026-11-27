#ifndef BLUETOOTHD_FALLBACK_H
#define BLUETOOTHD_FALLBACK_H

#include <stdbool.h>

// Which bluetoothd answered org.bluez after bring-up.
typedef enum {
	BT_UP_NONE = 0,
	BT_UP_PATCHED, // /usr/libexec/bluetooth/browse/bluetoothd
	BT_UP_STOCK,   // /usr/libexec/bluetooth/bluetoothd
} bt_up_t;

// The three things the bring-up needs from the rest of the Bluetooth code,
// injected so the sequence can be tested without a radio, a bus or a daemon.
// spawn() starts `path` with `argv` and reports whether it could be started;
// wait() asks whether bluez has claimed the bus and found hci0 within the
// milliseconds it is given; kill() removes every process named "bluetoothd".
typedef bool (*bt_daemon_spawn_fn)(const char *path, char *const argv[], void *user);
typedef bool (*bt_daemon_wait_fn)(int timeout_ms, void *user);
typedef bool (*bt_daemon_kill_fn)(void *user);

typedef struct {
	const char *patched_path;
	const char *stock_path;
	// When this file exists the patched daemon is deliberately turned off.
	const char *disable_flag;
	const char *conf;
	bool debug; // bluez's -d, only when the Bluetooth log is on
	int deadline_ms;
	bt_daemon_spawn_fn spawn;
	bt_daemon_wait_fn wait;
	bt_daemon_kill_fn kill;
	void *user;
} bt_daemon_fallback_t;

// At most two attempts, sharing one deadline. A patched daemon that fails has
// to be killed before the stock one may start; if the kill is refused, the
// stock daemon is never started, because two bluetoothds would fight over
// org.bluez and leave the radio in a worse state than no daemon at all.
bt_up_t bt_daemon_bring_up(const bt_daemon_fallback_t *f);

#endif /* BLUETOOTHD_FALLBACK_H */
