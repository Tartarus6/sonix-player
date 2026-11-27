/*
 * Asynchronous BlueZ media browsing bridge used by the AVRCP target.
 *
 * The player serves org.bluez.MediaFolder1 at /org/mpris/MediaPlayer2/
 * Filesystem and below; the children returned by ListItems are object paths
 * carrying org.bluez.MediaItem1.  This module turns those calls into AVRCP
 * folder items and plays an item by calling MediaItem1.Play on its path.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media-browse.h"
#include "avrcp-tg-browse.h"

enum call_kind {
	CALL_LIST,
	CALL_GET_ALL,
	CALL_COUNT,
	CALL_CHANGE,
	CALL_PLAY,
};

struct browse_call {
	unsigned id;
	enum call_kind kind;
	char *dest;
	char *path;
	char *arg;
	uint64_t uid;
	uint32_t start;
	uint32_t end;
	int retries;
	uint64_t stamp;
	DBusPendingCall *pending;
	struct media_browse *mb;
};

struct media_browse {
	DBusConnection *conn;
	struct media_browse_cb cb;
	void *user_data;
	GSList *calls;
	unsigned next_id;
	uint32_t last_count;
};

struct uid_entry {
	char *uri;
	uint64_t uid;
};

static GHashTable *uid_map;
static GSList *uid_lru;

/* ------------------------------------------------------------------ */
/* UID cache: 512 entries, least recently used evicted first.          */
/* ------------------------------------------------------------------ */

uint64_t media_browse_uid_lookup(const char *uri)
{
	struct uid_entry *e;

	if (uri == NULL || uid_map == NULL)
		return 0;

	e = g_hash_table_lookup(uid_map, uri);
	if (e == NULL)
		return 0;

	/* Touch: move to the front of the LRU list. */
	uid_lru = g_slist_remove(uid_lru, e);
	uid_lru = g_slist_prepend(uid_lru, e);

	return e->uid;
}

void media_browse_uid_insert(const char *uri, uint64_t uid)
{
	struct uid_entry *e;

	if (uri == NULL || *uri == '\0')
		return;

	if (uid_map == NULL) {
		uid_map = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);
		uid_lru = NULL;
	}

	e = g_hash_table_lookup(uid_map, uri);
	if (e != NULL) {
		e->uid = uid;
		uid_lru = g_slist_remove(uid_lru, e);
		uid_lru = g_slist_prepend(uid_lru, e);
		return;
	}

	e = g_new0(struct uid_entry, 1);
	e->uri = g_strdup(uri);
	e->uid = uid;
	g_hash_table_insert(uid_map, e->uri, e);
	uid_lru = g_slist_prepend(uid_lru, e);

	while (g_slist_length(uid_lru) > MEDIA_BROWSE_UID_CACHE) {
		GSList *last = g_slist_last(uid_lru);
		struct uid_entry *old = last->data;

		g_hash_table_remove(uid_map, old->uri);
		uid_lru = g_slist_remove_link(uid_lru, last);
		g_slist_free1(last);
	}
}

unsigned media_browse_uid_cache_size(void)
{
	return uid_map != NULL ? g_hash_table_size(uid_map) : 0;
}

void media_browse_uid_cache_clear(void)
{
	if (uid_map != NULL)
		g_hash_table_remove_all(uid_map);
	g_slist_free(uid_lru);
	uid_lru = NULL;
}

/*
 * The same 32 bit FNV-1a used by the player's browsing tree: the path is the
 * identity of a folder or item, and the UID must stay stable across folder
 * changes.  The result is stored in the LRU cache as well.
 */
uint64_t media_browse_path_uid(const char *path)
{
	uint64_t cached;
	uint32_t hash = 2166136261u;
	const unsigned char *p;

	if (path == NULL || *path == '\0')
		return 0;

	cached = media_browse_uid_lookup(path);
	if (cached != 0)
		return cached;

	for (p = (const unsigned char *) path; *p != '\0'; p++) {
		hash ^= *p;
		hash *= 16777619u;
	}
	if (hash == 0)
		hash = 1;

	media_browse_uid_insert(path, hash);

	return hash;
}

