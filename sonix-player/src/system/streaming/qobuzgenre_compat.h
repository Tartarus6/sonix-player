#ifndef QOBUZGENRE_COMPAT_H
#define QOBUZGENRE_COMPAT_H

// The unified Qobuz backend has no genre browsing API. The ported genre grid
// still needs these three calls to build its list; the compatibility
// implementations report an empty catalogue so the page shows its empty state
// instead of failing to link. Adding a real backend later only requires
// replacing the implementations, not the UI.

#include <stdbool.h>
#include <stddef.h>

typedef struct {
	long id;
	char name[128];
	char slug[128];
} qobuz_genre_t;

int qobuz_genres(int offset, qobuz_genre_t *out, int max);
int qobuz_reggae_genre(qobuz_genre_t *out);
int qobuz_genre_new_releases(long genre_id, int offset, void *out, int max);

#endif /* QOBUZGENRE_COMPAT_H */
