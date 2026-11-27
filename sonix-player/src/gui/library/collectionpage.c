#include "collectionpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/library/collections.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *collectionpage_screen;

// The exact geometry of the library lists' rows, so a collection reads as one
// more of them. The rows stand at a fixed 100 px pitch with no gap in
// between: the content area is exactly count * 100 tall, which is what makes
// scroll position a row number.
#define ROW_HEIGHT 100
// One widget per coverloader slot: the two pools are the same size by
// construction, so a row can never be left asking for a slot that is not
// there.
#define ROW_POOL COVERLOADER_COLLECTION_COUNT
#define ROW_RADIUS 12
#define ROW_PAD 14
#define THUMB_SIZE 72

// How often finished artwork is collected while any is pending.
#define THUMB_POLL_MS 150

// One row widget. `index` is the absolute position in the collection of the
// album it shows -- absolute, not window-relative, so that a one-step slide
// of the window moves exactly one album onto a new widget and the other nine
// keep theirs. -1 is a widget parked because the window ran past the end of
// the list; -2 is "standing for something else now", set when the page is
// rebuilt, so the next binding cannot be skipped as already current.
typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	lv_obj_t *artist;
	lv_obj_t *menu_btn;

	int index;

	bool has_thumb;
	bool thumb_settled; // the worker answered, whatever it found
	cover_image_t thumb;
} row_t;

static row_t rows[ROW_POOL];

// The albums of the current window, as collections_albums_page() filled them.
static collection_album_t album_rows[ROW_POOL];

static lv_obj_t *page_title;
static lv_obj_t *list; // the scrollable viewport
static lv_obj_t *body; // the fixed-height canvas the rows are placed on
static lv_obj_t *empty_label;
static lv_timer_t *thumb_timer;

// What the page is open on. `page_generation` marks which collection a play
// job was started for: a result coming back after the page was opened on
// another collection belongs to neither and is thrown away.
static int current_id;
static char current_name[COLLECTION_NAME_MAX];
static uint32_t page_generation;

static int album_count;
static int window_first;
// The revision the page's data was read at: a change behind another screen is
// caught when the page is loaded again.
static unsigned loaded_revision;

// ---------------------------------------------------------------------------
// the page furniture
// ---------------------------------------------------------------------------

// A glyph button on the title row, counted from the right edge.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	if (cb) {
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	}

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