/* Borrowed string: valid until the cache entry is evicted or cleared. */
const char *media_browse_uid_path(uint64_t uid)
{
	GSList *l;

	for (l = uid_lru; l != NULL; l = l->next) {
		struct uid_entry *e = l->data;

		if (e != NULL && e->uid == uid)
			return e->uri;
	}

	return NULL;
}

bool media_browse_check_signature(DBusMessage *reply, const char *expected)
{
	const char *got;

	if (reply == NULL || expected == NULL)
		return false;

	got = dbus_message_get_signature(reply);

	return got != NULL && strcmp(got, expected) == 0;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

const char *media_browse_folder_path(const char *path)
{
	if (path == NULL || *path == '\0' ||
			strcmp(path, "/org/mpris/MediaPlayer2") == 0)
		return MEDIA_BROWSE_ROOT_FOLDER;

	return path;
}

const char *media_browse_parent(const char *path, char *buf, size_t len)
{
	char *slash;

	if (path == NULL || buf == NULL || len == 0)
		return NULL;

	g_strlcpy(buf, path, len);
	slash = strrchr(buf, '/');
	if (slash == NULL || slash == buf)
		return buf;

	*slash = '\0';

	return buf;
}

/* ------------------------------------------------------------------ */
/* Reply parsing                                                       */
/* ------------------------------------------------------------------ */

/* The iterator points at a variant; read a string out of it when present. */
static void read_variant_string(DBusMessageIter *value, const char **out)
{
	DBusMessageIter var;
	const char *s = NULL;

	*out = NULL;
	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_VARIANT)
		return;

	dbus_message_iter_recurse(value, &var);
	if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING)
		dbus_message_iter_get_basic(&var, &s);

	*out = s;
}

static bool read_variant_bool(DBusMessageIter *value, bool *out)
{
	DBusMessageIter var;
	dbus_bool_t b = FALSE;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_VARIANT)
		return false;

	dbus_message_iter_recurse(value, &var);
	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
		return false;

	dbus_message_iter_get_basic(&var, &b);
	*out = b != FALSE;

	return true;
}

static bool read_variant_u32(DBusMessageIter *value, uint32_t *out)
{
	DBusMessageIter var;
	uint32_t n = 0;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_VARIANT)
		return false;

	dbus_message_iter_recurse(value, &var);
	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_UINT32)
		return false;

	dbus_message_iter_get_basic(&var, &n);
	*out = n;

	return true;
}

struct item_props {
	const char *name;
	const char *type;
	const char *folder_type;
	const char *title;
	const char *artist;
	bool playable;
	bool has_playable;
};

/* Metadata is a{sv}; the app writes xesam:title/xesam:artist, but accept the
 * plain Title/Artist keys too. */
static void parse_metadata(DBusMessageIter *value, struct item_props *props)
{
	DBusMessageIter var, array;

	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_VARIANT)
		return;

	dbus_message_iter_recurse(value, &var);
	if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(&var, &array);
	while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *key;

		dbus_message_iter_recurse(&array, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			break;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry))
			break;

		if (strcmp(key, "xesam:title") == 0 ||
				strcmp(key, "Title") == 0 ||
				strcmp(key, "title") == 0)
			read_variant_string(&entry, &props->title);
		else if (strcmp(key, "xesam:artist") == 0 ||
				strcmp(key, "Artist") == 0 ||
				strcmp(key, "artist") == 0)
			read_variant_string(&entry, &props->artist);

		dbus_message_iter_next(&array);
	}
}

static void parse_item_props(DBusMessageIter *array, struct item_props *props)
{
	DBusMessageIter dict;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *key;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			break;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry))
			break;

		if (strcmp(key, "Name") == 0)
			read_variant_string(&entry, &props->name);
		else if (strcmp(key, "Type") == 0)
			read_variant_string(&entry, &props->type);
		else if (strcmp(key, "FolderType") == 0)
			read_variant_string(&entry, &props->folder_type);
		else if (strcmp(key, "Playable") == 0) {
			props->has_playable =
				read_variant_bool(&entry, &props->playable);
		} else if (strcmp(key, "Metadata") == 0)
			parse_metadata(&entry, props);

		dbus_message_iter_next(&dict);
	}
}

