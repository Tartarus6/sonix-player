#include "collections.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// How many collection items are read and resolved at a time when the whole
// collection is walked, and how many rows of one album's track list are read
// at a time. Both keep the walk's memory bounded on a card of any size.
#define COLLECTIONS_PLAY_PAGE 32
#define COLLECTIONS_PLAY_WINDOW 256

// ---------------------------------------------------------------------------
// Name rules
//
// The same rules the playlists page applies, checked again here so nothing but
// the GUI decides what the database holds: not empty, at most 200 bytes, not
// starting with a dot, and none of the characters a FAT card or a Windows
// export would refuse. Trailing spaces are the GUI's to trim; the system only
// refuses what it must.
// ---------------------------------------------------------------------------

static bool name_ok(const char *name) {
	if (!name || !name[0] || name[0] == '.') {
		return false;
	}
	if (strlen(name) > COLLECTION_NAME_MAX) {
		return false;
	}
	return strpbrk(name, "/\\:*?\"<>|") == NULL;
}

// ---------------------------------------------------------------------------
// The lookup adapter
//
// collection_resolve() asks its questions through collection_lookup_t; these
// callbacks answer them out of the Task 3 library primitives. A snapshot's
// title is the album's name and its key the album's key, so a library snapshot
// maps onto a resolver snapshot one field at a time.
//
// The two candidate walks run with db_lock held: their callbacks only copy and
// fold. library_fold_text() is pure, so the resolver may call it from inside a
// candidate callback; every other library call here happens outside one.
// ---------------------------------------------------------------------------

static void snapshot_from_library(const library_album_snapshot_t *in, collection_snapshot_t *out) {
	memset(out, 0, sizeof(*out));
	snprintf(out->album, sizeof(out->album), "%s", in->title);
	snprintf(out->album_key, sizeof(out->album_key), "%s", in->key);
	snprintf(out->artist, sizeof(out->artist), "%s", in->artist);
	snprintf(out->folder, sizeof(out->folder), "%s", in->folder);
	snprintf(out->first_path, sizeof(out->first_path), "%s", in->first_path);
}

static bool lookup_snapshot(const char *value, collection_snapshot_t *out, void *user) {
	library_album_snapshot_t snapshot;

	(void)user;
	if (!library_album_snapshot(value, &snapshot)) {
		return false;
	}
	snapshot_from_library(&snapshot, out);
	return true;
}

// One album on its way from the library's candidate walk to the resolver's.
typedef struct {
	collection_snapshot_cb cb;
	void *user;
} lookup_forward_t;

static bool lookup_candidate(const library_album_snapshot_t *snapshot, void *user) {
	lookup_forward_t *forward = user;
	collection_snapshot_t out;

	snapshot_from_library(snapshot, &out);
	return forward->cb(&out, forward->user);
}

static int lookup_named(const char *name, collection_snapshot_cb cb, void *user) {
	lookup_forward_t forward = {.cb = cb, .user = user};

	return library_album_values_named(name, lookup_candidate, &forward);
}

static int lookup_keyed(const char *key, collection_snapshot_cb cb, void *user) {
	lookup_forward_t forward = {.cb = cb, .user = user};

	return library_album_values_with_key(key, lookup_candidate, &forward);
}

static bool lookup_value_for_path(const char *path, char *out, size_t out_size, void *user) {
	(void)user;
	return library_track_album_value(path, out, out_size);
}

static const collection_lookup_t lookup = {
	.snapshot = lookup_snapshot,
	.named = lookup_named,
	.keyed = lookup_keyed,
	.value_for_path = lookup_value_for_path,
	.fold = library_fold_text,
};

static void value_build(const collection_snapshot_t *snapshot, char *out, size_t size) { snprintf(out, size, "%s%c%s", snapshot->album, LIBRARY_ALBUM_KEY_SEP, snapshot->album_key); }

static void ref_from_item(const library_collection_item_t *item, collection_ref_t *ref) {
	memset(ref, 0, sizeof(*ref));
	snprintf(ref->album, sizeof(ref->album), "%s", item->album);
	snprintf(ref->album_key, sizeof(ref->album_key), "%s", item->album_key);
	snprintf(ref->artist, sizeof(ref->artist), "%s", item->artist);
	snprintf(ref->folder, sizeof(ref->folder), "%s", item->folder);
	snprintf(ref->first_path, sizeof(ref->first_path), "%s", item->first_path);
}

