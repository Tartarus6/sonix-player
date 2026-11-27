#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fake_provider.h"
#include "support/dbuslite_stub.h"

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

static int dummy_conn;

typedef struct {
	char path[320];
	char name[256];
	char type[32];
	char folder_type[32];
	char url[512];
	bool playable;
} item_info_t;

static char nested_album[320];

// ---------------------------------------------------------------------------
// Incoming calls and replies
// ---------------------------------------------------------------------------

static bool call_method(const char *path, const char *iface, const char *member, const uint8_t *body, size_t len) {
	dbus_test_reset();
	dbus_msg_t m;
	memset(&m, 0, sizeof(m));
	m.type = DBUS_TYPE_METHOD_CALL;
	snprintf(m.path, sizeof(m.path), "%s", path);
	snprintf(m.interface, sizeof(m.interface), "%s", iface);
	snprintf(m.member, sizeof(m.member), "%s", member);
	m.body = body;
	m.body_len = len;
	return btbrowser_on_call((dbus_conn_t *)&dummy_conn, &m, NULL);
}

// mode 0: valid Start/End; 1: Start with the wrong signature; 2: truncated body.
static size_t filter_body(uint8_t *buf, uint32_t start, uint32_t end, int mode) {
	dbus_test_scratch_reset();
	dbus_writer_t *w = dbus_test_scratch();
	dbus_array_t a;
	dbus_w_array_begin(w, "{sv}", &a);
	if (mode == 1) {
		dbus_w_dict_variant_begin(w, "Start", "s");
		dbus_w_string(w, "not a uint32");
	} else {
		dbus_w_dict_variant_begin(w, "Start", "u");
		dbus_w_u32(w, start);
		dbus_w_dict_variant_begin(w, "End", "u");
		dbus_w_u32(w, end);
	}
	dbus_w_array_end(w, &a);
	if (mode == 2) {
		// The array's length word claims more than the message holds.
		return dbus_test_scratch_copy(buf, 6);
	}
	return dbus_test_scratch_copy(buf, 4096);
}

static bool list_folder(const char *path, uint32_t start, uint32_t end, item_info_t *out, int max, int *count) {
	uint8_t body[4096];
	size_t len = filter_body(body, start, end, 0);
	bool handled = call_method(path, "org.bluez.MediaFolder1", "ListItems", body, len);
	*count = 0;
	if (!handled || dbus_test_result()->is_error) {
		return handled;
	}

	dbus_msg_t m;
	memset(&m, 0, sizeof(m));
	m.body = dbus_test_result()->body;
	m.body_len = dbus_test_result()->len;
	dbus_reader_t r;
	dbus_reader_init(&r, &m);
	dbus_array_iter_t it;
	if (!dbus_r_array_begin(&r, "(oa{sv})", &it)) {
		return handled;
	}
	while (dbus_r_array_more(&r, &it) && *count < max) {
		item_info_t *o = &out[*count];
		if (!dbus_r_string(&r, o->path, sizeof(o->path))) {
			break;
		}
		dbus_array_iter_t props;
		if (!dbus_r_array_begin(&r, "{sv}", &props)) {
			break;
		}
		while (dbus_r_array_more(&r, &props)) {
			char key[64], sig[64];
			if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
				break;
			}
			if (strcmp(key, "Name") == 0 && strcmp(sig, "s") == 0) {
				dbus_r_string(&r, o->name, sizeof(o->name));
			} else if (strcmp(key, "Type") == 0 && strcmp(sig, "s") == 0) {
				dbus_r_string(&r, o->type, sizeof(o->type));
			} else if (strcmp(key, "FolderType") == 0 && strcmp(sig, "s") == 0) {
				dbus_r_string(&r, o->folder_type, sizeof(o->folder_type));
			} else if (strcmp(key, "Playable") == 0 && strcmp(sig, "b") == 0) {
				dbus_r_bool(&r, &o->playable);
			} else if (strcmp(key, "Metadata") == 0) {
				if (strcmp(sig, "a{sv}") == 0) {
					dbus_array_iter_t meta;
					if (dbus_r_array_begin(&r, "{sv}", &meta)) {
						while (dbus_r_array_more(&r, &meta)) {
							char mkey[64], msig[64];
							if (!dbus_r_string(&r, mkey, sizeof(mkey)) ||
								!dbus_r_signature(&r, msig, sizeof(msig))) {
								break;
							}
							if (strcmp(mkey, "xesam:url") == 0 && strcmp(msig, "s") == 0) {
								dbus_r_string(&r, o->url, sizeof(o->url));
							} else {
								dbus_r_skip(&r, msig);
							}
						}
					}
				}
			} else {
				dbus_r_skip(&r, sig);
			}
		}
		(*count)++;
	}
	return handled;
}

