#include "btbrowser.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLAYER_PATH "/org/mpris/MediaPlayer2"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define FOLDER_IFACE "org.bluez.MediaFolder1"
#define ITEM_IFACE "org.bluez.MediaItem1"

#define ERR_INVALID "org.freedesktop.DBus.Error.InvalidArguments"
#define ERR_FAILED "org.freedesktop.DBus.Error.Failed"
#define ERR_UNSUPPORTED "org.bluez.Error.NotSupported"

// AVRCP gives a controller a small window at a time, and bluez's reply buffer
// is not infinite. Both limits are applied before a row is written, not after:
// a half-written tuple would corrupt the whole reply.
#define PAGE_MAX 24
#define BODY_BUDGET (14u * 1024u)
#define ENTRY_MAX 4096

// Enough slack for a folder path plus "/item/NNNNNNNN": the format-truncation
// checker needs to see that the concatenation cannot overflow the buffer.
#define PATH_MAX_LEN 384

typedef struct {
	char path[PATH_MAX_LEN];
	char name[256];
	char value[512]; // the raw library key, not the display name
	char folder_type[16];
	browse_kind_t kind;
	browse_filter_t filter;
	bool folder;
} folder_entry_t;

typedef struct {
	bool root;
	bool flat;
	char path[PATH_MAX_LEN];
	browse_kind_t kind;
	browse_filter_t filter;
	char value[512];
} folder_spec_t;

static const bt_browser_provider_t *provider;
static dbus_conn_t *browser_conn;
static pthread_mutex_t browser_lock = PTHREAD_MUTEX_INITIALIZER;
static folder_entry_t entries[ENTRY_MAX];
static size_t entry_count;
static char current_folder[PATH_MAX_LEN] = BROWSE_ROOT;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Library album keys carry a 0x1F and a suffix used by the SQL collation. A
// head unit should see only the part a person would call the name.
static void display_copy(char *dst, size_t size, const char *raw) {
	size_t len = 0;
	if (raw) {
		const char *cut = strchr(raw, 0x1f);
		len = cut ? (size_t)(cut - raw) : strlen(raw);
	}
	if (len > size - 1) {
		len = size - 1;
	}
	if (len) {
		memcpy(dst, raw, len);
	}
	dst[len] = '\0';
}

// A stable, short path for a raw library key. It is an address, not a title:
// the key itself lives in the entry table and is what goes back into SQL.
static uint32_t key_hash(const char *raw) {
	uint32_t h = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)(raw ? raw : ""); *p; p++) {
		h ^= *p;
		h *= 16777619u;
	}
	return h ? h : 1u;
}

// A fixed suffix is appended with memcpy after the prefix has been cut to fit.
// A printed "%s/%08x" into the destination makes the compiler rightly worry
// about a very long folder path; cutting first makes the bound obvious.
static void append_suffix(char *out, size_t size, const char *prefix, const char *suffix) {
	size_t slen = strlen(suffix);
	size_t n = strlen(prefix);
	if (n + slen >= size) {
		n = size - slen - 1;
	}
	memcpy(out, prefix, n);
	memcpy(out + n, suffix, slen + 1);
}

static void artist_folder_path(char *out, size_t size, const char *raw) {
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "/%08x", key_hash(raw));
	append_suffix(out, size, BROWSE_ROOT "/Artists", suffix);
}

static void album_folder_path(char *out, size_t size, const char *raw) {
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "/%08x", key_hash(raw));
	append_suffix(out, size, BROWSE_ROOT "/Albums", suffix);
}

static void nested_album_path(char *out, size_t size, const char *artist_path, const char *raw) {
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "/%08x", key_hash(raw));
	append_suffix(out, size, artist_path, suffix);
}

static void item_path(char *out, size_t size, const char *folder_path, uint32_t index) {
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "/item/%08x", index);
	append_suffix(out, size, folder_path, suffix);
}

// Strip the index slash first, then "/item", so a track in the flat folder
// and one in an album folder arrive at the same piece of code.
static bool split_item_path(const char *path, char *base, size_t base_size, uint32_t *index) {
	const char *slash = strrchr(path, '/');
	if (!slash || slash == path || !slash[1]) {
		return false;
	}
	const char *digits = slash + 1;
	size_t digits_len = strlen(digits);
	if (digits_len == 0 || digits_len > 8) {
		return false;
	}
	for (size_t i = 0; i < digits_len; i++) {
		if (!isxdigit((unsigned char)digits[i])) {
			return false;
		}
	}

	size_t len = (size_t)(slash - path);
	if (len >= 5 && strncmp(path + len - 5, "/item", 5) == 0) {
		len -= 5;
	}
	if (len == 0 || len >= base_size) {
		return false;
	}
	memcpy(base, path, len);
	base[len] = '\0';
	char *end = NULL;
	unsigned long v = strtoul(digits, &end, 16);
	if (*end) {
		return false;
	}
	*index = (uint32_t)v;
	return true;
}

