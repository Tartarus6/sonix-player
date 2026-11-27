#include "../src/system/library/collections_resolve.h"
#include "../src/system/library/library.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define FAKE_SNAPSHOT_MAX 16
#define FAKE_PATH_MAX 16

typedef struct {
	char path[COLLECTION_PATH_MAX];
	char value[LIBRARY_ALBUM_VALUE_MAX];
} fake_path_t;

typedef struct {
	collection_snapshot_t snapshots[FAKE_SNAPSHOT_MAX];
	int snapshot_count;
	fake_path_t paths[FAKE_PATH_MAX];
	int path_count;
} fake_lookup_t;

static fake_lookup_t *active_fake;

static collection_ref_t make_ref(const char *album, const char *key, const char *artist, const char *folder, const char *first_path) {
	collection_ref_t ref = {0};

	snprintf(ref.album, sizeof(ref.album), "%s", album);
	snprintf(ref.album_key, sizeof(ref.album_key), "%s", key);
	snprintf(ref.artist, sizeof(ref.artist), "%s", artist);
	snprintf(ref.folder, sizeof(ref.folder), "%s", folder);
	snprintf(ref.first_path, sizeof(ref.first_path), "%s", first_path);
	return ref;
}

static void fake_add(fake_lookup_t *fake, const char *album, const char *key, const char *artist, const char *folder, const char *first_path) {
	assert(fake->snapshot_count < FAKE_SNAPSHOT_MAX);
	fake->snapshots[fake->snapshot_count++] = make_ref(album, key, artist, folder, first_path);
}

static void fake_add_path(fake_lookup_t *fake, const char *path, const char *album, const char *key) {
	fake_path_t *item;

	assert(fake->path_count < FAKE_PATH_MAX);
	item = &fake->paths[fake->path_count++];
	snprintf(item->path, sizeof(item->path), "%s", path);
	snprintf(item->value, sizeof(item->value), "%s%c%s", album, LIBRARY_ALBUM_KEY_SEP, key);
}

static void test_fold(const char *in, char *out, size_t size) {
	size_t used = 0;

	if (size == 0)
		return;
	while (*in && used + 1 < size) {
		unsigned char c = (unsigned char)*in;
		char folded = 0;

		if (c == 0xc3 && in[1]) {
			switch ((unsigned char)in[1]) {
			case 0xa1: // á
				folded = 'a';
				break;
			case 0xa9: // é
				folded = 'e';
				break;
			case 0xad: // í
				folded = 'i';
				break;
			case 0xb3: // ó
				folded = 'o';
				break;
			case 0xba: // ú
				folded = 'u';
				break;
			case 0xbc: // ü
				folded = 'u';
				break;
			case 0xb1: // ñ
				folded = 'n';
				break;
			default:
				break;
			}
		}
		if (folded) {
			out[used++] = folded;
			in += 2;
		} else {
			out[used++] = (char)tolower(c);
			in++;
		}
	}
	out[used] = '\0';
}

static bool fake_snapshot(const char *value, collection_snapshot_t *out, void *user) {
	fake_lookup_t *fake = user;
	char candidate[LIBRARY_ALBUM_VALUE_MAX];
	int i;

	for (i = 0; i < fake->snapshot_count; i++) {
		collection_snapshot_t *snapshot = &fake->snapshots[i];

		snprintf(candidate, sizeof(candidate), "%s%c%s", snapshot->album, LIBRARY_ALBUM_KEY_SEP, snapshot->album_key);
		if (strcmp(candidate, value) == 0) {
			*out = *snapshot;
			return true;
		}
	}
	return false;
}

static int fake_named(const char *name, collection_snapshot_cb cb, void *user) {
	char folded_name[COLLECTION_ALBUM_MAX];
	int count = 0;
	int i;

	test_fold(name, folded_name, sizeof(folded_name));
	for (i = 0; i < active_fake->snapshot_count; i++) {
		char folded_album[COLLECTION_ALBUM_MAX];

		test_fold(active_fake->snapshots[i].album, folded_album, sizeof(folded_album));
		if (strcmp(folded_name, folded_album) == 0) {
			count++;
			if (!cb(&active_fake->snapshots[i], user))
				break;
		}
	}
	return count;
}

static int fake_keyed(const char *key, collection_snapshot_cb cb, void *user) {
	int count = 0;
	int i;

	for (i = 0; i < active_fake->snapshot_count; i++) {
		if (strcmp(active_fake->snapshots[i].album_key, key) == 0) {
			count++;
			if (!cb(&active_fake->snapshots[i], user))
				break;
		}
	}
	return count;
}

static bool fake_value_for_path(const char *path, char *out, size_t out_size, void *user) {
	fake_lookup_t *fake = user;
	int i;

	for (i = 0; i < fake->path_count; i++) {
		if (strcmp(fake->paths[i].path, path) == 0) {
			snprintf(out, out_size, "%s", fake->paths[i].value);
			return true;
		}
	}
	return false;
}

static bool resolution_is(const collection_ref_t *ref, fake_lookup_t *fake, collection_resolve_step_t expected, const char *expected_album) {
	collection_lookup_t lookup = {
		.snapshot = fake_snapshot,
		.named = fake_named,
		.keyed = fake_keyed,
		.value_for_path = fake_value_for_path,
		.fold = test_fold,
	};
	collection_snapshot_t out;
	collection_snapshot_t untouched;
	collection_resolve_step_t step;

	memset(&out, 0xa5, sizeof(out));
	untouched = out;
	active_fake = fake;
	step = collection_resolve(ref, &lookup, fake, &out);
	if (step != expected)
		return false;
	if (expected == COLLECTION_RESOLVE_MISSING)
		return memcmp(&out, &untouched, sizeof(out)) == 0;
	return strcmp(out.album, expected_album) == 0;
}