static struct media_browse_item *make_item(const char *path,
						const struct item_props *props)
{
	struct media_browse_item *it;
	const char *name;
	bool folder;

	it = g_new0(struct media_browse_item, 1);
	it->uri = g_strdup(path);
	it->uid = media_browse_path_uid(path);

	name = props->name != NULL && *props->name != '\0' ?
			props->name : props->title;
	if (name == NULL || *name == '\0')
		name = path;
	it->name = g_strdup(name);
	it->mime = g_strdup(props->type != NULL ? props->type : "");

	folder = !props->has_playable || !props->playable;
	if (props->type != NULL && strcmp(props->type, "folder") == 0)
		folder = true;
	if (props->folder_type != NULL && *props->folder_type != '\0')
		folder = true;
	it->playable = props->has_playable ? props->playable : !folder;

	if (folder) {
		it->type = TG_ITEM_FOLDER;
	} else if (props->type != NULL && strcmp(props->type, "audio") == 0) {
		it->type = TG_ITEM_SONG;
	} else if (props->type != NULL && strcmp(props->type, "video") == 0) {
		it->type = TG_ITEM_VIDEO;
	} else if (props->type != NULL && strcmp(props->type, "image") == 0) {
		it->type = TG_ITEM_IMAGE;
	} else {
		it->type = tg_map_item_type(path, it->mime, false);
	}

	return it;
}

/* The app replies a(oa{sv}); accept a{oa{sv}} as well, since both carry one
 * object path and one property dictionary per entry. */
static GSList *parse_list_reply(DBusMessage *msg)
{
	DBusMessageIter root, entries;
	GSList *items = NULL;
	int element;

	if (!dbus_message_iter_init(msg, &root) ||
			dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return NULL;

	element = dbus_message_iter_get_element_type(&root);
	if (element != DBUS_TYPE_STRUCT && element != DBUS_TYPE_DICT_ENTRY)
		return NULL;

	dbus_message_iter_recurse(&root, &entries);
	while (dbus_message_iter_get_arg_type(&entries) == element) {
		DBusMessageIter tuple, props;
		const char *path = NULL;
		struct item_props parsed;

		memset(&parsed, 0, sizeof(parsed));
		dbus_message_iter_recurse(&entries, &tuple);
		if (dbus_message_iter_get_arg_type(&tuple) != DBUS_TYPE_OBJECT_PATH)
			break;
		dbus_message_iter_get_basic(&tuple, &path);
		if (!dbus_message_iter_next(&tuple) ||
				dbus_message_iter_get_arg_type(&tuple) !=
								DBUS_TYPE_ARRAY)
			break;

		props = tuple;
		parse_item_props(&props, &parsed);
		if (path != NULL && *path != '\0')
			items = g_slist_prepend(items, make_item(path, &parsed));

		dbus_message_iter_next(&entries);
	}

	return g_slist_reverse(items);
}

static uint32_t parse_folder_count(DBusMessage *msg)
{
	DBusMessageIter root, dict;
	uint32_t count = 0;

	if (!dbus_message_iter_init(msg, &root) ||
			dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(&root, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *key = NULL;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			break;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry)) {
			dbus_message_iter_next(&dict);
			continue;
		}
		if (key != NULL && strcmp(key, "NumberOfItems") == 0) {
			read_variant_u32(&entry, &count);
			break;
		}
		dbus_message_iter_next(&dict);
	}

	return count;
}

static uint32_t parse_change_count(DBusMessage *msg)
{
	DBusMessageIter iter, inner;
	uint32_t count = 0;

	if (!dbus_message_iter_init(msg, &iter))
		return 0;

	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRUCT) {
		dbus_message_iter_recurse(&iter, &inner);
		if (dbus_message_iter_get_arg_type(&inner) != DBUS_TYPE_UINT32)
			return 0;
		dbus_message_iter_get_basic(&inner, &count);
	} else if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
		dbus_message_iter_get_basic(&iter, &count);
	}

	return count;
}

