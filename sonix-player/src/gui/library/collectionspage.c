#include "collectionspage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/collectionpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/library/collections.h"

lv_obj_t *collectionspage_screen;

static lv_obj_t *list;
static lv_obj_t *empty_label;
static lv_obj_t *title_label;

// Rows are rebuilt on every visit: a card holds a handful of collections, not
// thousands of albums, so the browser's windowing machinery would cost more
// than it saves here.
#define ROW_HEIGHT 88
#define ROW_GAP 8
#define ROW_RADIUS 12

// ---------------------------------------------------------------------------
// Picker mode: the albums waiting for a collection to be chosen. Owned by this
// page; deep-copied on the way in, so the caller may hand over anything. One
// array serves both entry points -- a single album is just a count of one.
// ---------------------------------------------------------------------------

static bool picking;
static char **pending_values;
static int pending_count;

// Bumped whenever the picker is entered or the page is opened for browsing. A
// worker's result carrying an older generation belongs to a session that is
// over: the albums it wrote are in the collection either way, but its toast
// and its way back have nowhere to go.
static uint32_t picking_generation;

static void pending_free(void) {
	for (int i = 0; i < pending_count; i++) {
		free(pending_values[i]);
	}
	free(pending_values);
	pending_values = NULL;
	pending_count = 0;
}

// ---------------------------------------------------------------------------
// Adding a set of albums
//
// Every album costs an indexed lookup and an insert, which for a whole
// selection at once is seconds of card access. It runs on a thread of its own,
// one set at a time, and comes back through gui_post().
// ---------------------------------------------------------------------------

typedef struct {
	int id;
	char **values;
	int count;
	int added;
	uint32_t generation;
} collection_batch_t;

static bool add_running;

static void batch_free(collection_batch_t *b) {
	for (int i = 0; i < b->count; i++) {
		free(b->values[i]);
	}
	free(b->values);
	free(b);
}

static void batch_done_cb(void *arg) {
	collection_batch_t *b = arg;
	add_running = false;
	toast_busy_end();

	if (b->generation != picking_generation) {
		// The page moved on to another session while the albums were being
		// written: not this page's toast to show, nor its way back to take.
		batch_free(b);
		return;
	}

	// The counts cannot say why an album was skipped -- already held, or
	// refused -- so the words claim only how many went in.
	if (b->added == b->count) {
		toast_success("collection_added");
	} else if (b->added > 0) {
		toast_error("collection_some_not_added");
	} else {
		gui_notify_popup("collection_cannot_add");
	}

	// The attempt is over either way: the user asked for these albums to go
	// into a collection, and a picker with the same albums still on it would
	// only ask again. Failure to even start the add is a different case
	// (picker_add_to) and does keep them.
	pending_free();
	back_btn_cb(NULL);
	batch_free(b);
}

static void *batch_worker(void *arg) {
	collection_batch_t *b = arg;
	thread_be_low_priority("collectionadd");

	int skipped = 0;
	collections_add_albums(b->id, (const char *const *)b->values, b->count, &b->added, &skipped);

	// gui_post refuses when its queue is full, and a dropped result would
	// leave the busy card up over a job that has finished. Wait for the queue
	// to take it: the busy veil only ever comes down in batch_done_cb, so this
	// is the one thing that must not give up.
	while (!gui_post(batch_done_cb, b)) {
		usleep(100000);
	}
	return NULL;
}

// Hands the pending albums to a worker that adds them to collection `id`.
// False when nothing could be started; the pending albums stay, so the choice
// can be made again.
static bool batch_start(int id) {
	if (add_running || pending_count <= 0) {
		return false;
	}

	collection_batch_t *b = calloc(1, sizeof(*b));
	if (!b) {
		return false;
	}
	b->id = id;
	b->count = pending_count;
	b->generation = picking_generation;
	b->values = malloc((size_t)b->count * sizeof(*b->values));
	if (!b->values) {
		free(b);
		return false;
	}
	for (int i = 0; i < b->count; i++) {
		b->values[i] = strdup(pending_values[i] ? pending_values[i] : "");
		if (!b->values[i]) {
			b->count = i;
			batch_free(b);
			return false;
		}
	}

	add_running = true;
	toast_busy("collection_loading");
	pthread_t thread;
	if (pthread_create(&thread, NULL, batch_worker, b) != 0) {
		add_running = false;
		toast_busy_end();
		batch_free(b);
		return false;
	}
	pthread_detach(thread);
	return true;
}

