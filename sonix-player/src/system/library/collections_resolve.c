#include "collections_resolve.h"

#include "library.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	const collection_ref_t *ref;
	const collection_lookup_t *lookup;
	int matches;
	collection_snapshot_t match;
} key_candidates_t;

typedef struct {
	const collection_lookup_t *lookup;
	const char *folded;
	bool by_artist;
	int matches;
	collection_snapshot_t match;
} named_candidates_t;

static void copy_match(collection_snapshot_t *out, const collection_snapshot_t *match) {
	if (out)
		*out = *match;
}

static bool folded_equal(const char *a, const char *b, size_t size, const collection_lookup_t *lookup) {
	char folded_a[COLLECTION_FOLDER_MAX];
	char folded_b[COLLECTION_FOLDER_MAX];

	lookup->fold(a, folded_a, size);
	lookup->fold(b, folded_b, size);
	return strcmp(folded_a, folded_b) == 0;
}

static bool corroborated(const collection_ref_t *ref, const collection_snapshot_t *candidate, const collection_lookup_t *lookup) {
	if (ref->artist[0] && candidate->artist[0] && folded_equal(ref->artist, candidate->artist, COLLECTION_ARTIST_MAX, lookup))
		return true;
	if (ref->folder[0] && candidate->folder[0] && folded_equal(ref->folder, candidate->folder, COLLECTION_FOLDER_MAX, lookup))
		return true;
	if (ref->artist[0] || ref->folder[0] || candidate->artist[0] || candidate->folder[0])
		return false;
	return folded_equal(ref->album, candidate->album, COLLECTION_ALBUM_MAX, lookup);
}

static bool key_candidate(const collection_snapshot_t *snapshot, void *user) {
	key_candidates_t *candidates = user;

	if (folded_equal(candidates->ref->album, snapshot->album, COLLECTION_ALBUM_MAX, candidates->lookup) && corroborated(candidates->ref, snapshot, candidates->lookup)) {
		candidates->matches++;
		if (candidates->matches == 1)
			candidates->match = *snapshot;
	}
	return true;
}

static bool named_candidate(const collection_snapshot_t *snapshot, void *user) {
	named_candidates_t *candidates = user;
	const char *text = candidates->by_artist ? snapshot->artist : snapshot->folder;
	size_t size = candidates->by_artist ? COLLECTION_ARTIST_MAX : COLLECTION_FOLDER_MAX;
	char folded[COLLECTION_FOLDER_MAX];

	candidates->lookup->fold(text, folded, size);
	if (strcmp(candidates->folded, folded) == 0) {
		candidates->matches++;
		if (candidates->matches == 1)
			candidates->match = *snapshot;
	}
	return true;
}

static collection_resolve_step_t resolve_named(const collection_ref_t *ref, const collection_lookup_t *lookup, bool by_artist, collection_snapshot_t *out) {
	named_candidates_t candidates = {
		.lookup = lookup,
		.by_artist = by_artist,
	};
	char folded[COLLECTION_FOLDER_MAX];
	const char *text = by_artist ? ref->artist : ref->folder;
	size_t size = by_artist ? COLLECTION_ARTIST_MAX : COLLECTION_FOLDER_MAX;

	lookup->fold(text, folded, size);
	candidates.folded = folded;
	lookup->named(ref->album, named_candidate, &candidates);
	if (candidates.matches != 1)
		return COLLECTION_RESOLVE_MISSING;
	copy_match(out, &candidates.match);
	return by_artist ? COLLECTION_RESOLVE_NAME_ARTIST : COLLECTION_RESOLVE_NAME_FOLDER;
}

collection_resolve_step_t collection_resolve(const collection_ref_t *ref, const collection_lookup_t *lookup, void *user, collection_snapshot_t *out) {
	char value[320];
	collection_snapshot_t snapshot;

	if (!ref || !lookup || !lookup->snapshot || !lookup->named || !lookup->keyed || !lookup->value_for_path || !lookup->fold || !ref->album[0] || !ref->album_key[0])
		return COLLECTION_RESOLVE_MISSING;

	snprintf(value, sizeof(value), "%s%c%s", ref->album, LIBRARY_ALBUM_KEY_SEP, ref->album_key);
	if (lookup->snapshot(value, &snapshot, user)) {
		copy_match(out, &snapshot);
		return COLLECTION_RESOLVE_EXACT;
	}

	{
		key_candidates_t candidates = {
			.ref = ref,
			.lookup = lookup,
		};

		lookup->keyed(ref->album_key, key_candidate, &candidates);
		if (candidates.matches == 1) {
			copy_match(out, &candidates.match);
			return COLLECTION_RESOLVE_KEY;
		}
	}

	if (ref->first_path[0] && lookup->value_for_path(ref->first_path, value, sizeof(value), user) && lookup->snapshot(value, &snapshot, user) && corroborated(ref, &snapshot, lookup)) {
		copy_match(out, &snapshot);
		return COLLECTION_RESOLVE_PATH;
	}

	if (ref->artist[0]) {
		collection_resolve_step_t step = resolve_named(ref, lookup, true, out);

		if (step != COLLECTION_RESOLVE_MISSING)
			return step;
	}
	if (ref->folder[0]) {
		collection_resolve_step_t step = resolve_named(ref, lookup, false, out);

		if (step != COLLECTION_RESOLVE_MISSING)
			return step;
	}
	return COLLECTION_RESOLVE_MISSING;
}