static folder_entry_t *find_entry(const char *path) {
	for (size_t i = 0; i < entry_count; i++) {
		if (strcmp(entries[i].path, path) == 0) {
			return &entries[i];
		}
	}
	return NULL;
}

static void register_entry(const char *path, const char *name, browse_kind_t kind, browse_filter_t filter,
						   const char *value, bool folder, const char *folder_type) {
	if (find_entry(path)) {
		return; // a folder can be listed again after a later page
	}
	if (entry_count == ENTRY_MAX) {
		fprintf(stderr, "btbrowser: folder table full (%d); not registering %s\n", ENTRY_MAX, path);
		return;
	}

	folder_entry_t *e = &entries[entry_count++];
	memset(e, 0, sizeof(*e));
	snprintf(e->path, sizeof(e->path), "%s", path);
	snprintf(e->name, sizeof(e->name), "%s", name);
	snprintf(e->value, sizeof(e->value), "%s", value ? value : "");
	snprintf(e->folder_type, sizeof(e->folder_type), "%s", folder_type ? folder_type : "");
	e->kind = kind;
	e->filter = filter;
	e->folder = folder;
}

static void reset_entries(void) {
	entry_count = 0;
	register_entry(BROWSE_TRACKS_PATH, "Tracks", BROWSE_KIND_TRACKS, BROWSE_FILTER_NONE, "", true, "titles");
	register_entry(BROWSE_ROOT "/Artists", "Artists", BROWSE_KIND_ARTISTS, BROWSE_FILTER_NONE, "", true,
				   "artists");
	register_entry(BROWSE_ROOT "/Albums", "Albums", BROWSE_KIND_ALBUMS, BROWSE_FILTER_NONE, "", true, "albums");
}

static bool folder_spec_for_path(const char *path, folder_spec_t *spec) {
	memset(spec, 0, sizeof(*spec));
	snprintf(spec->path, sizeof(spec->path), "%s", path);

	if (strcmp(path, BROWSE_ROOT) == 0) {
		spec->root = true;
		return true;
	}
	if (strcmp(path, BROWSE_TRACKS_PATH) == 0) {
		spec->flat = true;
		return true;
	}
	if (strcmp(path, BROWSE_ROOT "/Artists") == 0) {
		spec->kind = BROWSE_KIND_ARTISTS;
		return true;
	}
	if (strcmp(path, BROWSE_ROOT "/Albums") == 0) {
		spec->kind = BROWSE_KIND_ALBUMS;
		return true;
	}

	const folder_entry_t *e = find_entry(path);
	if (!e || !e->folder) {
		return false;
	}
	spec->kind = e->kind;
	spec->filter = e->filter;
	snprintf(spec->value, sizeof(spec->value), "%s", e->value);
	return true;
}

// NumberOfItems is asked for on its own by bluez, before any ListItems. Use
// the same provider calls as a listing but with no rows requested.
static int folder_total(const folder_spec_t *spec) {
	if (spec->root) {
		return 3;
	}
	if (spec->flat) {
		return provider ? provider->refresh(provider->user) : -1;
	}
	if (!provider) {
		return -1;
	}
	int total = 0;
	int n = provider->list(provider->user, spec->kind, spec->filter,
						   spec->value[0] ? spec->value : NULL, 0, 0, NULL, &total);
	return n < 0 ? -1 : total;
}

// One row for an item addressed by its index below `spec`. Both the flat
// Tracks folder and every album folder resolve the same way.
static bool row_for_item(const folder_spec_t *spec, uint32_t index, browse_item_t *row, int *total) {
	if (!provider) {
		return false;
	}
	if (spec->flat) {
		int n = provider->refresh(provider->user);
		if (total) {
			*total = n;
		}
		if (n < 0 || (uint32_t)n <= index) {
			return false;
		}
		return provider->window(provider->user, (int)index, 1, row) == 1;
	}

	int n = provider->list(provider->user, spec->kind, spec->filter, spec->value[0] ? spec->value : NULL,
						   (int)index, 1, row, total);
	return n == 1;
}