// The viewport of a windowed list: a scrollable page with a fixed-height canvas
// inside it, so ten row widgets can stand in for however many albums.
static lv_obj_t *make_viewport(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t **body_out, lv_obj_t **empty_out, int row_width, lv_event_cb_t scroll_cb) {
	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *viewport = lv_obj_create(screen);
	lv_obj_set_size(viewport, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(viewport, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(viewport, 0, 0);
	lv_obj_set_style_border_width(viewport, 0, 0);
	lv_obj_set_style_radius(viewport, 0, 0);
	lv_obj_set_style_pad_hor(viewport, cfg->padding, 0);
	lv_obj_set_style_pad_ver(viewport, 0, 0);
	lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(viewport, scroll_cb, LV_EVENT_SCROLL, NULL);
	// Presses on the rows and on the empty area bubble into the gesture
	// handlers; set here because an empty collection binds no row to set it
	// later.
	lv_obj_set_event_bubble(viewport, true);

	lv_obj_t *canvas = lv_obj_create(viewport);
	lv_obj_set_width(canvas, row_width);
	lv_obj_set_height(canvas, 1);
	lv_obj_set_pos(canvas, 0, 0);
	lv_obj_set_style_bg_opa(canvas, 0, 0);
	lv_obj_set_style_border_width(canvas, 0, 0);
	lv_obj_set_style_pad_all(canvas, 0, 0);
	lv_obj_set_scrollable(canvas, false);
	lv_obj_set_event_bubble(canvas, true);

	lv_obj_t *empty = lv_label_create(viewport);
	lv_obj_set_width(empty, row_width);
	lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty, &font_ui_24, 0);
	lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_set_hidden(empty, true);

	*body_out = canvas;
	*empty_out = empty;
	return viewport;
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

static int row_slot(const row_t *row) { return COVERLOADER_COLLECTION_BASE + (int)(row - rows); }

static void row_show_glyph(row_t *row) {
	lv_image_set_src(row->icon, &icon_album);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// A cover must not be tinted, so the recolour the glyph relies on is switched
// off for as long as a picture is on the row.
static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));

	if (row->has_thumb) {
		row_show_glyph(row); // stop pointing at the pixels before they go
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_settled = false;
}

static void row_bind(row_t *row, int index) {
	// The same album as before, wherever the window now sits: the labels and
	// the cover state are already its. Album j always lands on widget
	// j % ROW_POOL, so a one-step slide of the window moves one album onto
	// the widget the oldest one is leaving and leaves the other nine alone.
	if (row->index == index) {
		return;
	}

	row_drop_thumb(row);
	row->index = index;

	if (index < 0) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	const collection_album_t *album = &album_rows[index - window_first];
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_HEIGHT);
	lv_label_set_text(row->label, album->name);
	// A missing album keeps its stored names but wears the quiet colour and
	// says so under the title: it is still in the collection, only not on the
	// card any more.
	if (album->missing) {
		char detail[384];
		const char *note = tr("collection_not_in_library");
		if (album->artist[0]) {
			snprintf(detail, sizeof(detail), "%s - %s", album->artist, note);
		} else {
			snprintf(detail, sizeof(detail), "%s", note);
		}
		lv_label_set_text(row->artist, detail);
		lv_obj_add_style(row->label, &theme_style_text_dim, 0);
	} else {
		lv_label_set_text(row->artist, album->artist);
		lv_obj_remove_style(row->label, &theme_style_text_dim, 0);
	}

	// A cover is asked for only where there is a file to read it from: a
	// present album, or a missing one whose stored first path is still
	// readable. Anything else wears the album glyph.
	if (album->path[0]) {
		coverloader_request(row_slot(row), album->path, THUMB_SIZE);
	} else {
		row_show_glyph(row);
		row->thumb_settled = true; // no cover to wait for
	}
}

// Collects what the artwork worker has finished for the rows on screen. Runs
// whenever the page is active and something may be pending, not only when the
// window moved: the timer pauses itself while the page is under another
// screen, and this is what starts it again after a return.
static void thumbs_update(void) {
	bool anything_pending = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->thumb_settled) {
			continue;
		}

		cover_image_t image;
		bool finished = false;
		if (coverloader_take(row_slot(row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			row_show_cover(row, &row->thumb.dsc);
		} else if (finished) {
			row->thumb_settled = true; // no artwork in this one; the glyph stays
		} else {
			anything_pending = true;
		}
	}

	if (anything_pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update resumes it while work is pending
	if (lv_screen_active() == collectionpage_screen) {
		thumbs_update();
	}
}

// Reads the window the scroll position asks for and rebinds the pool to it.
static void window_update(void) {
	if (album_count <= 0) {
		window_first = -1;
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = scroll / ROW_HEIGHT;
	if (first + ROW_POOL > album_count) {
		first = album_count - ROW_POOL; // the last page is a full one
	}
	if (first < 0) {
		first = 0;
	}
	if (first == window_first) {
		// The scroll moved within the window; nothing to read back. The
		// covers go on being collected all the same: after a return from
		// another screen this is the only thing that starts the paused thumb
		// timer again.
		thumbs_update();
		return;
	}
	window_first = first;

	int filled = collections_albums_page(current_id, first, ROW_POOL, album_rows);
	for (int i = 0; i < ROW_POOL; i++) {
		// index -> widget index % ROW_POOL, so a one-step slide of the window
		// puts each album on the widget that is next over instead of renaming
		// the whole pool through one fixed mapping.
		int index = first + i;
		// The fill test is the relative slot: `filled` counts the albums read
		// back, while `index` is where this one sits in the whole collection.
		row_bind(&rows[index % ROW_POOL], i < filled ? index : -1);
	}

	thumbs_update();
}

static void scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

// Puts the page back together over the collection it is open on: the count,
// the canvas height, the scroll at the top, the empty note.
static void reload(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&rows[i]);
		rows[i].index = -2; // every widget now stands for a different album
	}

	album_count = collections_album_count(current_id);
	window_first = -1;

	lv_obj_set_height(body, album_count > 0 ? album_count * ROW_HEIGHT : 1);
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
	lv_obj_set_hidden(empty_label, album_count > 0);

	window_update();

	// Taken after the window is read, not before: reading it can heal a row,
	// and that write bumps the revision -- a page that counted its own read
	// against itself would come back from every visit scrolled to the top.
	loaded_revision = collections_revision();
}