static bool change_folder(const char *path) {
	uint8_t body[4096];
	dbus_test_scratch_reset();
	dbus_writer_t *w = dbus_test_scratch();
	dbus_w_string(w, path);
	size_t len = dbus_test_scratch_copy(body, sizeof(body));
	return call_method(BROWSE_ROOT, "org.bluez.MediaFolder1", "ChangeFolder", body, len);
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

static void test_root(void) {
	item_info_t items[8];
	int n = 0;
	CHECK(list_folder(BROWSE_ROOT, 0, 2, items, 8, &n));
	CHECK(n == 3);
	if (n == 3) {
		CHECK(strcmp(items[0].path, BROWSE_ROOT "/Artists") == 0);
		CHECK(strcmp(items[1].path, BROWSE_ROOT "/Albums") == 0);
		CHECK(strcmp(items[2].path, BROWSE_TRACKS_PATH) == 0);
		CHECK(strcmp(items[0].name, "Artists") == 0);
		CHECK(strcmp(items[1].name, "Albums") == 0);
		CHECK(strcmp(items[2].name, "Tracks") == 0);
		for (int i = 0; i < 3; i++) {
			CHECK(strcmp(items[i].type, "folder") == 0);
			CHECK(items[i].playable == false);
		}
	}
}

static void test_hierarchy(void) {
	item_info_t artists[8], albums[8], tracks[8];
	int n = 0;

	CHECK(list_folder(BROWSE_ROOT "/Artists", 0, 20, artists, 8, &n));
	CHECK(n == 2);
	if (n == 2) {
		CHECK(strcmp(artists[0].name, "Artist One") == 0);
		// The library key's 0x1F suffix never reaches the head unit.
		CHECK(strcmp(artists[1].name, "Artist Two") == 0);
		CHECK(strcmp(artists[1].folder_type, "artists") == 0);
	}

	CHECK(change_folder(artists[1].path));
	CHECK(!dbus_test_result()->is_error);

	CHECK(list_folder(artists[1].path, 0, 20, albums, 8, &n));
	CHECK(n == 2);
	if (n == 2) {
		CHECK(strcmp(albums[0].name, "Album B") == 0);
		CHECK(strcmp(albums[1].name, "Album C") == 0);
		CHECK(strcmp(albums[0].folder_type, "albums") == 0);
		snprintf(nested_album, sizeof(nested_album), "%s", albums[0].path);
	}

	CHECK(change_folder(nested_album));
	CHECK(!dbus_test_result()->is_error);

	CHECK(list_folder(nested_album, 0, 20, tracks, 8, &n));
	CHECK(n == 2);
	if (n == 2) {
		CHECK(strcmp(tracks[0].name, "Two Song") == 0);
		CHECK(strcmp(tracks[0].url, FAKE_TRACK1) == 0);
		CHECK(strcmp(tracks[1].name, "Three Song") == 0);
	}

	CHECK(list_folder(BROWSE_ROOT "/Albums", 0, 20, albums, 8, &n));
	CHECK(n == 3);
	if (n == 3) {
		CHECK(strcmp(albums[0].name, "Album A") == 0);
		CHECK(strcmp(albums[1].name, "Album B") == 0);
		CHECK(list_folder(albums[1].path, 0, 20, tracks, 8, &n));
		CHECK(n == 2);
		if (n == 2) {
			CHECK(strcmp(tracks[0].url, FAKE_TRACK1) == 0);
		}
	}
}

static void test_nested_play(void) {
	char item[400];
	snprintf(item, sizeof(item), "%s/item/%08x", nested_album, 0);
	fake_provider_reset();
	CHECK(call_method(item, "org.bluez.MediaItem1", "Play", NULL, 0));
	CHECK(!dbus_test_result()->is_error);
	CHECK(fake.play_calls == 1);
	CHECK(strcmp(fake.last_played, FAKE_TRACK1) == 0);

	snprintf(item, sizeof(item), "%s/item/%08x", nested_album, 1);
	fake_provider_reset();
	CHECK(call_method(item, "org.bluez.MediaItem1", "Play", NULL, 0));
	CHECK(!dbus_test_result()->is_error);
	CHECK(fake.play_calls == 1);
	CHECK(strcmp(fake.last_played, "/sdcard/music/Three Song.mp3") == 0);
}

static void test_end_at_max(void) {
	item_info_t items[8];
	int n = 0;
	CHECK(list_folder(BROWSE_ROOT, 0, 0xffffffffu, items, 8, &n));
	CHECK(n == 3);
}

static void test_change_folder_root(void) {
	CHECK(change_folder(BROWSE_ROOT));
	CHECK(!dbus_test_result()->is_error);
	CHECK(change_folder("/org/mpris/NoSuchFolder"));
	CHECK(dbus_test_result()->is_error);
	CHECK(strstr(dbus_test_result()->error, "InvalidArguments") != NULL);
}

static void test_root_after_descent(void) {
	item_info_t items[8];
	int n = 0;
	CHECK(list_folder(BROWSE_ROOT, 0, 2, items, 8, &n));
	CHECK(n == 3);
}

static void test_malformed_filter(void) {
	uint8_t body[4096];
	size_t len = filter_body(body, 0, 2, 1);
	CHECK(call_method(BROWSE_TRACKS_PATH, "org.bluez.MediaFolder1", "ListItems", body, len));
	CHECK(dbus_test_result()->is_error);
	CHECK(strstr(dbus_test_result()->error, "InvalidArguments") != NULL);

	len = filter_body(body, 0, 2, 2);
	CHECK(call_method(BROWSE_TRACKS_PATH, "org.bluez.MediaFolder1", "ListItems", body, len));
	CHECK(dbus_test_result()->is_error);
}

static void test_stale_retry(void) {
	item_info_t items[8];
	int n = 0;
	fake_provider_reset();
	fake.stale_once = 1;
	CHECK(list_folder(BROWSE_TRACKS_PATH, 0, 0, items, 8, &n));
	CHECK(!dbus_test_result()->is_error);
	CHECK(n == 1);
	CHECK(fake.window_calls == 2);
	if (n == 1) {
		CHECK(strcmp(items[0].url, FAKE_TRACK0) == 0);
	}
}

static void test_play_and_getall(void) {
	char item[400];
	snprintf(item, sizeof(item), "%s/item/%08x", BROWSE_TRACKS_PATH, 0);
	fake_provider_reset();
	CHECK(call_method(item, "org.bluez.MediaItem1", "Play", NULL, 0));
	CHECK(!dbus_test_result()->is_error);
	CHECK(fake.play_calls == 1);
	CHECK(strcmp(fake.last_played, FAKE_TRACK0) == 0);

	CHECK(call_method(item, "org.freedesktop.DBus.Properties", "GetAll", NULL, 0));
	CHECK(!dbus_test_result()->is_error);

	// Walk the reply's Metadata dictionary and find the real path there.
	dbus_msg_t m;
	memset(&m, 0, sizeof(m));
	m.body = dbus_test_result()->body;
	m.body_len = dbus_test_result()->len;
	dbus_reader_t r;
	dbus_reader_init(&r, &m);
	dbus_array_iter_t props;
	bool found = false;
	if (dbus_r_array_begin(&r, "{sv}", &props)) {
		while (dbus_r_array_more(&r, &props)) {
			char key[64], sig[64];
			if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
				break;
			}
			if (strcmp(key, "Metadata") == 0 && strcmp(sig, "a{sv}") == 0) {
				dbus_array_iter_t meta;
				if (dbus_r_array_begin(&r, "{sv}", &meta)) {
					while (dbus_r_array_more(&r, &meta)) {
						char mk[64], ms[64];
						if (!dbus_r_string(&r, mk, sizeof(mk)) || !dbus_r_signature(&r, ms, sizeof(ms))) {
							break;
						}
						if (strcmp(mk, "xesam:url") == 0) {
							char url[512];
							dbus_r_string(&r, url, sizeof(url));
							found = strcmp(url, FAKE_TRACK0) == 0;
						} else {
							dbus_r_skip(&r, ms);
						}
					}
				}
			} else {
				dbus_r_skip(&r, sig);
			}
		}
	}
	CHECK(found);
}

int main(void) {
	fake_provider_reset();
	dbus_test_reset();
	btbrowser_init((dbus_conn_t *)&dummy_conn, &fake_bt_provider);

	test_root();
	test_hierarchy();
	test_nested_play();
	test_end_at_max();
	test_change_folder_root();
	test_root_after_descent();
	test_malformed_filter();
	test_stale_retry();
	test_play_and_getall();

	btbrowser_shutdown();
	printf("btbrowser_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
