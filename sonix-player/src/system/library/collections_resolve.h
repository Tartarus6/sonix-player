#ifndef COLLECTIONS_RESOLVE_H
#define COLLECTIONS_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>

#define COLLECTION_ALBUM_MAX 256
#define COLLECTION_KEY_MAX 16
#define COLLECTION_ARTIST_MAX 256
#define COLLECTION_FOLDER_MAX 1024
#define COLLECTION_PATH_MAX 1024

typedef struct {
	char album[COLLECTION_ALBUM_MAX];
	char album_key[COLLECTION_KEY_MAX];
	char artist[COLLECTION_ARTIST_MAX];
	char folder[COLLECTION_FOLDER_MAX];
	char first_path[COLLECTION_PATH_MAX];
} collection_ref_t;

typedef collection_ref_t collection_snapshot_t;

typedef bool (*collection_snapshot_cb)(const collection_snapshot_t *snapshot, void *user);

typedef struct {
	bool (*snapshot)(const char *value, collection_snapshot_t *out, void *user);
	int (*named)(const char *name, collection_snapshot_cb cb, void *user);
	int (*keyed)(const char *key, collection_snapshot_cb cb, void *user);
	bool (*value_for_path)(const char *path, char *out, size_t out_size, void *user);
	void (*fold)(const char *in, char *out, size_t size);
} collection_lookup_t;

typedef enum {
	COLLECTION_RESOLVE_EXACT = 0,
	COLLECTION_RESOLVE_KEY,
	COLLECTION_RESOLVE_PATH,
	COLLECTION_RESOLVE_NAME_ARTIST,
	COLLECTION_RESOLVE_NAME_FOLDER,
	COLLECTION_RESOLVE_MISSING,
} collection_resolve_step_t;

collection_resolve_step_t collection_resolve(const collection_ref_t *ref, const collection_lookup_t *lookup, void *user, collection_snapshot_t *out);

#endif