// ---------------------------------------------------------------------------
// row behaviour
// ---------------------------------------------------------------------------

// The album a row widget is showing, or NULL when it shows nothing of the
// current window.
static const collection_album_t *row_album(const row_t *row) {
	if (row->index < 0 || row->index < window_first || row->index >= window_first + ROW_POOL) {
		return NULL;
	}
	return &album_rows[row->index - window_first];
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button != button) {
			continue;
		}
		const collection_album_t *album = row_album(&rows[i]);
		if (!album) {
			return;
		}
		if (album->missing) {
			confirm_notice("collection_not_in_library", "collection_missing_note");
		} else {
			medialist_open(album->value, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album->value);
		}
		return;
	}
}

// The collection item the ellipsis menu acts on, taken when the menu is
// opened. No confirmation: re-adding an album is one action.
static int menu_item;

static void remove_action(void *user) {
	(void)user;
	if (collections_remove_album(current_id, menu_item)) {
		toast_plain("collection_removed");
	}
	reload();
}

static void row_menu_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].menu_btn != button) {
			continue;
		}
		const collection_album_t *album = row_album(&rows[i]);
		if (!album) {
			return;
		}
		menu_item = album->idx;
		popover_item_t items[1] = {{"collection_remove_album", remove_action, NULL, false}};
		popover_show(button, items, 1);
		return;
	}
}

// ---------------------------------------------------------------------------
// playing the collection
//
// Walking every album for its tracks is card reads by the second, so it runs
// on a low-priority thread and comes back through gui_post(). One job at a
// time: a second tap while one is in flight would only queue the same work
// twice.
// ---------------------------------------------------------------------------

static bool play_running;

typedef struct {
	int id;
	bool shuffled;
	uint32_t generation;
	char **paths;
	int count;
} collection_play_t;

static void play_done_cb(void *arg) {
	collection_play_t *job = arg;
	play_running = false;
	toast_busy_end();

	if (job->generation != page_generation) {
		// The page moved on to another collection while the paths were being
		// gathered: not this page's queue to start.
		collections_paths_free(job->paths, job->count);
		free(job);
		return;
	}

	if (job->count <= 0) {
		gui_notify_popup("collection_cannot_play");
		collections_paths_free(job->paths, job->count);
		free(job);
		return;
	}

	// Mode first, as play_artist() does: with shuffle already the queue's way
	// of dealing, the list comes out shuffled in one step. The plain shuffle
	// deals once and stops; the other is the "Continuous shuffle" option and
	// comes round again in the same order.
	if (job->shuffled) {
		playlist_set_mode(musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE);
	} else {
		playlist_set_mode(PLAYBACK_MODE_NORMAL);
	}
	device_state_play_list((const char *const *)job->paths, job->count, 0);
	player_refresh_now_playing();
	switch_screen(player_screen);

	collections_paths_free(job->paths, job->count);
	free(job);
}

static void *play_worker(void *arg) {
	collection_play_t *job = arg;

	thread_be_low_priority("collectionplay");
	job->count = collections_track_paths(job->id, &job->paths);

	// gui_post refuses when its queue is full, and a dropped result would
	// leave the busy card up over a job that has finished. As the collection
	// add worker does: wait for the queue to take it. Giving up here would
	// strand play_running and the busy veil with no callback to clear them,
	// so this is the one thing that must not be dropped.
	while (!gui_post(play_done_cb, job)) {
		usleep(100000);
	}
	return NULL;
}