// A deliberately generous upper bound on one (oa{sv}) tuple. Overestimating
// only ends a page early; underestimating would hand bluez a broken message.
static size_t tuple_estimate(const char *path, const browse_item_t *row, bool folder, const char *folder_type) {
	size_t n = 128 + strlen(path) + strlen(row->name);
	if (folder) {
		return n + strlen(folder_type);
	}
	return n + strlen(row->value) + strlen(row->artist) + 96;
}

// ---------------------------------------------------------------------------
// The a{sv} filter of ListItems
// ---------------------------------------------------------------------------

static bool parse_filter(const dbus_msg_t *m, uint32_t *start, uint32_t *end, bool *has_end) {
	dbus_reader_t r;
	dbus_reader_init(&r, m);
	dbus_array_iter_t it;
	if (!dbus_r_array_begin(&r, "{sv}", &it)) {
		return false;
	}

	*start = 0;
	*end = 0;
	*has_end = false;
	while (dbus_r_array_more(&r, &it)) {
		char key[64], sig[64];
		if (!dbus_r_string(&r, key, sizeof(key)) || !dbus_r_signature(&r, sig, sizeof(sig))) {
			return false;
		}
		if (strcmp(key, "Start") == 0 || strcmp(key, "End") == 0) {
			// The offsets are uint32 in the AVRCP API. A string here is a
			// malformed filter, not a value to be lenient about.
			if (strcmp(sig, "u") != 0) {
				return false;
			}
			uint32_t v = 0;
			if (!dbus_r_u32(&r, &v)) {
				return false;
			}
			if (strcmp(key, "Start") == 0) {
				*start = v;
			} else {
				*end = v;
				*has_end = true;
			}
		} else if (!dbus_r_skip(&r, sig)) {
			return false;
		}
	}
	return !r.bad;
}

// ---------------------------------------------------------------------------
// Writing replies
// ---------------------------------------------------------------------------

static void write_dict_props(dbus_writer_t *w, const char *name, const char *type, bool folder,
							 const char *folder_type, const browse_item_t *row, bool metadata) {
	dbus_w_dict_string(w, "Name", name);
	dbus_w_dict_string(w, "Type", type);
	dbus_w_dict_bool(w, "Playable", folder ? false : true);
	if (folder) {
		dbus_w_dict_string(w, "FolderType", folder_type ? folder_type : "mixed");
		return;
	}
	if (!metadata) {
		return;
	}

	// A real path, not a display name: the controller may use it to play the
	// item outside MediaItem1.Play as well.
	dbus_w_dict_variant_begin(w, "Metadata", "a{sv}");
	dbus_array_t meta;
	dbus_w_array_begin(w, "{sv}", &meta);
	dbus_w_dict_string(w, "xesam:title", row->name);
	if (row->artist[0]) {
		dbus_w_dict_string(w, "xesam:artist", row->artist);
	}
	dbus_w_dict_string(w, "xesam:url", row->value);
	dbus_w_array_end(w, &meta);
}

static void write_folder_props(dbus_writer_t *w, const folder_spec_t *spec) {
	int total = folder_total(spec);
	dbus_w_dict_variant_begin(w, "NumberOfItems", "u");
	dbus_w_u32(w, total < 0 ? 0u : (uint32_t)total);
	dbus_w_dict_string(w, "Type", "folder");
	dbus_w_dict_bool(w, "Playable", false);

	const char *name = spec->path;
	const char *slash = strstr(spec->path, "/Filesystem");
	if (slash) {
		name = slash;
	}
	dbus_w_dict_string(w, "Name", name);
}

static void write_item_props(dbus_writer_t *w, const browse_item_t *row) {
	browse_item_t display;
	display_copy(display.name, sizeof(display.name), row->name);
	write_dict_props(w, display.name, "audio", false, NULL, row, true);
}

// ---------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------

