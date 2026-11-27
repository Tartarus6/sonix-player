#ifndef BTBROWSER_H
#define BTBROWSER_H

#include <stdbool.h>
#include "src/system/bluetooth/dbuslite.h"

// The object paths bluez asks about. The player is registered at
// /org/mpris/MediaPlayer2 (see btplayer.c); its filesystem scope lives below
// it. The item prefix is fixed for the flat Tracks folder, and nested folders
// put their own "/item/NNNNNNNN" below their path.
#define BROWSE_ROOT "/org/mpris/MediaPlayer2/Filesystem"
#define BROWSE_TRACKS_PATH BROWSE_ROOT "/Tracks"
#define BROWSE_ITEM_PREFIX BROWSE_TRACKS_PATH "/item/"

// Kinds the provider can list, and the filters that make sense for them.
// A value is always the raw library key: album names carry a 0x1F-separated
// suffix, which is stripped only for display and kept intact for SQL.
typedef enum {
	BROWSE_KIND_TRACKS = 0,
	BROWSE_KIND_ARTISTS,
	BROWSE_KIND_ALBUMS,
} browse_kind_t;

typedef enum {
	BROWSE_FILTER_NONE = 0,
	BROWSE_FILTER_ARTIST,
	BROWSE_FILTER_ALBUM,
} browse_filter_t;

typedef struct {
	char name[256];	  // display name; the library key is cut at byte 0x1F
	char value[512];	  // raw key for filtering, or the real file path for a track
	char artist[256];	  // artist of a track, empty for rows that have none
} browse_item_t;

// The player side of browsing. btplayer.c owns the library handles; this
// module only walks them and writes the D-Bus replies bluez wants.
//
// refresh() rebuilds the flat Tracks handle and answers how many rows it has.
// window() reads that handle; a negative answer means it went stale and has to
// be refreshed before the read is retried. close() drops it. list() opens a
// handle of its own for a dynamic folder; with out == NULL and count == 0 it
// only reports the total through *total. play() is handed a real file path.
typedef struct {
	void *user;
	int (*refresh)(void *user);
	int (*window)(void *user, int offset, int count, browse_item_t *out);
	void (*close)(void *user);
	void (*play)(const char *path, void *user);
	int (*list)(void *user, browse_kind_t kind, browse_filter_t filter, const char *value,
				int offset, int count, browse_item_t *out, int *total);
} bt_browser_provider_t;

// Called once after the player has registered with bluez. The connection is
// kept for replies; the provider must outlive btbrowser_shutdown().
void btbrowser_init(dbus_conn_t *conn, const bt_browser_provider_t *provider);
void btbrowser_shutdown(void);

// Called from the player's method handler. Returns false for messages this
// module does not own, so the MPRIS interfaces still reach btplayer.c.
bool btbrowser_on_call(dbus_conn_t *conn, const dbus_msg_t *msg, void *user);

#endif /* BTBROWSER_H */