static void play_collection(bool shuffled) {
	if (play_running) {
		return;
	}

	collection_play_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->id = current_id;
	job->shuffled = shuffled;
	job->generation = page_generation;
	play_running = true;

	toast_busy("collection_loading");
	pthread_t thread;
	if (pthread_create(&thread, NULL, play_worker, job) != 0) {
		play_running = false;
		toast_busy_end();
		gui_notify_popup("collection_cannot_play");
		free(job);
		return;
	}
	pthread_detach(thread);
}

static void play_shuffle_action(void *user) {
	(void)user;
	play_collection(true);
}

static void play_sequence_action(void *user) {
	(void)user;
	play_collection(false);
}

static void play_button_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	// The same two words the library lists' circle-play menu uses.
	popover_item_t items[2] = {
		{"medialist_play_in_random_order", play_shuffle_action, NULL, false},
		{"medialist_play_in_sequence", play_sequence_action, NULL, false},
	};
	popover_show(lv_event_get_current_target(e), items, 2);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void page_loaded_cb(lv_event_t *e) {
	(void)e;
	if (loaded_revision != collections_revision()) {
		// Albums added, removed or merged elsewhere -- or the card changed:
		// read the collection again, from the top.
		reload();
		return;
	}
	window_update();
}

void collectionpage_open(int collection_id, const char *name) {
	current_id = collection_id;
	snprintf(current_name, sizeof(current_name), "%s", name ? name : "");
	page_generation++; // a play in flight belongs to the collection it left

	// The heading is data, not a tag. settingsrow_title() ran "collections"
	// through tr() to have something to show until now; the collection's own
	// name goes on raw, or one called "delete" would come out translated.
	lv_label_set_text(page_title, current_name);

	reload();
	switch_screen(collectionpage_screen);
}

void collectionpage_init(gui_config_t *cfg) {
	lv_obj_add_style(collectionpage_screen, &theme_style_screen, 0);
	page_title = settingsrow_title(collectionpage_screen, cfg, "collections");
	settingsrow_title_corner_slots(page_title, cfg, 1); // one corner slot: the play button
	corner_button(collectionpage_screen, cfg, 0, &icon_circle_play, play_button_cb);

	int width = cfg->screen_width - 2 * cfg->padding;
	list = make_viewport(collectionpage_screen, cfg, &body, &empty_label, width, scroll_cb);
	lv_label_set_text(empty_label, tr("collection_no_albums"));

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, ROW_PAD, 0);
		lv_obj_set_style_pad_column(row->button, ROW_PAD, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true); // so the player sheet can be dragged in
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		// The title, and under it the artist: a column, so the two share the
		// width the cover leaves instead of sitting beside each other.
		lv_obj_t *text = lv_obj_create(row->button);
		lv_obj_remove_style_all(text);
		lv_obj_set_flex_grow(text, 1);
		lv_obj_set_height(text, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_scrollable(text, false);
		lv_obj_set_clickable(text, false);
		lv_obj_set_event_bubble(text, true);

		row->label = lv_label_create(text);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->label, LV_PCT(100));
		// One line, always: a title on two lines would push the artist out of
		// the row.
		lv_obj_set_height(row->label, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->artist = lv_label_create(text);
		lv_label_set_long_mode(row->artist, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->artist, LV_PCT(100));
		lv_obj_set_height(row->artist, lv_font_get_line_height(&font_ui_16));
		lv_obj_add_style(row->artist, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->artist, &font_ui_16, 0);

		// The per-album menu, riding the right edge of the row.
		row->menu_btn = lv_btn_create(row->button);
		lv_obj_set_size(row->menu_btn, 44, 44);
		lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(row->menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
		lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
		lv_obj_add_event_cb(row->menu_btn, row_menu_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *dots = lv_image_create(row->menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);

		row->index = -1;
		row->has_thumb = false;
		row->thumb_settled = false;
	}

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);
	player_sheet_attach_drag(collectionpage_screen, true);
	switcher_attach_back_gesture(collectionpage_screen);
	lv_obj_add_event_cb(collectionpage_screen, page_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