static void items_free(GSList *items)
{
	GSList *l;

	for (l = items; l != NULL; l = l->next) {
		struct media_browse_item *it = l->data;

		if (it == NULL)
			continue;
		g_free(it->name);
		g_free(it->uri);
		g_free(it->mime);
		g_free(it);
	}
	g_slist_free(items);
}

static void call_free(struct browse_call *call)
{
	if (call == NULL)
		return;

	if (call->pending != NULL) {
		dbus_pending_call_cancel(call->pending);
		dbus_pending_call_unref(call->pending);
	}
	g_free(call->dest);
	g_free(call->path);
	g_free(call->arg);
	g_free(call);
}

static void report_error(struct browse_call *call, const char *message)
{
	struct media_browse *mb = call->mb;

	if (mb->cb.error != NULL)
		mb->cb.error(call->id, call->dest, message, mb->user_data);
	if (mb->cb.done != NULL)
		mb->cb.done(call->id, false, mb->user_data);
}

static void report_done(struct browse_call *call, bool success)
{
	struct media_browse *mb = call->mb;

	if (mb->cb.done != NULL)
		mb->cb.done(call->id, success, mb->user_data);
}

static void handle_reply(struct browse_call *call, DBusMessage *reply)
{
	struct media_browse *mb = call->mb;

	if (reply != NULL && dbus_message_get_type(reply) ==
						DBUS_MESSAGE_TYPE_ERROR) {
		report_error(call, dbus_message_get_error_name(reply) != NULL ?
				dbus_message_get_error_name(reply) : "D-Bus error");
		return;
	}

	switch (call->kind) {
	case CALL_LIST: {
		GSList *items;
		uint32_t total;

		if (!media_browse_check_signature(reply, "a(oa{sv})") &&
				!media_browse_check_signature(reply, "a{oa{sv}}")) {
			report_error(call, "Bad ListItems signature");
			break;
		}

		items = parse_list_reply(reply);
		total = mb->last_count;
		if (total < g_slist_length(items))
			total = g_slist_length(items);

		/* NumberOfItems is the true folder total; the list itself is only
		 * one AVRCP window.  Ask the folder for it when available. */
		{
			uint32_t folder_total = media_browse_count_sync(mb,
					call->dest, media_browse_folder_path(call->path));

			if (folder_total != 0)
				total = folder_total;
		}

		mb->last_count = total;
		if (mb->cb.items != NULL)
			mb->cb.items(items, total, mb->user_data);
		else
			items_free(items);
		report_done(call, true);
		break;
	}
	case CALL_COUNT:
		if (!media_browse_check_signature(reply, "a{sv}")) {
			report_error(call, "Bad GetAll signature");
			break;
		}
		mb->last_count = parse_folder_count(reply);
		report_done(call, true);
		break;
	case CALL_GET_ALL:
		if (!media_browse_check_signature(reply, "a{sv}")) {
			report_error(call, "Bad GetAll signature");
			break;
		}
		{
			uint32_t count = parse_folder_count(reply);

			if (count != 0)
				mb->last_count = count;
		}
		report_done(call, true);
		break;
	case CALL_CHANGE: {
		uint32_t count;

		if (!media_browse_check_signature(reply, "") &&
				!media_browse_check_signature(reply, "uu") &&
				!media_browse_check_signature(reply, "(uu)")) {
			report_error(call, "Bad reply signature");
			break;
		}
		count = parse_change_count(reply);
		if (count != 0)
			mb->last_count = count;
		report_done(call, true);
		break;
	}
	case CALL_PLAY:
		if (!media_browse_check_signature(reply, "")) {
			report_error(call, "Bad reply signature");
			break;
		}
		report_done(call, true);
		break;
	}
}

static void reply_cb(DBusPendingCall *pending, void *user_data)
{
	struct browse_call *call = user_data;
	DBusMessage *reply;

	if (call->mb == NULL || call->pending != pending)
		return;

	reply = dbus_pending_call_steal_reply(pending);
	handle_reply(call, reply);
	if (reply != NULL)
		dbus_message_unref(reply);

	call->mb->calls = g_slist_remove(call->mb->calls, call);
	call_free(call);
}