// Adds the pending albums to collection `id`, one synchronously, more on a
// worker. Either way the attempt ends the picker session: the caller has one
// copy of the values and the page has asked its question.
static void picker_add_to(int id) {
	if (pending_count <= 0 || add_running) {
		return;
	}

	if (pending_count == 1) {
		// One indexed lookup and one insert: no worker behind it.
		int added = 0;
		int skipped = 0;
		collections_add_albums(id, (const char *const *)pending_values, 1, &added, &skipped);
		if (added > 0) {
			toast_success("collection_added");
		} else {
			gui_notify_popup("collection_cannot_add");
		}
		// The attempt is over either way, as with the worker below.
		pending_free();
		back_btn_cb(NULL); // straight back to where the album was tapped
		return;
	}

	// More than one: the picker waits under the busy card until the worker
	// answers, and the done callback shows the toast and takes the way back.
	// No worker at all is a failure to start, not a failed add: the pending
	// albums stay, so the choice can be made again.
	if (!batch_start(id)) {
		gui_notify_popup("collection_cannot_add");
	}
}

// The naming dialog.
static lv_obj_t *name_layer; // full-screen cover holding the field + keyboard
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static void rebuild_rows(void);
static void name_layer_show(const char *initial);

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

// One row widget. The row owns its reference, so tapping it and the menu that
// opens from it see the same collection however far the list is scrolled.
typedef struct {
	int id;
	char name[];
} row_ref_t;

static void row_ref_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// Collection the ellipsis menu is acting on. Deleting confirms first: a
// collection is something the user built by hand.
static int menu_id;
static char menu_name[COLLECTION_NAME_MAX + 1];

// The collection the naming dialog is renaming, empty when it is creating one.
// The dialog is the same either way -- same field, same keyboard -- and this
// is what tells its accept button which of the two it is finishing.
static char renaming[COLLECTION_NAME_MAX + 1];

static void do_delete(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	if (collections_delete(menu_id)) {
		rebuild_rows();
	} else {
		gui_notify_popup("collection_cannot_delete");
	}
	menu_name[0] = '\0';
}

static void menu_delete_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	// The albums are named in the message so it says what stays: the shelf
	// goes, the albums remain in the library.
	char message[COLLECTION_NAME_MAX + 128];
	snprintf(message, sizeof(message), tr("collection_delete_message"), menu_name);
	confirm_show("collection_delete_title", message, "delete", do_delete, NULL);
}

// Renaming opens the same dialog that names a new collection, with the current
// name already in the field: the checks it has to pass are the same.
static void menu_rename_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	snprintf(renaming, sizeof(renaming), "%s", menu_name);
	name_layer_show(menu_name);
}

static void menu_btn_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	row_ref_t *ref = lv_event_get_user_data(e);
	if (!ref) {
		return;
	}
	menu_id = ref->id;
	snprintf(menu_name, sizeof(menu_name), "%s", ref->name);

	popover_item_t items[2] = {
		{"rename", menu_rename_action, NULL},
		{"delete", menu_delete_action, NULL},
	};
	popover_show(lv_event_get_current_target(e), items, 2);
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	row_ref_t *ref = lv_event_get_user_data(e);
	if (!ref) {
		return;
	}

	if (!picking) {
		collectionpage_open(ref->id, ref->name);
		return;
	}
	picker_add_to(ref->id);
}

// Builds one collection row. The count is the subtitle; the ellipsis exists
// only in browse mode, because a picker's rows have nothing to rename or
// delete -- tapping one is the whole point.
static void add_row(int id, const char *name, int albums) {
	row_ref_t *ref = malloc(sizeof(*ref) + strlen(name) + 1);
	if (!ref) {
		return;
	}
	ref->id = id;
	strcpy(ref->name, name);

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true); // the player sheet drags from here too
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, ref);
	lv_obj_add_event_cb(row, row_ref_free_cb, LV_EVENT_DELETE, ref);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, &icon_album);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_remove_style_all(texts);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_event_bubble(texts, true);

	lv_obj_t *name_label = lv_label_create(texts);
	lv_label_set_text(name_label, name);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, lv_pct(100));
	// One line, cut with dots past it.
	lv_obj_set_height(name_label, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_24, 0);

	char subtitle[48];
	snprintf(subtitle, sizeof(subtitle), tr(albums == 1 ? "collection_album" : "collection_albums"), albums);
	lv_obj_t *sub = lv_label_create(texts);
	lv_label_set_text(sub, subtitle);
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_16, 0);

	if (picking) {
		return;
	}

	// The ellipsis on the right edge, as on every track row.
	lv_obj_t *menu_btn = lv_btn_create(row);
	lv_obj_set_size(menu_btn, 44, 44);
	lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(menu_btn, 0, 0);
	lv_obj_set_style_shadow_width(menu_btn, 0, 0);
	lv_obj_set_style_pad_all(menu_btn, 0, 0);
	lv_obj_add_event_cb(menu_btn, menu_btn_cb, LV_EVENT_CLICKED, ref);

	lv_obj_t *dots = lv_image_create(menu_btn);
	lv_image_set_src(dots, &icon_ellipsis_vertical);
	lv_obj_add_style(dots, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
	lv_obj_center(dots);
}

