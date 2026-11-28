#include "notescompat.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Notes are ordinary text files. The editor writes UTF-8; files dropped onto
// the card may carry a UTF-8 BOM or Windows line endings, which are normalised
// here so the text view never has to draw either of them.

bool notes_dir(char *out, size_t size, const char *card_root) {
	if (!out || !size || !card_root || !card_root[0]) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", card_root, NOTES_FOLDER);
	return n > 0 && (size_t)n < size;
}

bool notes_path(char *out, size_t size, const char *dir, const char *name) {
	if (!out || !size || !dir || !name) {
		return false;
	}
	int n = snprintf(out, size, "%s/%s", dir, name);
	return n > 0 && (size_t)n < size;
}

static bool has_ext(const char *name, const char *ext) {
	size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}

bool notes_is_note_name(const char *name) {
	if (!name || !name[0] || name[0] == '.') {
		return false;
	}
	return has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".text");
}

// A-Z, case-insensitively, with digit runs compared as numbers: note-2 comes
// before note-10. Ties fall back to strcmp so the order is stable.
static int compare_names(const void *a, const void *b) {
	const char *sa = ((const note_entry_t *)a)->name;
	const char *sb = ((const note_entry_t *)b)->name;
	while (*sa && *sb) {
		if (isdigit((unsigned char)*sa) && isdigit((unsigned char)*sb)) {
			const char *ea = sa, *eb = sb;
			while (isdigit((unsigned char)*ea)) ea++;
			while (isdigit((unsigned char)*eb)) eb++;
			size_t la = (size_t)(ea - sa), lb = (size_t)(eb - sb);
			if (la != lb) {
				return la < lb ? -1 : 1;
			}
			int cmp = strncmp(sa, sb, la);
			if (cmp) {
				return cmp;
			}
			sa = ea;
			sb = eb;
			continue;
		}
		int ca = tolower((unsigned char)*sa), cb = tolower((unsigned char)*sb);
		if (ca != cb) {
			return ca < cb ? -1 : 1;
		}
		sa++;
		sb++;
	}
	return (*sa == 0) ? (*sb == 0 ? 0 : -1) : 1;
}

int notes_list(const char *dir, note_entry_t *out, int max) {
	if (!dir || !out || max <= 0) {
		return 0;
	}
	DIR *d = opendir(dir);
	if (!d) {
		return 0;
	}
	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && count < max) {
		if (!notes_is_note_name(ent->d_name)) {
			continue;
		}
		snprintf(out[count].name, sizeof(out[count].name), "%s", ent->d_name);
		count++;
	}
	closedir(d);
	qsort(out, (size_t)count, sizeof(out[0]), compare_names);
	return count;
}

// Turns raw bytes into the text the editor can draw. At most four output bytes
// per input byte, so the caller can always allocate 4 * len + 1.
static char *decode(const unsigned char *raw, size_t len) {
	char *out = malloc(len * 4 + 1);
	if (!out) {
		return NULL;
	}
	size_t w = 0;
	if (len >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
		len -= 3;
		raw += 3;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = raw[i];
		if (c == '\r') {
			if (i + 1 < len && raw[i + 1] == '\n') {
				i++;
			}
			out[w++] = '\n';
		} else if (c == '\t') {
			for (int t = 0; t < 4; t++) {
				out[w++] = ' ';
			}
		} else if (c >= 0x20 || c == '\n') {
			out[w++] = (char)c;
		}
	}
	out[w] = 0;
	return out;
}

char *notes_read(const char *path, bool *truncated) {
	if (truncated) {
		*truncated = false;
	}
	if (!path) {
		return NULL;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	unsigned char *raw = malloc(NOTES_MAX_BYTES);
	if (!raw) {
		fclose(f);
		return NULL;
	}
	size_t bytes = fread(raw, 1, NOTES_MAX_BYTES, f);
	if (truncated && bytes == NOTES_MAX_BYTES && fgetc(f) != EOF) {
		*truncated = true;
	}
	fclose(f);
	char *text = decode(raw, bytes);
	free(raw);
	return text;
}

bool notes_fits_editor(const char *text) {
	if (!text) {
		return true;
	}
	int chars = 0;
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if ((*p & 0xc0) != 0x80) {
			chars++;
			if (chars > NOTES_MAX_CHARS) {
				return false;
			}
		}
	}
	return true;
}

bool notes_name_ok(const char *name) {
	if (!name || !name[0]) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if (*p < 0x20 || *p == '/' || *p == '\\' || *p == ':') {
			return false;
		}
	}
	return true;
}

bool notes_file_name(const char *name, char *out, size_t size) {
	if (!notes_name_ok(name) || !out || !size) {
		return false;
	}
	if (has_ext(name, ".txt") || has_ext(name, ".md") || has_ext(name, ".text")) {
		int n = snprintf(out, size, "%s", name);
		return n > 0 && (size_t)n < size;
	}
	int n = snprintf(out, size, "%s.txt", name);
	return n > 0 && (size_t)n < size;
}

bool notes_write(const char *dir, const char *name, const char *text) {
	if (!dir || !notes_name_ok(name) || !text) {
		return false;
	}
	mkdir(dir, 0777);
	char path[1200], tmp[1200];
	if (!notes_path(path, sizeof(path), dir, name) ||
		snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		return false;
	}
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fputs(text, f) != EOF && fputc('\n', f) != EOF;
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok && rename(tmp, path) != 0) {
		ok = false;
	}
	if (!ok) {
		remove(tmp);
	}
	return ok;
}

void notes_display_name(const char *name, char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	size_t n = name ? strlen(name) : 0;
	if (has_ext(name, ".txt")) {
		n -= 4;
	} else if (has_ext(name, ".md")) {
		n -= 3;
	} else if (has_ext(name, ".text")) {
		n -= 5;
	}
	if (n >= size) {
		n = size - 1;
	}
	memcpy(out, name ? name : "", n);
	out[n] = 0;
}

void notes_default_name(const char *dir, char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	time_t now = time(NULL);
	struct tm *t = localtime(&now);
	char base[64];
	if (t) {
		snprintf(base, sizeof(base), "%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
	} else {
		snprintf(base, sizeof(base), "note");
	}
	snprintf(out, size, "%s", base);
	if (!dir) {
		return;
	}
	char path[1200], file[NOTES_NAME_MAX + 8];
	for (int i = 2; i < 1000; i++) {
		if (!notes_file_name(out, file, sizeof(file)) || !notes_path(path, sizeof(path), dir, file)) {
			return;
		}
		if (access(path, F_OK) != 0) {
			return;
		}
		snprintf(out, size, "%s-%d", base, i);
	}
}