static void append_filter(DBusMessage *msg, uint32_t start, uint32_t end)
{
	DBusMessageIter iter, array, entry, var;
	static const char *keys[] = { "Start", "End" };
	uint32_t values[2];
	int i;

	values[0] = start;
	values[1] = end;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &array);
	for (i = 0; i < 2; i++) {
		const char *key = keys[i];

		dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
							"u", &var);
		dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &values[i]);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(&array, &entry);
	}
	dbus_message_iter_close_container(&iter, &array);
}

static bool send_call(struct media_browse *mb, struct browse_call *call)
{
	DBusMessage *msg = NULL;
	const char *path = media_browse_folder_path(call->path);

	switch (call->kind) {
	case CALL_LIST:
		msg = dbus_message_new_method_call(call->dest, path,
				MEDIA_BROWSE_FOLDER_IFACE, "ListItems");
		if (msg == NULL)
			return false;
		append_filter(msg, call->start, call->end);
		break;
	case CALL_COUNT:
	case CALL_GET_ALL: {
		const char *iface = call->kind == CALL_COUNT ?
				MEDIA_BROWSE_FOLDER_IFACE : MEDIA_BROWSE_ITEM_IFACE;

		msg = dbus_message_new_method_call(call->dest,
				call->kind == CALL_COUNT ? path : call->path,
				MEDIA_BROWSE_PROPERTIES, "GetAll");
		if (msg == NULL)
			return false;
		dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
						DBUS_TYPE_INVALID);
		break;
	}
	case CALL_CHANGE: {
		const char *child = media_browse_uid_path(call->uid);

		if (child == NULL)
			return false;
		msg = dbus_message_new_method_call(call->dest, path,
				MEDIA_BROWSE_FOLDER_IFACE, "ChangeFolder");
		if (msg == NULL)
			return false;
		dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &child,
						DBUS_TYPE_INVALID);
		break;
	}
	case CALL_PLAY:
		msg = dbus_message_new_method_call(call->dest, call->path,
				MEDIA_BROWSE_ITEM_IFACE, "Play");
		break;
	default:
		return false;
	}

	if (msg == NULL)
		return false;

	dbus_connection_send_with_reply(mb->conn, msg, &call->pending,
							MEDIA_BROWSE_TIMEOUT_MS);
	dbus_message_unref(msg);

	if (call->pending == NULL)
		return false;

	call->stamp = g_get_monotonic_time();

	return dbus_pending_call_set_notify(call->pending, reply_cb, call, NULL);
}

static unsigned new_call(struct media_browse *mb, enum call_kind kind,
					const char *dest, const char *path,
					uint32_t start, uint32_t end, uint64_t uid,
					const char *arg)
{
	struct browse_call *call;

	if (mb == NULL || mb->conn == NULL || dest == NULL || path == NULL)
		return 0;

	call = g_new0(struct browse_call, 1);
	call->id = ++mb->next_id;
	call->kind = kind;
	call->dest = g_strdup(dest);
	call->path = g_strdup(path);
	call->arg = g_strdup(arg != NULL ? arg : "");
	call->start = start;
	call->end = end;
	call->uid = uid;
	call->mb = mb;

	if (!send_call(mb, call)) {
		call_free(call);
		return 0;
	}

	mb->calls = g_slist_prepend(mb->calls, call);

	return call->id;
}

unsigned media_browse_list_items(struct media_browse *mb, const char *dest,
					const char *path, uint32_t start,
					uint32_t end)
{
	return new_call(mb, CALL_LIST, dest, media_browse_folder_path(path),
						start, end, 0, NULL);
}

unsigned media_browse_get_all(struct media_browse *mb, const char *dest,
					const char *path)
{
	return new_call(mb, CALL_GET_ALL, dest, path, 0, 0, 0, NULL);
}

unsigned media_browse_get_count(struct media_browse *mb, const char *dest,
					const char *path)
{
	return new_call(mb, CALL_COUNT, dest, media_browse_folder_path(path),
						0, 0, 0, NULL);
}

