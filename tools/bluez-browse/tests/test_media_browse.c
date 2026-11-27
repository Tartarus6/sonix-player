/*
 * D-Bus simulation harness for media-browse.
 *
 * run-dbus-sim.sh starts a private session bus.  This program opens a client
 * connection through media-browse and a second connection that pretends to be
 * the player: org.bluez.MediaFolder1 at /org/mpris/MediaPlayer2/Filesystem,
 * org.bluez.MediaItem1 below it, and org.freedesktop.DBus.Properties on both.
 * It checks the async calls, folder path use, exact signature validation,
 * per-destination cancellation, Play targeting and the UID cache.
 */

#include <string.h>
#include <pthread.h>

#include "common.h"
#include "media-browse.h"
#include "avrcp-tg-browse.h"

#define GOOD_NAME "org.mpris.MediaPlayer2.SimTest"
#define DICT_NAME "org.mpris.MediaPlayer2.Dict"
#define BAD_NAME  "org.mpris.MediaPlayer2.Bad"

#define ROOT "/org/mpris/MediaPlayer2/Filesystem"
#define TRACKS ROOT "/Tracks"
#define ITEM0 TRACKS "/item/00000000"
#define ITEM1 TRACKS "/item/00000001"
#define PLAYER_PATH "/org/mpris/MediaPlayer2"

static int items_calls, error_calls, done_calls;
static uint32_t reported_total;
static char last_error[128];
static GSList *held_items;

static char item_names[3][64];
static char item_uris[3][160];
static uint32_t item_uids[3];
static uint8_t item_types[3];

static int list_calls, folder_getall_calls, item_getall_calls;
static int change_calls, play_calls;
static char last_list_path[160], last_folder_path[160], last_item_path[160];
static char last_change_path[160], last_change_arg[160], last_play_path[160];
static uint32_t last_list_start, last_list_end;

static void free_items(GSList *items)
{
	GSList *l;

	for (l = items; l != NULL; l = l->next) {
		struct media_browse_item *it = l->data;

		g_free(it->name);
		g_free(it->uri);
		g_free(it->mime);
		g_free(it);
	}
	g_slist_free(items);
}

static void on_items(GSList *items, uint32_t total, void *user_data)
{
	GSList *l;
	int i = 0;

	(void)user_data;
	items_calls++;
	reported_total = total;
	free_items(held_items);
	held_items = items;

	for (l = items; l != NULL && i < 3; l = l->next, i++) {
		struct media_browse_item *it = l->data;

		g_strlcpy(item_names[i], it != NULL ? it->name : "",
							sizeof(item_names[i]));
		g_strlcpy(item_uris[i], it != NULL ? it->uri : "",
							sizeof(item_uris[i]));
		item_uids[i] = it != NULL ? it->uid : 0;
		item_types[i] = it != NULL ? it->type : 0;
	}
}

static void on_error(unsigned id, const char *dest, const char *msg,
								void *user_data)
{
	(void)id;
	(void)dest;
	(void)user_data;
	error_calls++;
	g_strlcpy(last_error, msg, sizeof(last_error));
}

static void on_done(unsigned id, bool success, void *user_data)
{
	(void)id;
	(void)success;
	(void)user_data;
	done_calls++;
}

/* ------------------------------------------------------------------ */
/* Service-side reply builders                                         */
/* ------------------------------------------------------------------ */

static void dict_string(DBusMessageIter *dict, const char *key,
						const char *value)
{
	DBusMessageIter entry, var;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
									&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &value);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static void dict_bool(DBusMessageIter *dict, const char *key, bool value)
{
	DBusMessageIter entry, var;
	dbus_bool_t v = value ? TRUE : FALSE;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
									&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static void dict_path(DBusMessageIter *dict, const char *key,
						const char *value)
{
	DBusMessageIter entry, var;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
									&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "o", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_OBJECT_PATH, &value);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static void dict_u32(DBusMessageIter *dict, const char *key, uint32_t value)
{
	DBusMessageIter entry, var;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL,
									&entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &value);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
}

