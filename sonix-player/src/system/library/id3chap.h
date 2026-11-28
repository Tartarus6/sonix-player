#ifndef ID3CHAP_H
#define ID3CHAP_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// The chapters inside an MP3
//
// An audiobook is not always an .m4b. A great many are one long .mp3 with ID3v2
// CHAP frames in it -- that is what every tool that splits an audiobook by
// chapter writes, and what Audible conversions come out as -- and to a player
// that only reads MP4 chapters those look like a four-hour track with no marks.
//
// This reads the CHAP frames and nothing else. It is deliberately not a general
// ID3 parser: metadata.c already reads the tags that name a track, and the one
// thing missing was the chapter list.
//
// It is fed files off a card, so every length in the file is treated as a claim
// and not as a fact: a frame that says it is bigger than what is left is the end
// of the parse, not a read past the buffer.
// ---------------------------------------------------------------------------

typedef struct {
	double start; // seconds from the beginning of the file
	char title[128];
} id3chap_t;

// Reads up to `max` chapters into `out`, in file order. Returns the number of
// chapters found (capped at 1024), even if fewer were stored. Pass NULL and 0
// to count chapters without storing them.
int id3chap_read(const char *path, id3chap_t *out, int max);

// True when the file has at least one chapter. Stops parsing at the first.
bool id3chap_present(const char *path);

// For a growing cache file: true when it has fewer than 10 bytes, or when
// a supported ID3v2.3/v2.4 header declares an in-budget payload past EOF.
// False for missing files, non-ID3 data, invalid headers and complete tags.
// This tests file length, not audio completeness or unwritten sparse regions.
bool id3chap_tag_pending(const char *path);

#endif /* ID3CHAP_H */
