#ifndef PODCASTNOTESTEXT_H
#define PODCASTNOTESTEXT_H

#include "podcast.h"

#define PODCAST_NOTES_MAX (32 * 1024)

// Convert bounded publisher markup to a bounded, heap-owned plain text note.
podcast_notes_result_t podcast_notes_to_text(const char *html, char **text);

#endif
