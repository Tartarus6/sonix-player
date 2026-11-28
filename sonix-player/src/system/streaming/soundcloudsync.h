#ifndef SOUNDCLOUDSYNC_H
#define SOUNDCLOUDSYNC_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/soundcloud.h"

// What the player writes back to the SoundCloud account, off the UI thread.
//
// The star is one tap. For a SoundCloud track it has to land on the account:
// the stored path would be a cache file that is gone twenty tracks later, and
// no other device of the user's would ever see the like.
//
// An HTTP request takes seconds and the UI thread cannot stall, so this holds
// one worker with a single-slot queue, and every job ends in a callback. The
// callback runs on the worker, not the UI thread; the receiver must bounce it
// itself with gui_post(), as the rest of the program does.
//
// The star also cannot wait on the network to know how to draw itself. A
// mirror of the liked track URNs is kept in memory, filled once in the
// background and updated on every tap. Until it exists the star draws empty.
// SoundCloud ids are URNs ("soundcloud:tracks:12345"), so they pass through
// here as strings.

// --- the favourites mirror -------------------------------------------------

bool soundcloudsync_favorites_known(void);
bool soundcloudsync_is_favorite(const char *id);
void soundcloudsync_forget(void);
void soundcloudsync_favorites_refresh(void (*done)(void));

// --- the jobs --------------------------------------------------------------

typedef void (*soundcloudsync_done_cb)(bool ok, const char *error, void *user);

// Likes or unlikes the track on the account. The mirror is updated before the
// network so the star answers the tap at once, and put back if the request
// fails.
void soundcloudsync_favorite_toggle(const char *id, bool want, soundcloudsync_done_cb done, void *user);

#endif /* SOUNDCLOUDSYNC_H */