static void check(bool condition, const char *name, int line, int *checks, int *failures) {
	(*checks)++;
	if (!condition) {
		fprintf(stderr, "not ok %d - %s (line %d)\n", *checks, name, line);
		(*failures)++;
	} else {
		printf("ok %d - %s\n", *checks, name);
	}
}

#define CHECK(condition, name) check((condition), (name), __LINE__, &checks, &failures)

int main(void) {
	int checks = 0;
	int failures = 0;

	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Album", "a1b2c3d4", "Artist", "/music/album", "/music/album/01.flac");
		fake_add(&fake, "Album", "a1b2c3d4", "Artist", "/music/album", "/music/album/01.flac");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_EXACT, "Album"), "exact value resolves");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Café", "a1b2c3d4", "Artist", "/music/old", "");
		fake_add(&fake, "CAFE", "a1b2c3d4", "artist", "/music/new", "/music/new/01.flac");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_KEY, "CAFE"), "same key, one candidate, same folded album name, matching artist resolves");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Old title", "a1b2c3d4", "Artist", "/music/old", "");
		fake_add(&fake, "old title", "a1b2c3d4", "Someone else", "/music/new", "/music/new/01.flac");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "same key, one candidate, no corroboration is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Deleted Album", "a1b2c3d4", "Artist", "/music/deleted", "");
		fake_add(&fake, "Other Album", "a1b2c3d4", "artist", "/music/other", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "same key, one candidate, different album name, matching artist is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Stored Album", "a1b2c3d4", "Artist", "/music/old", "");
		fake_add(&fake, "Other Album", "a1b2c3d4", "artist", "/music/other", "");
		fake_add(&fake, "stored album", "a1b2c3d4", "ARTIST", "/music/new", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_KEY, "stored album"), "same key, two candidates, one with the stored folded name and corroboration resolves");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Before", "oldkey", "Artist", "/music/before", "/music/after/01.flac");
		fake_add(&fake, "After", "newkey", "artist", "/music/after", "/music/after/01.flac");
		fake_add_path(&fake, "/music/after/01.flac", "After", "newkey");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_PATH, "After"), "first path resolves a renamed album with the same artist");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Before", "oldkey", "Old artist", "/music/shared", "/music/shared/01.flac");
		fake_add(&fake, "After", "newkey", "New artist", "/MUSIC/SHARED", "/music/shared/01.flac");
		fake_add_path(&fake, "/music/shared/01.flac", "After", "newkey");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_PATH, "After"), "first path resolves a moved album with the same folder");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Before", "oldkey", "Artist", "/music/before", "/music/unrelated/01.flac");
		fake_add(&fake, "Unrelated", "otherkey", "Other artist", "/music/unrelated", "/music/unrelated/01.flac");
		fake_add_path(&fake, "/music/unrelated/01.flac", "Unrelated", "otherkey");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "first path to an unrelated album is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Album", "oldkey", "Beyoncé", "", "");
		fake_add(&fake, "album", "newkey", "BEYONCE", "/music/album", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_NAME_ARTIST, "album"), "name plus folded artist resolves");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Greatest Hits", "oldkey", "Shared Artist", "", "");
		fake_add(&fake, "Greatest Hits", "newkey1", "shared artist", "/music/one", "");
		fake_add(&fake, "Greatest Hits", "newkey2", "SHARED ARTIST", "/music/two", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "name plus artist with two matches is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Greatest Hits", "oldkey", "", "/music/shared", "");
		fake_add(&fake, "Greatest Hits", "newkey1", "Artist one", "/MUSIC/SHARED", "");
		fake_add(&fake, "Greatest Hits", "newkey2", "Artist two", "/music/other", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_NAME_FOLDER, "Greatest Hits"), "name plus folder resolves uniquely");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Greatest Hits", "oldkey", "Stored Artist", "", "");
		fake_add(&fake, "Greatest Hits", "newkey", "Different Artist", "/music/other", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "a single same-named album with a different artist is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Café", "oldkey", "Beyoncé", "", "");
		fake_add(&fake, "Cafe", "newkey", "Beyonce", "/music/cafe", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_NAME_ARTIST, "Cafe"), "accent-folded name and artist resolve");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Album", "", "Artist", "/music/album", "");
		fake_add(&fake, "Album", "newkey", "Artist", "/music/album", "");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_MISSING, NULL), "an item with no key is missing");
	}
	{
		fake_lookup_t fake = {0};
		collection_ref_t ref = make_ref("Greatest Hits", "exactkey", "Artist", "/music/exact", "/music/exact/01.flac");
		fake_add(&fake, "Greatest Hits", "exactkey", "Artist", "/music/exact", "/music/exact/01.flac");
		fake_add(&fake, "Greatest Hits", "otherkey", "Artist", "/music/exact", "/music/exact/01.flac");
		fake_add(&fake, "Other album", "exactkey", "Artist", "/music/exact", "/music/exact/01.flac");
		fake_add_path(&fake, "/music/exact/01.flac", "Other album", "exactkey");
		CHECK(resolution_is(&ref, &fake, COLLECTION_RESOLVE_EXACT, "Greatest Hits"), "exact wins over every fallback");
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
