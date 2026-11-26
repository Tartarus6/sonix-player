#include "src/gui/library/browserstate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPECT(cond) do { if (!(cond)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

#define TEST_STACK_MAX 12
#define TEST_FOLDER_MAX 512

// A hand-written V2 record, so a bad field can be put on disk without using
// the code under test to build it.
typedef struct {
	char magic[8];
	int32_t scroll;
	int32_t stack_len;
	int32_t stack[TEST_STACK_MAX];
	char folder[TEST_FOLDER_MAX];
} test_disk_v2_t;

static void write_v2(const char *file, int scroll, int stack_len, const int *stack, const char *folder) {
	test_disk_v2_t rec;
	memset(&rec, 0, sizeof(rec));
	memcpy(rec.magic, "SNXBRW2", 8);
	rec.scroll = scroll;
	rec.stack_len = stack_len;
	if (stack && stack_len > 0) {
		memcpy(rec.stack, stack, (size_t)stack_len * sizeof(int32_t));
	}
	snprintf(rec.folder, sizeof(rec.folder), "%s", folder);
	FILE *f = fopen(file, "wb");
	if (f) {
		fwrite(&rec, sizeof(rec), 1, f);
		fclose(f);
	}
}

static void write_v1(const char *file, int scroll, const char *folder) {
	char rec[8 + 4 + TEST_FOLDER_MAX];
	memset(rec, 0, sizeof(rec));
	memcpy(rec, "SNXBRW1", 8);
	memcpy(rec + 8, &scroll, sizeof(scroll));
	snprintf(rec + 12, TEST_FOLDER_MAX, "%s", folder);
	FILE *f = fopen(file, "wb");
	if (f) {
		fwrite(rec, 1, sizeof(rec), f);
		fclose(f);
	}
}

// The reported bug: the ancestor scrolls must survive a save/load round trip.
static int test_stack_round_trip(void) {
	char file[] = "/tmp/sonix-bstate-XXXXXX";
	int fd = mkstemp(file);
	EXPECT(fd >= 0);
	close(fd);

	const char *folder = "/mnt/sdcard/Music/Artist";
	int stack[TEST_STACK_MAX] = {11, 22, 33};
	EXPECT(browser_state_save(file, folder, 500, stack, 3));

	char got_folder[256] = "";
	int got_scroll = -1;
	int got_stack[TEST_STACK_MAX] = {0};
	int got_len = -1;
	EXPECT(browser_state_load(file, got_folder, sizeof(got_folder), &got_scroll, got_stack, TEST_STACK_MAX, &got_len));
	EXPECT(strcmp(got_folder, folder) == 0);
	EXPECT(got_scroll == 500);
	EXPECT(got_len == 3);
	EXPECT(got_stack[0] == 11);
	EXPECT(got_stack[1] == 22);
	EXPECT(got_stack[2] == 33);

	unlink(file);
	return 0;
}

static int test_v1_loads_with_empty_stack(void) {
	char file[] = "/tmp/sonix-bstate-XXXXXX";
	int fd = mkstemp(file);
	EXPECT(fd >= 0);
	close(fd);

	write_v1(file, 700, "/mnt/sdcard/Music/Album");

	char got_folder[256] = "";
	int got_scroll = -1;
	int got_stack[TEST_STACK_MAX] = {0};
	int got_len = -1;
	EXPECT(browser_state_load(file, got_folder, sizeof(got_folder), &got_scroll, got_stack, TEST_STACK_MAX, &got_len));
	EXPECT(strcmp(got_folder, "/mnt/sdcard/Music/Album") == 0);
	EXPECT(got_scroll == 700);
	EXPECT(got_len == 0);

	unlink(file);
	return 0;
}

static int test_rejects_bad_stack(void) {
	char file[] = "/tmp/sonix-bstate-XXXXXX";
	int fd = mkstemp(file);
	EXPECT(fd >= 0);
	close(fd);

	char got_folder[256] = "";
	int got_scroll = -1;
	int got_stack[TEST_STACK_MAX] = {0};
	int got_len = -1;

	write_v2(file, 500, 99, NULL, "/mnt/sdcard/Music/Album");
	EXPECT(!browser_state_load(file, got_folder, sizeof(got_folder), &got_scroll, got_stack, TEST_STACK_MAX, &got_len));

	int negative = -1;
	write_v2(file, 500, 1, &negative, "/mnt/sdcard/Music/Album");
	EXPECT(!browser_state_load(file, got_folder, sizeof(got_folder), &got_scroll, got_stack, TEST_STACK_MAX, &got_len));

	int full[TEST_STACK_MAX];
	for (int i = 0; i < TEST_STACK_MAX; i++) {
		full[i] = i;
	}
	write_v2(file, 500, TEST_STACK_MAX, full, "/mnt/sdcard/Music/Album");
	EXPECT(browser_state_load(file, got_folder, sizeof(got_folder), &got_scroll, got_stack, TEST_STACK_MAX, &got_len));
	EXPECT(got_len == TEST_STACK_MAX);
	EXPECT(got_stack[TEST_STACK_MAX - 1] == TEST_STACK_MAX - 1);

	unlink(file);
	return 0;
}

// The stack must only be applied when the saved folder was really used; a
// fallback to the track/Music/root must not inherit a stale ancestor scroll.
static int test_choose_reports_saved_usage(void) {
	char dir[] = "/tmp/sonix-bstate-dir-XXXXXX";
	EXPECT(mkdtemp(dir) != NULL);
	char root[640];
	snprintf(root, sizeof(root), "%s/root", dir);
	char artist[4096];
	snprintf(artist, sizeof(artist), "%s/Music/Artist", root);
	char track[4096];
	snprintf(track, sizeof(track), "%s/Music/song.mp3", root);
	char music[4096];
	snprintf(music, sizeof(music), "%s/Music", root);
	EXPECT(mkdir(root, 0755) == 0);
	EXPECT(mkdir(music, 0755) == 0);
	EXPECT(mkdir(artist, 0755) == 0);
	FILE *f = fopen(track, "wb");
	EXPECT(f != NULL);
	fclose(f);

	char out[512] = "";
	int scroll = -1;
	bool used = false;
	EXPECT(browser_state_choose(root, artist, 500, NULL, out, sizeof(out), &scroll, &used));
	EXPECT(used);
	EXPECT(strcmp(out, artist) == 0);
	EXPECT(scroll == 500);

	out[0] = '\0';
	scroll = -1;
	used = true;
	EXPECT(browser_state_choose(root, "/no/such/folder", 0, track, out, sizeof(out), &scroll, &used));
	EXPECT(!used);
	EXPECT(strcmp(out, music) == 0);
	EXPECT(scroll == 0);

	unlink(track);
	rmdir(artist);
	rmdir(music);
	rmdir(root);
	rmdir(dir);
	return 0;
}

int main(void) {
	if (test_stack_round_trip())
		return 1;
	if (test_v1_loads_with_empty_stack())
		return 1;
	if (test_rejects_bad_stack())
		return 1;
	if (test_choose_reports_saved_usage())
		return 1;
	puts("browserstate_test: ok");
	return 0;
}
