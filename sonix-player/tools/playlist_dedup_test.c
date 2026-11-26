// Host test: two copies of a playlist whose names differ only in case must
// produce one candidate whose name, path and exists flag all describe the
// winning file. The playlist name is derived from that path on import, so a
// mismatch here either hides the overwrite warning or reports the wrong one.
//
// Built from the Makefile's test-playlist-dedup target with --gc-sections, so
// the two small library.c/utils.c stand-ins below are all the player code the
// tested functions need.

#include "src/system/core/utils.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

void file_identity_read(const char *path, file_identity_t *out) {
	(void)path;
	if (out) {
		memset(out, 0, sizeof(*out));
	}
}
void thread_be_background(const char *name) { (void)name; }

static void must(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(1);
	}
}

static char card[512];

static void new_card(void) {
	char template[] = "/tmp/playlist-dedup-XXXXXX";
	must(mkdtemp(template) != NULL, "mkdtemp");
	snprintf(card, sizeof(card), "%s", template);
	char folder[600];
	snprintf(folder, sizeof(folder), "%s/Playlist", card);
	must(mkdir(folder, 0700) == 0, "mkdir Playlist");
	snprintf(folder, sizeof(folder), "%s/playlist_data", card);
	must(mkdir(folder, 0700) == 0, "mkdir playlist_data");
	playlists_init(card);
}

static void write_m3u(const char *path, time_t mtime) {
	FILE *f = fopen(path, "w");
	must(f != NULL, "create m3u");
	fputs("#EXTM3U\n", f);
	fclose(f);
	struct utimbuf times = { .actime = mtime, .modtime = mtime };
	must(utime(path, &times) == 0, "set m3u mtime");
}

// An older copy at the card root, a newer copy in Playlist. The player may
// already hold either spelling of the name.
static void check_case(const char *existing, const char *root_name, const char *backup_name, bool expect_exists) {
	new_card();
	char path[700];
	snprintf(path, sizeof(path), "%s/%s.m3u", card, root_name);
	write_m3u(path, 1000000000);
	snprintf(path, sizeof(path), "%s/Playlist/%s.m3u", card, backup_name);
	write_m3u(path, 1000001000);
	if (existing) {
		must(library_playlist_create(existing), "create existing playlist");
	}

	playlists_candidate_t candidates[8];
	int count = playlists_importable(candidates, 8);
	must(count == 1, "one candidate for two copies of one name");
	must(strcmp(candidates[0].name, backup_name) == 0, "name from the winning path");
	must(strstr(candidates[0].path, "/Playlist/") != NULL, "newer copy wins");
	must(candidates[0].exists == expect_exists, "exists matches the winning name");
	if (existing) {
		must(library_playlist_exists(root_name) && library_playlist_exists(backup_name),
			 "playlist existence ignores case");
	}
	library_playlist_drop(existing ? existing : backup_name);
}

int main(void) {
	char db_dir[] = "/tmp/playlist-dedup-db-XXXXXX";
	must(mkdtemp(db_dir) != NULL, "mkdtemp db");
	char db_path[600];
	snprintf(db_path, sizeof(db_path), "%s/library.db", db_dir);
	must(library_open(db_path), "open library");

	// The player holds "Hits", the newer backup is "Hits.m3u" and the older
	// root copy is "hits.m3u": the warning must be shown for that backup.
	check_case("Hits", "hits", "Hits", true);
	// And the other way around: the backup is "hits.m3u", so the derived name
	// is "hits" but the existing "hits" playlist must still be detected.
	check_case("hits", "Hits", "hits", true);
	// Neither spelling exists yet: still one candidate, named after the file
	// that will actually be imported.
	check_case(NULL, "fresh", "Fresh", false);

	library_close();
	puts("playlist dedup: OK");
	return 0;
}