// ---------------------------------------------------------------------------
// The naming dialog
// ---------------------------------------------------------------------------

static void name_layer_hide(void) {
	if (name_layer) {
		lv_obj_set_hidden(name_layer, true);
	}
	// A rename that was cancelled, or a dialog put away on the way into the
	// page: either way there is no longer a collection waiting to be renamed,
	// and leaving one here would turn the next "New collection" into a rename.
	renaming[0] = '\0';
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *typed = lv_textarea_get_text(name_field);
	char name[COLLECTION_NAME_MAX + 1];
	snprintf(name, sizeof(name), "%s", typed ? typed : "");

	// Trailing spaces produce a name that looks identical but is not.
	size_t len = strlen(name);
	while (len > 0 && name[len - 1] == ' ') {
		name[--len] = '\0';
	}
	if (len == 0) {
		gui_notify_popup("name_required");
		return;
	}

	// Renaming comes first: it is the one job of this dialog that acts on a
	// collection that already exists, so none of what follows applies to it.
	if (renaming[0]) {
		// strcasecmp, not strcmp: the registry matches names without case, so
		// this test has to let a change of capitalisation through --
		// collections_rename() is the one that knows whether the name really
		// belongs to another collection, and it refuses if it does.
		if (strcasecmp(renaming, name) != 0 && collections_exists(name, NULL)) {
			gui_notify_popup("collection_name_taken");
			return;
		}
		if (!collections_rename(menu_id, name)) {
			gui_notify_popup("collection_cannot_rename");
			return;
		}
		name_layer_hide(); // which is also what forgets the collection just renamed
		menu_name[0] = '\0';
		rebuild_rows();
		return;
	}

	if (collections_exists(name, NULL)) {
		gui_notify_popup("collection_name_taken");
		return;
	}

	int id = collections_create(name);
	if (id < 0) {
		gui_notify_popup("collection_cannot_create");
		return;
	}

	name_layer_hide();

	// Created from the picker: straight on to the albums it was made for. If
	// the add fails the empty collection stays, and the error says so.
	if (picking && pending_count > 0) {
		picker_add_to(id);
		if (add_running || pending_count == 0) {
			return;
		}
	}

	rebuild_rows();
}

static void name_layer_show(const char *initial) {
	lv_textarea_set_text(name_field, initial ? initial : "");
	keyboard_reset(name_keyboard);

	// LVGL restarts the caret blink on focus, and nothing ever focuses this
	// field because the keyboard types into it without it being tapped. Sending
	// the event by hand avoids a frozen caret here while the search page blinks.
	lv_obj_add_state(name_field, LV_STATE_FOCUSED);
	lv_obj_send_event(name_field, LV_EVENT_FOCUSED, NULL);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
}

static void new_collection_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	(void)e;
	renaming[0] = '\0';
	menu_name[0] = '\0';
	name_layer_show(NULL);
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// The revision the rows on screen were read at. A collection renamed, deleted
// or filled from inside the detail page changes a number on this page, and
// coming back to it with the chevron does not go through collectionspage_open()
// -- so the page checks, on the way in, whether anything it drew has moved
// since.
static unsigned rows_revision;

static bool collect_row_cb(int id, const char *name, int albums, void *user) {
	int *count = user;

	add_row(id, name, albums);
	(*count)++;
	return true;
}

