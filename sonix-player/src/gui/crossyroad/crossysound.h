#ifndef GUI_CROSSYSOUND_H
#define GUI_CROSSYSOUND_H

#include <stdbool.h>

// Crossy Road's sound effects. There are no files: the three sounds are
// synthesised when the mixer starts, with the same envelopes as the web
// prototype (a rising chirp for a hop, a falling triangle for the splash and a
// falling sawtooth for the crash).
//
// A thread mixes whatever is sounding into the external PCM (audio.h), writing
// silence in between, from crossysound_start() to crossysound_stop(). Music and
// radio are stopped first: the PCM takes a single writer.

typedef enum {
	CROSSY_SFX_HOP,
	CROSSY_SFX_SPLASH,
	CROSSY_SFX_CRASH,
	CROSSY_SFX_COIN,
	CROSSY_SFX_COUNT,
} crossy_sfx_t;

// Starts the mixer thread and returns at once. False only when the thread
// cannot be made. When there is no PCM the game runs silently and
// crossysound_play() does nothing.
bool crossysound_start(void);

// Starts `sfx` from its beginning. Any thread.
void crossysound_play(crossy_sfx_t sfx);

// Stops the mixer, closes the PCM and frees the sounds.
void crossysound_stop(void);

#endif /* GUI_CROSSYSOUND_H */
