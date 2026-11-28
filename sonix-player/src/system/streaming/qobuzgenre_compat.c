#include "qobuzgenre_compat.h"

int qobuz_genres(int offset, qobuz_genre_t *out, int max) {
	(void)offset; (void)out; (void)max;
	return 0;
}

int qobuz_reggae_genre(qobuz_genre_t *out) {
	(void)out;
	return 0;
}

int qobuz_genre_new_releases(long genre_id, int offset, void *out, int max) {
	(void)genre_id; (void)offset; (void)out; (void)max;
	return 0;
}