static void rebuild_rows(void) {
	lv_obj_clean(list);

	// Collections live on the card: without it there is nothing to list and
	// nowhere to create one, so the page says so and offers no rows at all.
	if (!gui_card_available()) {
		lv_label_set_text(empty_label, tr("sd_card_missing"));
		lv_obj_set_hidden(empty_label, false);
		rows_revision = collections_revision();
		return;
	}

	// The "new collection" row comes first in both modes: it is the only way
	// the first collection can be made, picker or not.
	lv_obj_t *new_row = lv_btn_create(list);
	lv_obj_set_size(new_row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(new_row, &theme_style_card, 0);
	lv_obj_add_style(new_row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(new_row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(new_row, 0, 0);
	lv_obj_set_style_shadow_width(new_row, 0, 0);
	lv_obj_set_style_pad_hor(new_row, 16, 0);
	lv_obj_set_style_pad_column(new_row, 14, 0);
	lv_obj_set_flex_flow(new_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(new_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(new_row, true);
	lv_obj_add_event_cb(new_row, new_collection_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *new_icon = lv_image_create(new_row);
	lv_image_set_src(new_icon, &icon_plus);
	lv_obj_add_style(new_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(new_icon, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(new_icon, LV_OPA_COVER, 0);

	lv_obj_t *new_label = lv_label_create(new_row);
	lv_label_set_text(new_label, tr("collection_new"));
	lv_obj_add_style(new_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(new_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(new_label, theme()->accent, 0);

	// Taken before the list is read: a write that lands while the rows are
	// being drawn then differs from this value on the next look, so the
	// screen-loaded check reloads instead of certifying it as shown.
	rows_revision = collections_revision();

	int count = 0;
	collections_list(collect_row_cb, &count);

	if (count == 0) {
		lv_label_set_text(empty_label, tr("collection_empty_note"));
		lv_obj_set_hidden(empty_label, false);
	} else {
		lv_obj_set_hidden(empty_label, true);
	}
}

void collectionspage_open(void) {
	picking = false;
	pending_free();
	picking_generation++;
	lv_label_set_text(title_label, tr("collections"));
	// The row menu lives on the top layer, so it would stay over whatever
	// screen comes next; and its collection is gone with the rebuild.
	popover_close();
	name_layer_hide();
	menu_name[0] = '\0';
	rebuild_rows();
	switch_screen(collectionspage_screen);
}

// Picker mode for one album or a set of them: the caller has already handed
// the values over. Sets the title to say where the albums are about to go.
static void picker_enter(void) {
	picking = true;
	picking_generation++;
	lv_label_set_text(title_label, tr("collection_add_to_collection"));
	popover_close(); // a browse row menu has no meaning in the picker
	name_layer_hide();
	menu_name[0] = '\0';
	rebuild_rows();
	switch_screen(collectionspage_screen);
}

void collectionspage_add_album(const char *album_value) {
	const char *values[1] = {album_value};
	collectionspage_add_albums(values, album_value && album_value[0] ? 1 : 0);
}

void collectionspage_add_albums(const char *const *album_values, int count) {
	pending_free();
	if (!album_values || count <= 0) {
		return;
	}

	pending_values = malloc((size_t)count * sizeof(*pending_values));
	if (!pending_values) {
		return;
	}
	for (int i = 0; i < count; i++) {
		pending_values[pending_count] = strdup(album_values[i] ? album_values[i] : "");
		if (!pending_values[pending_count]) {
			pending_free();
			return;
		}
		pending_count++;
	}

	picker_enter();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (rows_revision != collections_revision()) {
		rebuild_rows();
	}
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// The pending albums belonged to the session this page is leaving; the
	// caller keeps its own copy. The generation bump discards the result of an
	// add still running, whose toast and way back have nowhere to go now.
	pending_free();
	picking_generation++;
	// The popover is on the top layer, above every screen: left open it would
	// hang over the page this one is being replaced by.
	popover_close();
}

void collectionspage_init(gui_config_t *cfg) {
	// gui.c created the screen; this page fills it, as collectionpage_init()
	// does. Creating another one here would leak the first.
	lv_obj_add_style(collectionspage_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(collectionspage_screen, cfg, "collections");
	// No corner buttons on this page: the heading gets the whole width.
	settingsrow_title_corner_slots(title_label, cfg, 0);

	int content_top = settingsrow_content_top(cfg);

	list = lv_obj_create(collectionspage_screen);
	lv_obj_set_size(list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(list, 12, 0);
	lv_obj_set_style_pad_gap(list, ROW_GAP, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);

	empty_label = lv_label_create(collectionspage_screen);
	lv_label_set_text(empty_label, tr("collection_empty_note"));
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_MID, 0, content_top + 140);
	lv_obj_set_hidden(empty_label, true);

	// --- the naming dialog: a full-screen layer with the field at the top and
	// the shared keyboard at the bottom, matching the playlist page.
	name_layer = lv_obj_create(collectionspage_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("collection_name"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	// Offset clear of the floating back chevron, which sits on top of this
	// layer; the same offset every page title uses.
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, 56, 56);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, COLLECTION_NAME_MAX);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);

	lv_obj_add_event_cb(collectionspage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(collectionspage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(collectionspage_screen);
	player_sheet_attach_drag(collectionspage_screen, true);
}