uint32_t media_browse_count_sync(struct media_browse *mb, const char *dest,
					const char *path)
{
	DBusMessage *msg, *reply;
	DBusError err;
	uint32_t count = 0;
	const char *iface = MEDIA_BROWSE_FOLDER_IFACE;

	if (mb == NULL || mb->conn == NULL || dest == NULL || path == NULL)
		return 0;

	msg = dbus_message_new_method_call(dest, media_browse_folder_path(path),
					MEDIA_BROWSE_PROPERTIES, "GetAll");
	if (msg == NULL)
		return 0;
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface,
							DBUS_TYPE_INVALID);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(mb->conn, msg,
							MEDIA_BROWSE_TIMEOUT_MS, &err);
	dbus_message_unref(msg);
	if (reply == NULL) {
		dbus_error_free(&err);
		return 0;
	}
	if (media_browse_check_signature(reply, "a{sv}"))
		count = parse_folder_count(reply);
	dbus_message_unref(reply);
	return count;
}

unsigned media_browse_change_folder(struct media_browse *mb, const char *dest,
					const char *path, uint64_t uid)
{
	return new_call(mb, CALL_CHANGE, dest, media_browse_folder_path(path),
							0, 0, uid, NULL);
}

unsigned media_browse_play(struct media_browse *mb, const char *dest,
					const char *item_path)
{
	if (item_path == NULL || *item_path == '\0')
		return 0;

	return new_call(mb, CALL_PLAY, dest, item_path, 0, 0, 0, NULL);
}

void media_browse_cancel_dest(struct media_browse *mb, const char *dest)
{
	GSList *l;

	if (mb == NULL || dest == NULL)
		return;

	for (l = mb->calls; l != NULL;) {
		GSList *next = l->next;
		struct browse_call *call = l->data;

		if (call != NULL && strcmp(call->dest, dest) == 0) {
			mb->calls = g_slist_remove_link(mb->calls, l);
			call_free(call);
			g_slist_free1(l);
		}

		l = next;
	}
}

void media_browse_dispatch(struct media_browse *mb, int timeout_ms)
{
	GSList *l;

	if (mb == NULL || mb->conn == NULL)
		return;

	dbus_connection_read_write(mb->conn, 0);
	dbus_connection_dispatch(mb->conn);

	for (l = mb->calls; l != NULL; l = l->next) {
		struct browse_call *call = l->data;
		uint64_t elapsed;

		if (call == NULL || call->pending == NULL ||
				dbus_pending_call_get_completed(call->pending))
			continue;

		elapsed = (g_get_monotonic_time() - call->stamp) / 1000;
		if (elapsed < (uint64_t) MEDIA_BROWSE_TIMEOUT_MS)
			continue;

		if (call->retries >= MEDIA_BROWSE_MAX_RETRIES) {
			report_error(call, "Timeout");
			dbus_pending_call_cancel(call->pending);
			call->pending = NULL;
			mb->calls = g_slist_remove(mb->calls, call);
			call_free(call);
			break;
		}

		/* One retry with the same destination and arguments. */
		dbus_pending_call_cancel(call->pending);
		dbus_pending_call_unref(call->pending);
		call->pending = NULL;
		call->retries++;
		send_call(mb, call);
	}

	if (timeout_ms > 0)
		dbus_connection_read_write_dispatch(mb->conn, timeout_ms);
}

uint32_t media_browse_count(struct media_browse *mb)
{
	return mb != NULL ? mb->last_count : 0;
}

struct media_browse *media_browse_new(DBusConnection *conn,
				const struct media_browse_cb *cb, void *user_data)
{
	struct media_browse *mb;

	if (conn == NULL)
		return NULL;

	mb = g_new0(struct media_browse, 1);
	mb->conn = conn;
	mb->user_data = user_data;
	if (cb != NULL)
		mb->cb = *cb;

	return mb;
}

void media_browse_free(struct media_browse *mb)
{
	GSList *l;

	if (mb == NULL)
		return;

	for (l = mb->calls; l != NULL; l = l->next)
		call_free(l->data);
	g_slist_free(mb->calls);
	g_free(mb);
}