// The stored item a resolved album writes back over its row.
static void item_from_snapshot(const collection_snapshot_t *snapshot, int idx, library_collection_item_t *item) {
	memset(item, 0, sizeof(*item));
	item->idx = idx;
	snprintf(item->album, sizeof(item->album), "%s", snapshot->album);
	snprintf(item->album_key, sizeof(item->album_key), "%s", snapshot->album_key);
	snprintf(item->artist, sizeof(item->artist), "%s", snapshot->artist);
	snprintf(item->folder, sizeof(item->folder), "%s", snapshot->folder);
	snprintf(item->first_path, sizeof(item->first_path), "%s", snapshot->first_path);
}

// ---------------------------------------------------------------------------
// Listing and naming
// ---------------------------------------------------------------------------

int collections_list(collection_name_cb cb, void *user) { return library_collection_list(cb, user); }

bool collections_exists(const char *name, int *id_out) {
	if (!name_ok(name)) {
		fprintf(stderr, "collections: unusable name looked up\n");
		return false;
	}
	return library_collection_exists(name, id_out);
}

int collections_create(const char *name) {
	if (!name_ok(name)) {
		fprintf(stderr, "collections: unusable name not created\n");
		return -1;
	}
	int id = library_collection_create(name);
	if (id < 0) {
		fprintf(stderr, "collections: the database would not create \"%s\"\n", name);
	}
	return id;
}

bool collections_rename(int id, const char *name) {
	if (id <= 0) {
		fprintf(stderr, "collections: no collection %d to rename\n", id);
		return false;
	}
	if (!name_ok(name)) {
		fprintf(stderr, "collections: unusable rename for collection %d\n", id);
		return false;
	}
	if (!library_collection_rename(id, name)) {
		fprintf(stderr, "collections: the database would not rename collection %d to \"%s\"\n", id, name);
		return false;
	}
	return true;
}

bool collections_delete(int id) {
	if (id <= 0) {
		fprintf(stderr, "collections: no collection %d to delete\n", id);
		return false;
	}
	if (!library_collection_delete(id)) {
		fprintf(stderr, "collections: the database would not delete collection %d\n", id);
		return false;
	}
	return true;
}

int collections_album_count(int id) { return library_collection_count(id); }

bool collections_remove_album(int id, int position) {
	if (id <= 0 || position < 0) {
		fprintf(stderr, "collections: bad removal (collection %d, position %d)\n", id, position);
		return false;
	}
	int positions[1] = {position};
	if (library_collection_remove(id, positions, 1) <= 0) {
		fprintf(stderr, "collections: album %d not removed from collection %d\n", position, id);
		return false;
	}
	return true;
}

unsigned collections_revision(void) { return library_collections_revision(); }

// ---------------------------------------------------------------------------
// One page of a collection's albums
//
// The raw rows come out in one read, then each is resolved with the lock back,
// because resolving asks the library for album snapshots. A row a fallback
// step resolved is written back over its stored album, so the next visit takes
// the exact step again; the writer is single-writer, and the page holds no
// update open across rows.
// ---------------------------------------------------------------------------

// How one raw row is filled into the page's answer. `*merged` is set when the
// self-heal's update merged the row away.
static void row_fill(int id, const library_collection_item_t *item, collection_album_t *out, library_collection_item_t *healed, bool *merged) {
	collection_ref_t ref;
	collection_snapshot_t snapshot;

	ref_from_item(item, &ref);
	collection_resolve_step_t step = collection_resolve(&ref, &lookup, NULL, &snapshot);

	memset(out, 0, sizeof(*out));
	out->idx = item->idx;
	if (step == COLLECTION_RESOLVE_MISSING) {
		// The honest answer: the stored name and artist, and a cover only
		// from a file that is still there. Never deleted, never guessed at.
		out->missing = true;
		snprintf(out->name, sizeof(out->name), "%s", item->album);
		snprintf(out->artist, sizeof(out->artist), "%s", item->artist);
		if (item->first_path[0] && access(item->first_path, R_OK) == 0) {
			snprintf(out->path, sizeof(out->path), "%s", item->first_path);
		}
		return;
	}

	out->missing = false;
	snprintf(out->name, sizeof(out->name), "%s", snapshot.album);
	snprintf(out->artist, sizeof(out->artist), "%s", snapshot.artist);
	snprintf(out->path, sizeof(out->path), "%s", snapshot.first_path);
	value_build(&snapshot, out->value, sizeof(out->value));
	if (step == COLLECTION_RESOLVE_EXACT) {
		return;
	}

	item_from_snapshot(&snapshot, item->idx, healed);
	library_collection_update_result_t result = library_collection_update_item(id, item->idx, healed);
	if (result == LIBRARY_COLLECTION_ITEM_FAILED) {
		fprintf(stderr, "collections: self-heal of item %d failed; shown resolved for this visit\n", item->idx);
	} else if (result == LIBRARY_COLLECTION_ITEM_MERGED) {
		fprintf(stderr, "collections: item %d merged into an earlier position of collection %d\n", item->idx, id);
		*merged = true;
	}
}

