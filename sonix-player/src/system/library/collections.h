#ifndef COLLECTIONS_H
#define COLLECTIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "collections_resolve.h"
#include "library.h"

// ---------------------------------------------------------------------------
// Collections
//
// The public face of the album collections feature. A collection is a named
// shelf of whole albums; the rows live in SONIX_COLLECTION and
// SONIX_COLLECTION_ITEM, written through the library_collection_* primitives,
// and every album a row names is resolved through the cascade in
// collections_resolve.c each time it is shown, so a retag, a rescan or a
// folder move cannot silently attach a different album.
//
// Everything here is safe to call from a worker: the functions take db_lock
// internally, one at a time, and answer the empty answer when the card is not
// mounted (db == NULL) rather than failing or crashing.
//
// Sizes come from collections_resolve.h: an album name, its key, an artist, a
// folder and a path. LIBRARY_ALBUM_VALUE_MAX (library.h) is the room a whole
// album value takes.
// ---------------------------------------------------------------------------

#define COLLECTION_NAME_MAX 200

// One album of a collection as the detail page shows it. `idx` is the item's
// stable position; `name`, `artist` and `path` are the resolved album's, or the
// stored snapshots when it is missing; `value` is the album value to open, and
// is empty for a missing one, whose `path` is also empty unless the stored
// first path is still readable.
typedef struct {
	int idx;
	char name[COLLECTION_ALBUM_MAX];
	char artist[COLLECTION_ARTIST_MAX];
	char path[COLLECTION_PATH_MAX];
	char value[LIBRARY_ALBUM_VALUE_MAX];
	bool missing;
} collection_album_t;

// Every collection, in name order: its id, its name and how many albums it
// holds, missing ones included. The callback runs with the index locked: copy
// what you need and do not call back into the library. Return false to stop
// early. Returns how many collections were delivered.
typedef bool (*collection_name_cb)(int id, const char *name, int albums, void *user);
int collections_list(collection_name_cb cb, void *user);

// True when a collection by that name exists; `id_out` (may be NULL) gets its
// id.
bool collections_exists(const char *name, int *id_out);

// Creates an empty collection. Returns its id, or -1 when the name is unusable
// or the database would not take it.
int collections_create(const char *name);

// Renames a collection. Fails when another one already answers to the new
// name; renaming to the current name succeeds and does nothing.
bool collections_rename(int id, const char *name);

// Deletes a collection and its album rows. The albums stay in the library.
bool collections_delete(int id);

// How many albums a collection holds, missing ones included.
int collections_album_count(int id);

// Fills `out` with up to `count` albums from `offset`, in the order they were
// added. Returns how many were filled, which is fewer than asked at the end of
// the list and 0 when the collection is empty or unreadable.
//
// Each row is resolved through the cascade. A row a fallback step had to
// resolve is written back to the stored album (the self-heal), so the next
// visit takes the exact step again; when two rows resolved to the same album
// and one was merged away, the same window is read again once, because the
// count shrank under it. A missing row keeps its stored title and artist and
// is never deleted.
int collections_albums_page(int id, int offset, int count, collection_album_t *out);

// Adds albums to a collection, given as keyed album values (see
// LIBRARY_ALBUM_KEY_SEP). A value without the separator is a name alone, which
// could pick the wrong same-named album, and is skipped rather than guessed
// at; so is a value the index does not hold and one the collection already
// holds. The whole selection goes in as one transaction, one item in memory at
// a time, so a failure leaves no partial rows.
//
// Returns 0 when the transaction committed and -1 when it failed; `added` and
// `skipped` (either may be NULL) always receive the counts.
int collections_add_albums(int id, const char *const *values, int count, int *added, int *skipped);

// Removes the album at that position of the collection. True when it went.
bool collections_remove_album(int id, int position);

// The paths of every track of every album a collection holds, in album order,
// for playing the collection in one go. Missing albums are skipped, the list
// is capped (a collection that answers with more is cut off), and the whole
// answer is malloc'd: free it with collections_paths_free(). Returns how many
// paths there are, 0 with *paths_out NULL when there are none or the list
// could not be built.
#define COLLECTION_TRACK_PATH_MAX 20000
int collections_track_paths(int id, char ***paths_out);
void collections_paths_free(char **paths, int count);

// Bumped by every write, so an open page knows to re-read.
unsigned collections_revision(void);

#endif
