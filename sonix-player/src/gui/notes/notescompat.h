#ifndef GUI_NOTES_NOTESCOMPAT_H
#define GUI_NOTES_NOTESCOMPAT_H

// The storage half of the Notes app, kept inside the app so the port imports
// no other V1 feature group. Notes are plain text files in <card>/Notes; the
// page above this only draws what these helpers return.

#include <stdbool.h>
#include <stddef.h>

#define NOTES_FOLDER "Notes"
#define NOTES_MAX_BYTES (64 * 1024)
#define NOTES_MAX_CHARS (NOTES_MAX_BYTES / 4)
#define NOTES_NAME_MAX 96
#define NOTES_MAX_ENTRIES 200

typedef struct {
	char name[NOTES_NAME_MAX + 1];
} note_entry_t;

bool notes_dir(char *out, size_t size, const char *card_root);
bool notes_path(char *out, size_t size, const char *dir, const char *name);
bool notes_is_note_name(const char *name);
int notes_list(const char *dir, note_entry_t *out, int max);
char *notes_read(const char *path, bool *truncated);
bool notes_fits_editor(const char *text);
bool notes_file_name(const char *name, char *out, size_t size);
bool notes_write(const char *dir, const char *name, const char *text);
bool notes_name_ok(const char *name);
void notes_display_name(const char *name, char *out, size_t size);
void notes_default_name(const char *dir, char *out, size_t size);

#endif /* GUI_NOTES_NOTESCOMPAT_H */