static void child_for_row(const folder_spec_t *spec, const browse_item_t *row, char *path, size_t path_size,
						  browse_kind_t *kind, browse_filter_t *filter, char *folder_type, size_t type_size) {
	*kind = BROWSE_KIND_TRACKS;
	*filter = BROWSE_FILTER_NONE;
	snprintf(folder_type, type_size, "titles");

	if (spec->kind == BROWSE_KIND_ARTISTS && spec->filter == BROWSE_FILTER_NONE) {
		artist_folder_path(path, path_size, row->value);
		*kind = BROWSE_KIND_ALBUMS;
		*filter = BROWSE_FILTER_ARTIST;
		snprintf(folder_type, type_size, "artists");
	} else if (spec->kind == BROWSE_KIND_ALBUMS && spec->filter == BROWSE_FILTER_NONE) {
		album_folder_path(path, path_size, row->value);
		*filter = BROWSE_FILTER_ALBUM;
		snprintf(folder_type, type_size, "albums");
	} else if (spec->kind == BROWSE_KIND_ALBUMS && spec->filter == BROWSE_FILTER_ARTIST) {
		nested_album_path(path, path_size, spec->path, row->value);
		*filter = BROWSE_FILTER_ALBUM;
		snprintf(folder_type, type_size, "albums");
	} else {
		item_path(path, path_size, spec->path, 0); // replaced by the caller
	}
}

static bool on_list_items(dbus_conn_t *c, const dbus_msg_t *m) {
	uint32_t start = 0, end = 0;
	bool has_end = false;
	if (!parse_filter(m, &start, &end, &has_end)) {
		dbus_error(c, m, ERR_INVALID, "Start and End must be uint32");
		return true;
	}

	folder_spec_t spec;
	if (!folder_spec_for_path(m->path, &spec)) {
		fprintf(stderr, "btbrowser: ListItems refused for %s\n", m->path);
		dbus_error(c, m, ERR_INVALID, "unknown folder");
		return true;
	}

	int total = folder_total(&spec);
	if (total < 0) {
		dbus_error(c, m, ERR_FAILED, "the library is not available");
		return true;
	}

	uint32_t wanted = 0;
	if (total > 0) {
		uint32_t last = has_end ? end : (uint32_t)(total - 1);
		if (last >= start) {
			uint64_t span = (uint64_t)last - start + 1;
			wanted = span > PAGE_MAX ? PAGE_MAX : (uint32_t)span;
		}
	}
	if (wanted > PAGE_MAX) {
		wanted = PAGE_MAX;
	}

	browse_item_t *rows = wanted ? calloc(wanted, sizeof(*rows)) : NULL;
	if (wanted && !rows) {
		dbus_error(c, m, ERR_FAILED, "out of memory");
		return true;
	}

	int got = 0;
	if (wanted) {
		if (spec.flat) {
			got = provider->window(provider->user, (int)start, (int)wanted, rows);
			if (got < 0) {
				// The handle went stale under the read. One rebuild and one
				// retry; a second failure is reported rather than repeated.
				folder_total(&spec);
				got = provider->window(provider->user, (int)start, (int)wanted, rows);
			}
		} else if (!spec.root) {
			int fresh_total = total;
			got = provider->list(provider->user, spec.kind, spec.filter,
								 spec.value[0] ? spec.value : NULL, (int)start, (int)wanted, rows,
								 &fresh_total);
			if (got < 0) {
				provider->refresh(provider->user);
				got = provider->list(provider->user, spec.kind, spec.filter,
									 spec.value[0] ? spec.value : NULL, (int)start, (int)wanted, rows,
									 &fresh_total);
			}
			if (got >= 0) {
				total = fresh_total;
			}
		}
	}
	if (got < 0) {
		free(rows);
		dbus_error(c, m, ERR_FAILED, "the library changed while listing");
		return true;
	}

	dbus_writer_t *w = dbus_reply_begin(c, m, "a(oa{sv})");
	dbus_array_t a;
	dbus_w_array_begin(w, "(oa{sv})", &a);
	size_t used = 8; // the array's length word and the structs' alignment

	if (spec.root) {
		static const char *const names[] = {"Artists", "Albums", "Tracks"};
		static const char *const paths[] = {BROWSE_ROOT "/Artists", BROWSE_ROOT "/Albums", BROWSE_TRACKS_PATH};
		static const char *const types[] = {"artists", "albums", "titles"};
		for (uint32_t i = start; wanted && i < start + wanted && i < 3; i++) {
			browse_item_t row;
			memset(&row, 0, sizeof(row));
			snprintf(row.name, sizeof(row.name), "%s", names[i]);
			snprintf(row.value, sizeof(row.value), "%s", paths[i]);
			if (used + tuple_estimate(paths[i], &row, true, types[i]) > BODY_BUDGET) {
				break;
			}
			dbus_w_pad(w, 8);
			dbus_w_path(w, paths[i]);
			dbus_array_t props;
			dbus_w_array_begin(w, "{sv}", &props);
			write_dict_props(w, names[i], "folder", true, types[i], &row, false);
			dbus_w_array_end(w, &props);
			used += tuple_estimate(paths[i], &row, true, types[i]);
		}
	} else {
		for (int i = 0; i < got; i++) {
			char path[PATH_MAX_LEN];
			browse_kind_t kind;
			browse_filter_t filter;
			char folder_type[16];
			bool folder = !spec.flat && !(spec.kind == BROWSE_KIND_TRACKS);

			child_for_row(&spec, &rows[i], path, sizeof(path), &kind, &filter, folder_type,
						  sizeof(folder_type));
			if (spec.flat || spec.kind == BROWSE_KIND_TRACKS) {
				item_path(path, sizeof(path), spec.path, (uint32_t)(start + (uint32_t)i));
			}

			browse_item_t display;
			memset(&display, 0, sizeof(display));
			display_copy(display.name, sizeof(display.name), rows[i].name);
			snprintf(display.value, sizeof(display.value), "%s", rows[i].value);
			snprintf(display.artist, sizeof(display.artist), "%s", rows[i].artist);
			size_t est = tuple_estimate(path, &display, folder, folder_type);
			if (used + est > BODY_BUDGET) {
				fprintf(stderr, "btbrowser: ListItems stopped at the 14 KB reply budget\n");
				break;
			}

			dbus_w_pad(w, 8);
			dbus_w_path(w, path);
			dbus_array_t props;
			dbus_w_array_begin(w, "{sv}", &props);
			write_dict_props(w, display.name, folder ? "folder" : "audio", folder, folder_type, &rows[i],
							 !folder);
			dbus_w_array_end(w, &props);
			used += est;

			if (folder) {
				register_entry(path, display.name, kind, filter, rows[i].value, true, folder_type);
			}
		}
	}

	dbus_w_array_end(w, &a);
	dbus_reply_send(c);
	free(rows);
	return true;
}

