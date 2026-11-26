// Host-only check: lang_list() must return language names in alphabetical
// order, whatever order readdir() hands back.
#include "src/system/core/lang.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

// main.c defines these for the player. This test leaves main.o out, so supply
// the five display helpers that the other objects call.
void display_wake_begin(lv_display_t *disp) { (void)disp; }
void display_wake_end(lv_display_t *disp) { (void)disp; }
bool display_rotation_supported(void) { return false; }
void display_set_rotated(bool rotated) { (void)rotated; }
uint16_t *display_capture_frame(int *out_w, int *out_h) {
	if (out_w) {
		*out_w = 0;
	}
	if (out_h) {
		*out_h = 0;
	}
	return NULL;
}

static void write_ini(const char *dir, const char *name) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ini", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fprintf(f, "; %s\n", name);
	fclose(f);
}

static void write_ini_labeled(const char *dir, const char *name, const char *label) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s.ini", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fprintf(f, "[Language: %s]\n", label);
	fclose(f);
}

int main(void) {
	char dir[] = "/tmp/lang-sort-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 2;
	}

	// Mixed case and a pair that folds to the same key. Raw byte order and
	// collated order cannot coincide on any filesystem.
	const char *created[] = {"Zulu",	 "alpha",   "Ukrainian", "Russian", "Português BR",
							 "Polski",	 "Nederlands", "Japanese", "Italiano", "Français",
							 "Español", "Espanol", "English",	"Deutsch",	"Beta",
							 "Čeština"};
	const char *duplicates[] = {"Dup one", "Dup two"};
	const size_t total = sizeof(created) / sizeof(created[0]);
	for (size_t i = 0; i < total; i++) {
		write_ini(dir, created[i]);
	}
	// Two files declaring one language: dedup must leave a single entry.
	for (size_t i = 0; i < sizeof(duplicates) / sizeof(duplicates[0]); i++) {
		write_ini_labeled(dir, duplicates[i], "Same Language");
	}

	lang_init(dir);
	const char *names[MAX_LANGUAGES] = {0};
	int count = lang_list(names);
	int status = 0;
	if (count != (int)total + 1) {
		fprintf(stderr, "FAIL: expected %zu languages, got %d\n", total + 1, count);
		status = 1;
	} else {
		const char *expected[] = {"alpha",	 "Beta",	  "Čeština",	"Deutsch", "English",
								  "Espanol", "Español",  "Français", "Italiano", "Japanese",
								  "Nederlands", "Polski", "Português BR", "Russian", "Ukrainian",
								  "Zulu"};
		int dups = 0, pos = 0;
		for (int i = 0; i < count; i++) {
			if (strcmp(names[i], duplicates[0]) == 0 || strcmp(names[i], duplicates[1]) == 0) {
				dups++;
				continue;
			}
			if (pos >= (int)total || strcmp(names[i], expected[pos]) != 0) {
				fprintf(stderr, "FAIL: position %d: expected %s, got %s\n", i,
						pos < (int)total ? expected[pos] : "(nothing)", names[i]);
				status = 1;
			}
			pos++;
		}
		if (dups != 1 || pos != (int)total) {
			fprintf(stderr, "FAIL: expected one duplicate entry, got %d\n", dups);
			status = 1;
		}
	}
	if (!status) {
		puts("PASS: lang_list() returns one sorted, deduplicated language per file");
	}

	for (size_t i = 0; i < total; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.ini", dir, created[i]);
		unlink(path);
	}
	for (size_t i = 0; i < sizeof(duplicates) / sizeof(duplicates[0]); i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.ini", dir, duplicates[i]);
		unlink(path);
	}
	rmdir(dir);
	return status;
}
