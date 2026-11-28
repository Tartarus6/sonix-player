#ifndef SOUNDCLOUDCACHE_H
#define SOUNDCLOUDCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/soundcloud.h"

// SoundCloud tracks on the card, played like any local file.
//
// The reasons are the same as for Qobuz: the decoders open paths, a seek needs
// to go backwards, and the queue, the equalizer and the progress bar all work
// once the bytes are in a file. SoundCloud answers a progressive MP3 request
// with a redirect to its CDN; the OAuth token goes on the first hop only and
// the download that follows carries no account credentials.
//
// Playback starts once the prebuffer in streamturn.h has arrived. The file is
// written to its final name, because a rename mid-download would swap the file
// under the decoder. An empty marker beside it records completeness: a file
// with a marker is a half-download and is fetched again.

#define SOUNDCLOUDCACHE_DIR ".local/soundcloud-cache"

// A gigabyte is several days of listening at 128 kbit/s, and still small enough
// not to quietly take over a 32 GB card with files the user never put there.
#define SOUNDCLOUDCACHE_MAX_BYTES (1024L * 1024L * 1024L)

// Where the card is. NULL or empty disables the cache. Called at startup and
// on every card change.
void soundcloudcache_set_root(const char *sd_root);
bool soundcloudcache_ready(void);

// The path this id would have, whether or not the file is there. The id is a
// URN ("soundcloud:tracks:12345"); its last component becomes the file name.
void soundcloudcache_path(const char *id, char *out, size_t size);

// True only for a complete, non-empty download. The path is copied to `out`
// when that is wanted.
bool soundcloudcache_has(const char *id, char *out, size_t size);

// Downloads `stream->url` into the cache and puts the local path in `out`.
//
// Returns once there is enough for the decoder to open the file; the rest
// arrives on a worker. Blocks for the prebuffer, so call from a worker thread.
// `duration_secs` is the API's length in seconds, used to work out how much
// must be in hand before playback starts; 0 means use the fixed prebuffer.
bool soundcloudcache_start(const char *id, const soundcloud_stream_t *stream, int duration_secs, char *out,
						   size_t size);

// Removes every file, including interrupted downloads and their markers.
// False when there is no cache directory or it could not be opened.
bool soundcloudcache_clear(void);

// True when `path` is a file in this cache. The player asks this of the file
// it is about to play to decide which service owns it, as it does for
// podcastcache_is_episode() and tidalcache_track_id().
bool soundcloudcache_is_track(const char *path);

// Why the last soundcloudcache_start() failed, as a sentence fit to display.
// Empty on success. Per thread, like http_last_error().
const char *soundcloudcache_last_error(void);

#endif /* SOUNDCLOUDCACHE_H */