static bool on_change_folder(dbus_conn_t *c, const dbus_msg_t *m) {
	dbus_reader_t r;
	dbus_reader_init(&r, m);
	char wanted[PATH_MAX_LEN];
	if (!dbus_r_string(&r, wanted, sizeof(wanted))) {
		dbus_error(c, m, ERR_INVALID, "folder must be an object path");
		return true;
	}

	if (strcmp(wanted, BROWSE_ROOT) != 0) {
		const folder_entry_t *e = find_entry(wanted);
		if (!e || !e->folder) {
			fprintf(stderr, "btbrowser: ChangeFolder refused for %s\n", wanted);
			dbus_error(c, m, ERR_INVALID, "unknown folder");
			return true;
		}
	}

	folder_spec_t spec;
	if (!folder_spec_for_path(wanted, &spec)) {
		dbus_error(c, m, ERR_INVALID, "unknown folder");
		return true;
	}
	int total = folder_total(&spec);
	snprintf(current_folder, sizeof(current_folder), "%s", wanted);

	dbus_writer_t *w = dbus_reply_begin(c, m, "uu");
	dbus_w_u32(w, total < 0 ? 0u : (uint32_t)total);
	dbus_w_u32(w, 0);
	dbus_reply_send(c);
	return true;
}

static bool on_item_call(dbus_conn_t *c, const dbus_msg_t *m) {
	char base[PATH_MAX_LEN];
	uint32_t index = 0;
	if (!split_item_path(m->path, base, sizeof(base), &index)) {
		dbus_error(c, m, ERR_INVALID, "not an item path");
		return true;
	}

	folder_spec_t spec;
	if (!folder_spec_for_path(base, &spec)) {
		dbus_error(c, m, ERR_INVALID, "unknown item");
		return true;
	}

	browse_item_t row;
	int total = 0;
	if (!row_for_item(&spec, index, &row, &total) || (total > 0 && (uint32_t)total <= index)) {
		dbus_error(c, m, ERR_INVALID, "item is gone");
		return true;
	}

	if (strcmp(m->member, "Play") == 0) {
		if (provider && provider->play) {
			fprintf(stderr, "btbrowser: Play %s\n", row.value);
			provider->play(row.value, provider->user);
		}
		dbus_reply_begin(c, m, "");
		dbus_reply_send(c);
		return true;
	}

	// AddtoNowPlaying is not a separate operation here: playing the item is
	// the closest thing to loading it that this player has.
	if (strcmp(m->member, "AddtoNowPlaying") == 0) {
		dbus_error(c, m, ERR_UNSUPPORTED, "no separate now playing list");
		return true;
	}
	dbus_error(c, m, ERR_UNSUPPORTED, m->member);
	return true;
}