static int page_fill(int id, int offset, int count, collection_album_t *out, library_collection_item_t *rows, library_collection_item_t *healed) {
	int got = library_collection_page(id, offset, count, rows);
	if (got <= 0) {
		return got;
	}

	bool merged = false;
	for (int i = 0; i < got; i++) {
		row_fill(id, &rows[i], &out[i], healed, &merged);
	}
	// A merge deleted a row, so what this window names has shifted: read the
	// same window once more, for the answer to describe what is there now.
	// Once, whatever it finds: the page never loops on its own account.
	if (merged) {
		got = library_collection_page(id, offset, count, rows);
		merged = false;
		for (int i = 0; i < got; i++) {
			row_fill(id, &rows[i], &out[i], healed, &merged);
		}
	}
	return got;
}

int collections_albums_page(int id, int offset, int count, collection_album_t *out) {
	if (id <= 0 || !out || offset < 0 || count <= 0) {
		fprintf(stderr, "collections: bad albums page asked (collection %d, offset %d, count %d)\n", id, offset, count);
		return 0;
	}

	// A page is some 2.7 KB a row, which is why it is the caller's array and
	// why the raw rows read into it are on the heap too -- with one more for
	// the item a self-heal writes back with.
	library_collection_item_t *rows = malloc(((size_t)count + 1) * sizeof(*rows));
	if (!rows) {
		fprintf(stderr, "collections: out of memory for a page of %d albums\n", count);
		return 0;
	}

	int got = page_fill(id, offset, count, out, rows, &rows[count]);
	free(rows);
	return got;
}

// ---------------------------------------------------------------------------
// Adding albums
//
// The whole selection is one transaction with one item in memory: the writer
// owns a savepoint, each album is snapshotted and offered a row at a time, and
// write_end() commits the lot or rolls the lot back. A name-only value or one
// the index does not hold is skipped, not guessed at, and a value the
// collection already holds is skipped by the unique key, not an error.
// ---------------------------------------------------------------------------

