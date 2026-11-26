#include "browserstate.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The folder is fixed-width so the file has one shape and a short read is
// never mistaken for a short path. A card path that does not fit is simply
// not remembered; the browser then opens <card>/Music, and finally the root.
#define STATE_PATH_MAX 512

// V2 adds the ancestor scrolls. V1 is still read as an empty stack so a state
// file written by an earlier build does not make the browser start at zero.
typedef struct {
	char magic[8];
	int32_t scroll;
	int32_t stack_len;
	int32_t stack[BROWSER_STATE_STACK_MAX];
	char folder[STATE_PATH_MAX];
} browser_state_disk_t;

typedef struct {
	char magic[8];
	int32_t scroll;
	char folder[STATE_PATH_MAX];
} browser_state_disk_v1_t;

static const char STATE_MAGIC[8] = {'S', 'N', 'X', 'B', 'R', 'W', '2', '\0'};
static const char STATE_MAGIC_V1[8] = {'S', 'N', 'X', 'B', 'R', 'W', '1', '\0'};

bool browser_state_save(const char *file, const char *folder, int scroll, const int *stack, int stack_len) {
	if (!file || !file[0] || !folder || !folder[0] || strlen(folder) >= STATE_PATH_MAX || stack_len < 0 || stack_len > BROWSER_STATE_STACK_MAX || (stack_len > 0 && !stack)) {
		return false;
	}

	char temp[PATH_MAX];
	if (snprintf(temp, sizeof(temp), "%s.tmp", file) >= (int)sizeof(temp)) {
		return false;
	}

	browser_state_disk_t state;
	memset(&state, 0, sizeof(state));
	memcpy(state.magic, STATE_MAGIC, sizeof(state.magic));
	state.scroll = scroll < 0 ? 0 : scroll;
	state.stack_len = stack_len;
	for (int i = 0; i < stack_len; i++) {
		state.stack[i] = stack[i] < 0 ? 0 : stack[i];
	}
	memcpy(state.folder, folder, strlen(folder) + 1);

	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(&state, sizeof(state), 1, f) == 1 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && rename(temp, file) != 0) {
		ok = false;
	}
	if (!ok) {
		unlink(temp);
	}
	return ok;
}

bool browser_state_load(const char *file, char *folder, size_t size, int *scroll, int *stack, int stack_max, int *stack_len) {
	if (!file || !folder || !size || !scroll || !stack || !stack_len || stack_max < 0) {
		return false;
	}
	*stack_len = 0;

	FILE *f = fopen(file, "rb");
	if (!f) {
		return false;
	}
	unsigned char raw[sizeof(browser_state_disk_t)];
	size_t n = fread(raw, 1, sizeof(raw), f);
	bool extra = fgetc(f) != EOF;
	bool error = ferror(f) != 0;
	fclose(f);
	if (extra || error) {
		return false;
	}

	browser_state_disk_t state;
	if (n == sizeof(browser_state_disk_t) && memcmp(raw, STATE_MAGIC, sizeof(STATE_MAGIC)) == 0) {
		memcpy(&state, raw, sizeof(state));
	} else if (n == sizeof(browser_state_disk_v1_t) && memcmp(raw, STATE_MAGIC_V1, sizeof(STATE_MAGIC_V1)) == 0) {
		browser_state_disk_v1_t old;
		memcpy(&old, raw, sizeof(old));
		memset(&state, 0, sizeof(state));
		memcpy(state.magic, STATE_MAGIC, sizeof(state.magic));
		state.scroll = old.scroll;
		memcpy(state.folder, old.folder, sizeof(state.folder));
	} else {
		return false;
	}

	int count = state.stack_len;
	if (state.scroll < 0 || count < 0 || count > BROWSER_STATE_STACK_MAX || count > stack_max || !memchr(state.folder, '\0', sizeof(state.folder)) || !state.folder[0] || strlen(state.folder) >= size) {
		return false;
	}
	for (int i = 0; i < count; i++) {
		if (state.stack[i] < 0) {
			return false;
		}
	}

	memcpy(folder, state.folder, strlen(state.folder) + 1);
	*scroll = state.scroll;
	for (int i = 0; i < count; i++) {
		stack[i] = state.stack[i];
	}
	*stack_len = count;
	return true;
}

static bool inside(const char *root, const char *path) {
	size_t n = strlen(root);
	if (n == 0) {
		return false;
	}
	if (strcmp(root, "/") == 0) {
		return path[0] == '/';
	}
	return strncmp(root, path, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool browser_state_directory(const char *root, const char *folder, char *out, size_t size) {
	if (!root || !folder || !out || !size) {
		return false;
	}

	char real_root[PATH_MAX];
	char real_folder[PATH_MAX];
	struct stat st;
	if (!realpath(root, real_root) || !realpath(folder, real_folder) || !inside(real_root, real_folder) || stat(real_folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return false;
	}

	// Rebase the canonical suffix onto the configured spelling of the root, so
	// the browser can keep walking up to the mount point even when the mount
	// itself is a symlink. The suffix is empty when the folder is the root.
	const char *suffix = real_folder + strlen(real_root);
	if (real_root[0] == '/' && real_root[1] == '\0') {
		suffix = real_folder + 1;
	}
	if (suffix[0] == '/') {
		suffix++;
	}

	int n;
	if (suffix[0] == '\0') {
		n = snprintf(out, size, "%s", root);
	} else {
		size_t rlen = strlen(root);
		if (rlen > 0 && root[rlen - 1] == '/') {
			n = snprintf(out, size, "%s%s", root, suffix);
		} else {
			n = snprintf(out, size, "%s/%s", root, suffix);
		}
	}
	return n >= 0 && (size_t)n < size;
}

bool browser_state_choose(const char *root, const char *saved, int saved_scroll, const char *track, char *out, size_t size, int *scroll, bool *used_saved) {
	if (!root || !out || !size || !scroll) {
		return false;
	}

	*scroll = 0;
	if (used_saved) {
		*used_saved = false;
	}
	if (saved && browser_state_directory(root, saved, out, size)) {
		*scroll = saved_scroll > 0 ? saved_scroll : 0;
		if (used_saved) {
			*used_saved = true;
		}
		return true;
	}

	// The remembered track, when it is still there and its folder is on this
	// card. A path that has gone since is not a folder anyone was in.
	if (track && track[0] && strlen(track) < STATE_PATH_MAX) {
		char parent[STATE_PATH_MAX];
		memcpy(parent, track, strlen(track) + 1);
		char *slash = strrchr(parent, '/');
		struct stat st;
		if (slash && stat(track, &st) == 0 && S_ISREG(st.st_mode)) {
			if (slash == parent) {
				parent[1] = '\0';
			} else {
				*slash = '\0';
			}
			if (browser_state_directory(root, parent, out, size)) {
				return true;
			}
		}
	}

	// A card laid out the way the player expects, with no history at all.
	char music[STATE_PATH_MAX];
	if (snprintf(music, sizeof(music), "%s/Music", root) < (int)sizeof(music) && browser_state_directory(root, music, out, size)) {
		return true;
	}

	// The card may not be mounted yet; the browser can show an empty root.
	if (strlen(root) >= size) {
		return false;
	}
	memcpy(out, root, strlen(root) + 1);
	return true;
}