static bool on_properties(dbus_conn_t *c, const dbus_msg_t *m) {
	char base[PATH_MAX_LEN];
	uint32_t index = 0;
	bool item = split_item_path(m->path, base, sizeof(base), &index);
	folder_spec_t spec;
	browse_item_t row;

	if (item) {
		if (!folder_spec_for_path(base, &spec) || !row_for_item(&spec, index, &row, NULL)) {
			return false;
		}
	} else if (!folder_spec_for_path(m->path, &spec)) {
		return false; // the MPRIS player object, handled by btplayer.c
	}

	if (strcmp(m->member, "GetAll") == 0) {
		dbus_writer_t *w = dbus_reply_begin(c, m, "a{sv}");
		dbus_array_t a;
		dbus_w_array_begin(w, "{sv}", &a);
		if (item) {
			write_item_props(w, &row);
		} else {
			write_folder_props(w, &spec);
		}
		dbus_w_array_end(w, &a);
		dbus_reply_send(c);
		return true;
	}

	if (strcmp(m->member, "Get") == 0) {
		dbus_reader_t r;
		dbus_reader_init(&r, m);
		char iface[128], prop[128];
		if (!dbus_r_string(&r, iface, sizeof(iface)) || !dbus_r_string(&r, prop, sizeof(prop))) {
			dbus_error(c, m, ERR_INVALID, "Get needs an interface and a property");
			return true;
		}
		(void)iface;
		dbus_writer_t *w = dbus_reply_begin(c, m, "v");
		if (item) {
			if (strcmp(prop, "Name") == 0) {
				char name[256];
				display_copy(name, sizeof(name), row.name);
				dbus_w_variant_string(w, name);
			} else if (strcmp(prop, "Type") == 0) {
				dbus_w_variant_string(w, "audio");
			} else if (strcmp(prop, "Playable") == 0) {
				dbus_w_variant_bool(w, true);
			} else {
				dbus_w_variant_string(w, row.value);
			}
		} else if (strcmp(prop, "NumberOfItems") == 0) {
			int total = folder_total(&spec);
			dbus_w_variant_u32(w, total < 0 ? 0u : (uint32_t)total);
		} else {
			const char *name = spec.path;
			const char *slash = strstr(spec.path, "/Filesystem");
			if (slash) {
				name = slash;
			}
			dbus_w_variant_string(w, name);
		}
		dbus_reply_send(c);
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void btbrowser_init(dbus_conn_t *conn, const bt_browser_provider_t *p) {
	pthread_mutex_lock(&browser_lock);
	browser_conn = conn;
	provider = p;
	reset_entries();
	snprintf(current_folder, sizeof(current_folder), "%s", BROWSE_ROOT);
	pthread_mutex_unlock(&browser_lock);
}

void btbrowser_shutdown(void) {
	pthread_mutex_lock(&browser_lock);
	if (provider && provider->close) {
		provider->close(provider->user);
	}
	provider = NULL;
	browser_conn = NULL;
	entry_count = 0;
	pthread_mutex_unlock(&browser_lock);
}

bool btbrowser_on_call(dbus_conn_t *c, const dbus_msg_t *m, void *user) {
	(void)user;
	if (!c || !m) {
		return false;
	}

	pthread_mutex_lock(&browser_lock);
	bool handled;
	if (strcmp(m->interface, PROPS_IFACE) == 0) {
		handled = on_properties(c, m);
	} else if (strcmp(m->interface, FOLDER_IFACE) == 0) {
		if (strcmp(m->member, "ListItems") == 0) {
			handled = on_list_items(c, m);
		} else if (strcmp(m->member, "ChangeFolder") == 0) {
			handled = on_change_folder(c, m);
		} else {
			dbus_error(c, m, ERR_UNSUPPORTED, m->member);
			handled = true;
		}
	} else if (strcmp(m->interface, ITEM_IFACE) == 0) {
		handled = on_item_call(c, m);
	} else {
		handled = false;
	}
	pthread_mutex_unlock(&browser_lock);
	return handled;
}