static void append_item(DBusMessageIter *array, bool as_dict,
					const char *path, const char *name,
					const char *type, const char *folder_type,
					bool playable, const char *title)
{
	DBusMessageIter tuple, props;

	if (as_dict)
		dbus_message_iter_open_container(array, DBUS_TYPE_DICT_ENTRY,
							NULL, &tuple);
	else
		dbus_message_iter_open_container(array, DBUS_TYPE_STRUCT,
							NULL, &tuple);
	dbus_message_iter_append_basic(&tuple, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&tuple, DBUS_TYPE_ARRAY, "{sv}",
								&props);

	dict_string(&props, "Name", name);
	dict_string(&props, "Type", type);
	if (folder_type != NULL)
		dict_string(&props, "FolderType", folder_type);
	dict_bool(&props, "Playable", playable);
	dict_path(&props, "Player", PLAYER_PATH);
	if (title != NULL) {
		DBusMessageIter entry, var, meta;
		const char *key = "Metadata";

		dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						"a{sv}", &var);
		dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY,
						"{sv}", &meta);
		dict_string(&meta, "xesam:title", title);
		dict_string(&meta, "xesam:artist", "Artist A");
		dict_string(&meta, "xesam:url", "file:///music/a.flac");
		dbus_message_iter_close_container(&var, &meta);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(&props, &entry);
	}

	dbus_message_iter_close_container(&tuple, &props);
	dbus_message_iter_close_container(array, &tuple);
}

static DBusConnection *pump_conn;
static volatile int pump_stop;

static void *pump_thread(void *unused)
{
	(void)unused;
	while (!pump_stop)
		dbus_connection_read_write_dispatch(pump_conn, 10);
	return NULL;
}

static bool parse_filter(DBusMessage *msg)
{
	DBusMessageIter root, dict;

	if (!dbus_message_iter_init(msg, &root) ||
			dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY)
		return false;

	last_list_start = 0;
	last_list_end = 0;
	dbus_message_iter_recurse(&root, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, var;
		const char *key;
		uint32_t value = 0;

		dbus_message_iter_recurse(&dict, &entry);
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			return false;
		dbus_message_iter_get_basic(&entry, &key);
		if (!dbus_message_iter_next(&entry) ||
			dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			return false;
		dbus_message_iter_recurse(&entry, &var);
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_UINT32)
			return false;
		dbus_message_iter_get_basic(&var, &value);
		if (strcmp(key, "Start") == 0)
			last_list_start = value;
		else if (strcmp(key, "End") == 0)
			last_list_end = value;
		dbus_message_iter_next(&dict);
	}

	return true;
}

