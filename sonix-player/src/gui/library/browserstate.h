#ifndef BROWSERSTATE_H
#define BROWSERSTATE_H

#include <stdbool.h>
#include <stddef.h>

// The browser keeps the scroll of each folder it walked down through, so it
// can put the parent back where it was left on the way up. One slot per depth,
// matching the browser's own in-memory stack.
#define BROWSER_STATE_STACK_MAX 12

// The browser's folder, scroll and ancestor scrolls are kept beside the
// settings file in a small binary of their own, not in the INI: a card path can
// be longer than a settings value (capped at 128 bytes) and can contain bytes
// an INI line cannot carry.
bool browser_state_save(const char *file, const char *folder, int scroll, const int *stack, int stack_len);
bool browser_state_load(const char *file, char *folder, size_t size, int *scroll, int *stack, int stack_max, int *stack_len);

// Only an existing directory inside the card is accepted, after realpath, so a
// ".." or a symlink cannot walk the browser off it. The answer is rebased onto
// the configured spelling of the root, which is what the browser navigates by.
bool browser_state_directory(const char *root, const char *folder, char *out, size_t size);

// Chooses the folder the browser opens: the saved one, then the local track's
// parent when the track still exists, then <root>/Music, then the root. The
// scroll is returned only for the saved folder. used_saved says which branch
// won, so a fallback never inherits the saved folder's ancestor scrolls.
bool browser_state_choose(const char *root, const char *saved, int saved_scroll, const char *track, char *out, size_t size, int *scroll, bool *used_saved);

#endif /* BROWSERSTATE_H */