int collections_add_albums(int id, const char *const *values, int count, int *added, int *skipped) {
	if (added) {
		*added = 0;
	}
	if (skipped) {
		*skipped = 0;
	}
	if (id <= 0 || !values || count <= 0) {
		fprintf(stderr, "collections: bad add asked (collection %d, count %d)\n", id, count);
		if (skipped) {
			*skipped = count > 0 ? count : 0;
		}
		return -1;
	}

	library_collection_writer_t *writer = library_collection_write_begin(id);
	if (!writer) {
		fprintf(stderr, "collections: cannot start writing to collection %d\n", id);
		if (skipped) {
			*skipped = count;
		}
		return -1;
	}

	// One snapshot and one item, reused for the whole selection.
	library_album_snapshot_t *snapshot = malloc(sizeof(*snapshot));
	library_collection_item_t *item = malloc(sizeof(*item));
	if (!snapshot || !item) {
		fprintf(stderr, "collections: out of memory adding to collection %d\n", id);
		free(snapshot);
		free(item);
		library_collection_write_end(writer, false, NULL, NULL);
		if (skipped) {
			*skipped = count;
		}
		return -1;
	}

	// Values refused before a row was offered: a name alone, or one the index
	// does not hold. The writer never saw them, so they are counted here and
	// added to what write_end() reports, for added + skipped to reach count.
	int refused = 0;

	for (int i = 0; i < count; i++) {
		const char *value = values[i];
		// A keyed value is taken as is; a name alone could pick the wrong
		// same-named album, so it is refused.
		const char *sep = value ? strchr(value, LIBRARY_ALBUM_KEY_SEP) : NULL;
		if (!value || !value[0] || !sep || sep == value) {
			fprintf(stderr, "collections: value %d is a name alone, skipped\n", i);
			refused++;
			continue;
		}
		if (!library_album_snapshot(value, snapshot)) {
			fprintf(stderr, "collections: album of value %d not in the index, skipped\n", i);
			refused++;
			continue;
		}

		memset(item, 0, sizeof(*item));
		snprintf(item->album, sizeof(item->album), "%s", snapshot->title);
		snprintf(item->album_key, sizeof(item->album_key), "%s", snapshot->key);
		snprintf(item->artist, sizeof(item->artist), "%s", snapshot->artist);
		snprintf(item->folder, sizeof(item->folder), "%s", snapshot->folder);
		snprintf(item->first_path, sizeof(item->first_path), "%s", snapshot->first_path);
		// The writer hands out positions; false is a duplicate or a failed
		// writer, both of which write_end() accounts for.
		library_collection_write_row(writer, item);
	}
	free(snapshot);
	free(item);

	// keep asks for a commit; a writer that failed on the way rolls back
	// inside write_end() whatever this asks, and answers false.
	int writer_added = 0, writer_skipped = 0;
	bool committed = library_collection_write_end(writer, true, &writer_added, &writer_skipped);
	if (added) {
		*added = writer_added;
	}
	if (skipped) {
		*skipped = writer_skipped + refused;
	}
	if (!committed) {
		fprintf(stderr, "collections: adding %d albums to collection %d failed, nothing kept\n", count, id);
		if (added) {
			*added = 0;
		}
		if (skipped) {
			*skipped = count;
		}
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Playing a collection
//
// The paths of every album in order, walked a page of items at a time and read
// out of each album's track list in windows, so a large collection costs a
// bounded piece of memory and a bounded number of rows held. Missing albums
// are skipped; the walk answers nothing rather than a half list it cannot
// finish.
// ---------------------------------------------------------------------------

typedef struct {
	char **list;
	int count;
	int cap;
	bool failed;
} paths_ctx_t;

static bool paths_grow(paths_ctx_t *ctx) {
	if (ctx->count < ctx->cap) {
		return true;
	}
	int cap = ctx->cap ? ctx->cap * 2 : 64;
	char **grown = realloc(ctx->list, (size_t)cap * sizeof(*grown));
	if (!grown) {
		return false;
	}
	ctx->list = grown;
	ctx->cap = cap;
	return true;
}

static bool track_path_row(const char *name, const char *path, const char *artist, void *user) {
	paths_ctx_t *ctx = user;

	(void)name;
	(void)artist;
	if (!path || !path[0]) {
		return true;
	}
	if (ctx->count >= COLLECTION_TRACK_PATH_MAX) {
		return false; // the cap: stop the walk, keep what is held
	}
	if (!paths_grow(ctx)) {
		ctx->failed = true;
		return false;
	}
	char *copy = strdup(path);
	if (!copy) {
		ctx->failed = true;
		return false;
	}
	ctx->list[ctx->count++] = copy;
	return true;
}

void collections_paths_free(char **paths, int count) {
	if (!paths) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(paths[i]);
	}
	free(paths);
}

int collections_track_paths(int id, char ***paths_out) {
	if (paths_out) {
		*paths_out = NULL;
	}
	if (id <= 0 || !paths_out) {
		fprintf(stderr, "collections: bad track paths asked (collection %d)\n", id);
		return 0;
	}

	paths_ctx_t ctx = {0};
	library_collection_item_t *rows = malloc(COLLECTIONS_PLAY_PAGE * sizeof(*rows));
	collection_snapshot_t *snapshot = malloc(sizeof(*snapshot));
	if (!rows || !snapshot) {
		fprintf(stderr, "collections: out of memory walking collection %d\n", id);
		free(rows);
		free(snapshot);
		return 0;
	}

	int offset = 0;
	bool capped = false;
	while (!capped) {
		int got = library_collection_page(id, offset, COLLECTIONS_PLAY_PAGE, rows);
		if (got <= 0) {
			break;
		}
		for (int i = 0; i < got && !capped && !ctx.failed; i++) {
			collection_ref_t ref;

			ref_from_item(&rows[i], &ref);
			if (collection_resolve(&ref, &lookup, NULL, snapshot) == COLLECTION_RESOLVE_MISSING) {
				continue;
			}

			char value[LIBRARY_ALBUM_VALUE_MAX];
			value_build(snapshot, value, sizeof(value));
			library_index_t *ix = library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, value, LIBRARY_ORDER_DEFAULT, false);
			if (!ix) {
				fprintf(stderr, "collections: cannot open the tracks of \"%s\"\n", snapshot->album);
				continue;
			}
			int row_offset = 0;
			for (;;) {
				int delivered = library_index_window(ix, row_offset, COLLECTIONS_PLAY_WINDOW, track_path_row, &ctx);
				if (ctx.failed || delivered < COLLECTIONS_PLAY_WINDOW) {
					break;
				}
				row_offset += delivered;
			}
			library_index_close(ix);
			if (ctx.count >= COLLECTION_TRACK_PATH_MAX) {
				capped = true;
			}
		}
		if (got < COLLECTIONS_PLAY_PAGE) {
			break;
		}
		offset += got;
	}
	free(snapshot);
	free(rows);

	if (ctx.failed) {
		fprintf(stderr, "collections: out of memory collecting the tracks of collection %d\n", id);
		collections_paths_free(ctx.list, ctx.count);
		return 0;
	}
	*paths_out = ctx.list;
	return ctx.count;
}