static DBusHandlerResult service_filter(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	DBusMessage *reply;
	DBusMessageIter root, array;
	const char *dest, *iface, *member, *path;

	(void)user_data;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	dest = dbus_message_get_destination(msg);
	iface = dbus_message_get_interface(msg);
	member = dbus_message_get_member(msg);
	path = dbus_message_get_path(msg);

	/* Wrong reply signature: used to exercise exact signature checks. */
	if (dest != NULL && strcmp(dest, BAD_NAME) == 0) {
		const char *wrong = "wrong";

		reply = dbus_message_new_method_return(msg);
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &wrong,
						DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (iface != NULL && strcmp(iface, "org.freedesktop.DBus.Properties") ==
								0 &&
			member != NULL && strcmp(member, "GetAll") == 0) {
		DBusMessageIter in;
		const char *requested = NULL;

		if (dbus_message_iter_init(msg, &in) &&
			dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_STRING)
			dbus_message_iter_get_basic(&in, &requested);

		if (requested != NULL && strcmp(requested,
					"org.bluez.MediaFolder1") == 0) {
			folder_getall_calls++;
			g_strlcpy(last_folder_path, path, sizeof(last_folder_path));
			reply = dbus_message_new_method_return(msg);
			dbus_message_iter_init_append(reply, &root);
			dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
							"{sv}", &array);
			dict_u32(&array, "NumberOfItems", 42);
			dict_string(&array, "Type", "folder");
			dbus_message_iter_close_container(&root, &array);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		if (requested != NULL && strcmp(requested,
					"org.bluez.MediaItem1") == 0) {
			item_getall_calls++;
			g_strlcpy(last_item_path, path, sizeof(last_item_path));
			reply = dbus_message_new_method_return(msg);
			dbus_message_iter_init_append(reply, &root);
			dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
							"{sv}", &array);
			dict_string(&array, "Name", "Alpha Song");
			dict_string(&array, "Type", "audio");
			dbus_message_iter_close_container(&root, &array);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &root);
		dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
						"{sv}", &array);
		dbus_message_iter_close_container(&root, &array);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (iface != NULL && strcmp(iface, "org.bluez.MediaFolder1") == 0 &&
			member != NULL && strcmp(member, "ListItems") == 0) {
		bool as_dict = dest != NULL && strcmp(dest, DICT_NAME) == 0;

		if (!parse_filter(msg))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		list_calls++;
		g_strlcpy(last_list_path, path, sizeof(last_list_path));
		reply = dbus_message_new_method_return(msg);
		dbus_message_iter_init_append(reply, &root);
		dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
				as_dict ? "{oa{sv}}" : "(oa{sv})", &array);
		append_item(&array, as_dict, ROOT "/Artists", "Artists",
					"folder", "artists", false, NULL);
		append_item(&array, as_dict, ITEM0, "Alpha Song", "audio",
					NULL, true, "Ignored Title");
		/* Empty Name: the bridge must fall back to Metadata Title. */
		append_item(&array, as_dict, ITEM1, "", "audio", NULL,
					true, "Beta Title");
		dbus_message_iter_close_container(&root, &array);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (iface != NULL && strcmp(iface, "org.bluez.MediaFolder1") == 0 &&
			member != NULL && strcmp(member, "ChangeFolder") == 0) {
		DBusMessageIter in;
		const char *child = NULL;

		if (dbus_message_iter_init(msg, &in) &&
			dbus_message_iter_get_arg_type(&in) ==
							DBUS_TYPE_OBJECT_PATH)
			dbus_message_iter_get_basic(&in, &child);
		change_calls++;
		g_strlcpy(last_change_path, path, sizeof(last_change_path));
		g_strlcpy(last_change_arg, child != NULL ? child : "",
							sizeof(last_change_arg));
		reply = dbus_message_new_method_return(msg);
		if (change_calls == 1) {
			DBusMessageIter st;
			uint32_t total = 7, position = 0;

			dbus_message_iter_init_append(reply, &root);
			dbus_message_iter_open_container(&root, DBUS_TYPE_STRUCT,
							NULL, &st);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32,
								&total);
			dbus_message_iter_append_basic(&st, DBUS_TYPE_UINT32,
								&position);
			dbus_message_iter_close_container(&root, &st);
		}
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (iface != NULL && strcmp(iface, "org.bluez.MediaItem1") == 0 &&
			member != NULL && strcmp(member, "Play") == 0) {
		play_calls++;
		g_strlcpy(last_play_path, path, sizeof(last_play_path));
		reply = dbus_message_new_method_return(msg);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void pump(DBusConnection *client, int ms)
{
	int i;

	(void)pump_conn;
	for (i = 0; i < ms; i++)
		dbus_connection_read_write_dispatch(client, 1);
}

static DBusConnection *connect_bus(void)
{
	DBusError err;
	DBusConnection *conn;

	dbus_error_init(&err);
	conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (conn == NULL) {
		fprintf(stderr, "D-Bus connect failed: %s\n", err.message);
		dbus_error_free(&err);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	return conn;
}

int main(void)
{
	DBusConnection *client, *service;
	DBusError err;
	struct media_browse_cb cb = {
		.items = on_items, .error = on_error, .done = on_done,
	};
	struct media_browse *mb;
	unsigned id;
	int baseline_errors, baseline_done;
	pthread_t thread;
	uint64_t uid_a, uid_b;

	client = connect_bus();
	service = connect_bus();
	if (client == NULL || service == NULL)
		return 1;

	dbus_error_init(&err);
	CHECK(dbus_bus_request_name(service, GOOD_NAME, 0, &err) ==
					DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
		dbus_bus_request_name(service, DICT_NAME, 0, &err) ==
					DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
		dbus_bus_request_name(service, BAD_NAME, 0, &err) ==
					DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER,
						"service names acquired");
	dbus_error_free(&err);

	dbus_connection_add_filter(service, service_filter, NULL, NULL);
	pump_conn = service;
	pthread_create(&thread, NULL, pump_thread, NULL);

	mb = media_browse_new(client, &cb, NULL);

	/* A caller that passes the MPRIS player object must hit the app's root
	 * folder object, not the player object. */
	CHECK(strcmp(media_browse_folder_path(PLAYER_PATH), ROOT) == 0,
				"player path maps to root folder");
	CHECK(strcmp(media_browse_folder_path(ROOT), ROOT) == 0,
				"folder path is left unchanged");

	media_browse_uid_cache_clear();
	id = media_browse_list_items(mb, GOOD_NAME, PLAYER_PATH, 0, 1);
	CHECK(id != 0, "list_items returns a call id");
	pump(client, 40);
	CHECK(items_calls == 1, "items callback fired once");
	CHECK(list_calls == 1, "ListItems was called");
	CHECK(strcmp(last_list_path, ROOT) == 0,
					"ListItems used the root folder path");
	CHECK(last_list_start == 0 && last_list_end == 1,
					"Start/End sent as uint32 filter");
	CHECK(g_slist_length(held_items) == 3, "three items from ListItems");
	CHECK(reported_total == 42, "folder NumberOfItems reported as total");
	CHECK(folder_getall_calls >= 1, "folder GetAll supplied the total");
	CHECK(media_browse_count(mb) == 42, "last count is the folder total");
	CHECK(media_browse_uid_cache_size() == 3, "three UIDs cached");
	CHECK(strcmp(item_uris[1], ITEM0) == 0,
					"item uri is the object path");
	CHECK(item_uids[1] == media_browse_path_uid(ITEM0),
					"UID derived from object path");
	CHECK(strcmp(item_names[0], "Artists") == 0,
					"folder Name property used");
	CHECK(strcmp(item_names[1], "Alpha Song") == 0,
					"track Name property used");
	CHECK(strcmp(item_names[2], "Beta Title") == 0,
					"Metadata Title used when Name is empty");
	CHECK(item_types[0] == TG_ITEM_FOLDER, "folder item type");
	CHECK(item_types[1] == TG_ITEM_SONG && item_types[2] == TG_ITEM_SONG,
					"audio items map to songs");

	id = media_browse_get_count(mb, GOOD_NAME, PLAYER_PATH);
	CHECK(id != 0, "get_count returns a call id");
	pump(client, 20);
	CHECK(media_browse_count(mb) == 42, "async count reads NumberOfItems");
	CHECK(strcmp(last_folder_path, ROOT) == 0,
					"count used the folder path");

	id = media_browse_get_all(mb, GOOD_NAME, ITEM0);
	CHECK(id != 0, "get_all returns a call id");
	pump(client, 20);
	CHECK(item_getall_calls == 1, "item GetAll was called");
	CHECK(strcmp(last_item_path, ITEM0) == 0,
					"GetAll used the item path");

	baseline_errors = error_calls;
	baseline_done = done_calls;
	id = media_browse_change_folder(mb, GOOD_NAME, PLAYER_PATH,
							item_uids[1]);
	CHECK(id != 0, "change_folder returns a call id");
	pump(client, 20);
	CHECK(change_calls == 1, "ChangeFolder was called once");
	CHECK(strcmp(last_change_path, ROOT) == 0,
					"ChangeFolder used the folder path");
	CHECK(strcmp(last_change_arg, ITEM0) == 0,
					"ChangeFolder carried the item object path");
	CHECK(error_calls == baseline_errors && done_calls == baseline_done + 1,
					"(uu) ChangeFolder reply accepted");

	id = media_browse_change_folder(mb, GOOD_NAME, PLAYER_PATH,
							item_uids[2]);
	CHECK(id != 0, "second change_folder returns a call id");
	pump(client, 20);
	CHECK(change_calls == 2 && strcmp(last_change_arg, ITEM1) == 0,
					"second ChangeFolder resolved the UID");
	CHECK(error_calls == baseline_errors,
					"empty ChangeFolder reply accepted");

	baseline_errors = error_calls;
	id = media_browse_play(mb, GOOD_NAME, ITEM1);
	CHECK(id != 0, "play returns a call id");
	pump(client, 20);
	CHECK(play_calls == 1, "Play was called once");
	CHECK(strcmp(last_play_path, ITEM1) == 0,
					"Play targeted the stored item object path");
	CHECK(error_calls == baseline_errors,
					"Play succeeded with an empty reply");

	/* The app replies a(oa{sv}); the dict form is accepted too. */
	baseline_done = done_calls;
	id = media_browse_list_items(mb, DICT_NAME, ROOT, 0, 2);
	CHECK(id != 0, "dict-form list returns a call id");
	pump(client, 30);
	CHECK(items_calls == 2 && g_slist_length(held_items) == 3,
					"a{oa{sv}} reply parsed");
	CHECK(done_calls == baseline_done + 1,
					"dict-form list completed");

	/* Per destination cancel: a call to a destination with no service must
	 * vanish without producing a callback. */
	baseline_errors = error_calls;
	id = media_browse_list_items(mb, "org.mpris.MediaPlayer2.Gone",
							PLAYER_PATH, 0, 1);
	CHECK(id != 0, "cancelled call gets an id");
	media_browse_cancel_dest(mb, "org.mpris.MediaPlayer2.Gone");
	pump(client, 20);
	CHECK(error_calls == baseline_errors, "cancelled call has no error");

	/* Signature validation: BAD_NAME returns a string instead of a list. */
	id = media_browse_list_items(mb, BAD_NAME, PLAYER_PATH, 0, 1);
	CHECK(id != 0, "bad-signature call gets an id");
	pump(client, 20);
	CHECK(error_calls == baseline_errors + 1 &&
			strstr(last_error, "signature") != NULL,
						"invalid signature reported");

	/* LRU eviction. */
	media_browse_uid_cache_clear();
	for (int i = 0; i < MEDIA_BROWSE_UID_CACHE + 1; i++) {
		char uri[64];

		snprintf(uri, sizeof(uri), "/org/mpris/MediaPlayer2/Track/%d", i);
		media_browse_uid_insert(uri, i + 1);
	}
	CHECK(media_browse_uid_cache_size() == MEDIA_BROWSE_UID_CACHE,
						"UID cache capped at 512");

	uid_a = media_browse_path_uid(ITEM0);
	media_browse_uid_cache_clear();
	uid_b = media_browse_path_uid(ITEM0);
	CHECK(uid_a != 0 && uid_a == uid_b && uid_a <= UINT32_MAX,
					"FNV-1a path UID is stable");

	CHECK(media_browse_count_sync(mb, GOOD_NAME, PLAYER_PATH) == 42,
				"player path maps to folder for sync count");
	CHECK(media_browse_count_sync(mb, GOOD_NAME, ROOT) == 42,
				"sync folder count");
	CHECK(media_browse_count_sync(mb, GOOD_NAME, TRACKS) == 42,
				"sync count on a nested folder");

	free_items(held_items);

	media_browse_free(mb);
	pump_stop = 1;
	pthread_join(thread, NULL);
	dbus_connection_close(client);
	dbus_connection_close(service);
	dbus_connection_unref(client);
	dbus_connection_unref(service);

	TEST_REPORT("media-browse");
}
