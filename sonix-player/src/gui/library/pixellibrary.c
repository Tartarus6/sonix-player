#include "pixellibrary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/browser.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/music.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/settings/musictiles.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------

#define HEAD_H 64
#define HEAD_BTN 48
#define TABS_H 56
#define TAB_H 44
#define STRIP_H 60 // the Shuffle / Playlists chip above a list
#define CARD_GAP 12
#define CARD_TEXT_H 74
#define CARD_RADIUS 22
#define ROW_H 76
#define ROW_GAP 8
#define ROW_THUMB 56
#define THUMB_SIZE 72 // what the rows ask the loader for
#define POLL_MS 40

// How many cards or rows exist: enough for a screen and a little over, reused
// as the list scrolls. Each one has its own slot in the cover loader.
#define POOL COVERLOADER_PIXELLIB_COUNT
#define WINDOW_ROWS 64
#define PLAYLISTS_MAX 256

typedef enum { MODE_NONE, MODE_ALBUMS, MODE_TRACKS, MODE_NAMES, MODE_PLAYLISTS, MODE_BROWSE } mode_t_;

typedef struct {
	lv_obj_t *obj;
	lv_obj_t *thumb_box; // the picture's rounded frame
	lv_obj_t *thumb;
	lv_obj_t *initial; // the first letter, where there is no picture
	lv_obj_t *panel;   // a card's tinted lower part
	lv_obj_t *title;
	lv_obj_t *sub;
	cover_image_t cover;
	int index;
	bool pending;
} item_t;

static gui_config_t *cfg_saved;
static lv_obj_t *root, *heading, *tabs, *list, *body, *strip_btn, *strip_label, *empty_label;
static lv_obj_t *tab_btns[MUSIC_TILE_COUNT];
static music_tile_t tab_tiles[MUSIC_TILE_COUNT];
static int tab_n;
static music_tile_t current_tile = MUSIC_TILE_ALBUMS;
static mode_t_ mode = MODE_NONE;
static bool shown;
static int scr_w, pad;
static int card_w, card_h, pitch; // pitch: one row of cards, or one list row, with its gap
// The album grid's columns: 2, 3 or 4, the same choice the ordinary album grid
// keeps ([library] album_grid_columns), and the text panel's height to match.
static int cols = 2;
static int text_h = CARD_TEXT_H;
static lv_obj_t *head_btns[3];
static lv_obj_t *sort_btn, *cols_btn;

static item_t pool[POOL];
static int pool_n;
static lv_timer_t *cover_poll;

// The list behind the current tab.
static library_index_t *ix;
static int count;
static library_list_t ix_kind;

// The rows held in memory, around the ones on screen.
typedef struct {
	char name[256];
	char path[512];
	char artist[128];
	bool has_path;
} winrow_t;
static winrow_t window[WINDOW_ROWS];
static int window_first = -1, window_count;

// The playlists, by name, with their track counts once read.
static char (*pl_names)[128];
static int *pl_counts;
static int pl_n;

// ---------------------------------------------------------------------------
// The rows of the list
// ---------------------------------------------------------------------------

typedef struct {
	int filled;
} fill_t;

static bool fill_cb(const char *name, const char *path, const char *artist, void *user) {
	fill_t *f = user;
	if (f->filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *r = &window[f->filled++];
	snprintf(r->name, sizeof(r->name), "%s", name ? name : "");
	r->has_path = path && path[0];
	snprintf(r->path, sizeof(r->path), "%s", path ? path : "");
	snprintf(r->artist, sizeof(r->artist), "%s", artist ? artist : "");
	return true;
}

static const winrow_t *row_at(int index) {
	if (!ix || index < 0 || index >= count) {
		return NULL;
	}
	if (window_first < 0 || index < window_first || index >= window_first + window_count) {
		int first = index - WINDOW_ROWS / 4;
		if (first < 0) {
			first = 0;
		}
		fill_t f = {0};
		library_index_window(ix, first, WINDOW_ROWS, fill_cb, &f);
		window_first = first;
		window_count = f.filled;
	}
	if (index < window_first || index >= window_first + window_count) {
		return NULL;
	}
	return &window[index - window_first];
}

static bool pl_collect(const char *name, void *user) {
	(void)user;
	if (pl_n >= PLAYLISTS_MAX) {
		return false;
	}
	snprintf(pl_names[pl_n], sizeof(pl_names[0]), "%s", name);
	pl_counts[pl_n] = -1;
	pl_n++;
	return true;
}

static void list_close(void) {
	if (ix) {
		library_index_close(ix);
		ix = NULL;
	}
	count = 0;
	window_first = -1;
	window_count = 0;
	pl_n = 0;
}

// ---------------------------------------------------------------------------
// The pictures
// ---------------------------------------------------------------------------

static int slot_of(const item_t *it) { return COVERLOADER_PIXELLIB_BASE + (int)(it - pool); }

static void item_drop_cover(item_t *it) {
	if (it->pending) {
		coverloader_release(slot_of(it));
		it->pending = false;
	}
	if (it->thumb) {
		lv_image_set_src(it->thumb, NULL);
	}
	cover_free(&it->cover);
	memset(&it->cover, 0, sizeof(it->cover));
}

// A card's lower part in a soft version of the sleeve's colour; the theme's
// surface while the picture is on its way, or with none.
static void card_tint(item_t *it, uint32_t tone) {
	if (!it->panel) {
		return;
	}
	lv_color_t base = theme()->surface;
	if (tone) {
		lv_color_t c = lv_color_hex(tone);
		base = theme_is_dark() ? lv_color_mix(c, lv_color_hex(0x202124), 90) : lv_color_mix(c, lv_color_white(), 80);
	}
	lv_obj_set_style_bg_color(it->panel, base, 0);
	// A short fade from the panel up into the picture, as on the reference.
	lv_obj_set_style_bg_grad_color(it->panel, base, 0);
}

static void poll_cb(lv_timer_t *timer) {
	bool any = false;
	for (int i = 0; i < pool_n; i++) {
		item_t *it = &pool[i];
		if (!it->pending) {
			continue;
		}
		cover_image_t c;
		bool finished = false;
		memset(&c, 0, sizeof(c));
		if (coverloader_take(slot_of(it), &c, &finished)) {
			cover_free(&it->cover);
			it->cover = c;
			it->cover.dsc.data = it->cover.pixels;
			lv_image_set_src(it->thumb, &it->cover.dsc);
			lv_obj_set_hidden(it->thumb, false);
			if (it->initial) {
				lv_obj_set_hidden(it->initial, true);
			}
			if (mode == MODE_ALBUMS) {
				card_tint(it, it->cover.has_tone ? it->cover.tone : cover_dominant_tone(&it->cover));
			}
		}
		it->pending = !finished;
		any |= it->pending;
	}
	if (!any) {
		lv_timer_pause(timer);
	}
}

static void item_request_cover(item_t *it, const char *path, int size) {
	item_drop_cover(it);
	lv_obj_set_hidden(it->thumb, true);
	if (it->initial) {
		lv_obj_set_hidden(it->initial, false);
	}
	if (path && path[0]) {
		coverloader_request(slot_of(it), path, size);
		it->pending = true;
		lv_timer_resume(cover_poll);
	}
}

// ---------------------------------------------------------------------------
// Building the cards and rows
// ---------------------------------------------------------------------------

static void item_click_cb(lv_event_t *e);

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, "");
	lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(l, font, 0);
	// One line exactly: a label as tall as its text wraps a long name onto a
	// second line instead of cutting it, and that line lands on the next one.
	lv_obj_set_height(l, lv_font_get_line_height(font));
	lv_obj_set_style_text_color(l, color, 0);
	lv_obj_set_clickable(l, false);
	return l;
}

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	return o;
}

static void item_base(item_t *it) {
	it->obj = lv_button_create(body);
	lv_obj_remove_style_all(it->obj);
	lv_obj_set_style_bg_opa(it->obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(it->obj, theme()->surface, 0);
	lv_obj_set_style_bg_color(it->obj, theme()->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_scrollable(it->obj, false);
	lv_obj_set_event_bubble(it->obj, true); // a drag that starts on it still scrolls the list
	lv_obj_add_event_cb(it->obj, item_click_cb, LV_EVENT_CLICKED, it);
	lv_obj_set_hidden(it->obj, true);
	it->index = -1;
}

static void make_card(item_t *it) {
	item_base(it);
	lv_obj_set_size(it->obj, card_w, card_h);
	lv_obj_set_style_radius(it->obj, CARD_RADIUS, 0);
	lv_obj_set_style_clip_corner(it->obj, true, 0);

	it->thumb_box = plain(it->obj);
	lv_obj_set_size(it->thumb_box, card_w, card_w);
	lv_obj_set_style_bg_opa(it->thumb_box, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(it->thumb_box, theme()->cover_bg, 0);
	it->initial = lv_image_create(it->thumb_box);
	lv_image_set_src(it->initial, &icon_menu_album);
	lv_obj_center(it->initial);
	it->thumb = lv_image_create(it->thumb_box);
	lv_obj_center(it->thumb);
	lv_image_set_inner_align(it->thumb, LV_IMAGE_ALIGN_COVER);
	lv_obj_set_size(it->thumb, card_w, card_w);

	// The text on a tinted panel that fades up into the picture.
	it->panel = plain(it->obj);
	lv_obj_set_size(it->panel, card_w, text_h + 26);
	lv_obj_set_pos(it->panel, 0, card_h - text_h - 26);
	lv_obj_set_style_bg_opa(it->panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_main_opa(it->panel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_grad_opa(it->panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_grad_dir(it->panel, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_main_stop(it->panel, 0, 0);
	lv_obj_set_style_bg_grad_stop(it->panel, 70, 0);
	card_tint(it, 0);

	// Smaller type on the narrower cards, and on four columns the name alone.
	const lv_font_t *tf = cols == 2 ? &font_ui_20_bold : cols == 3 ? &font_ui_18 : &font_ui_16;
	int inset = cols == 2 ? 12 : 8;
	it->title = label(it->obj, tf, theme()->text_primary);
	lv_obj_set_width(it->title, card_w - 2 * inset);
	lv_obj_set_pos(it->title, inset, card_h - text_h + 6);
	it->sub = label(it->obj, cols == 2 ? &font_ui_16 : &font_ui_14, theme()->text_secondary);
	lv_obj_set_width(it->sub, card_w - 2 * inset);
	lv_obj_set_pos(it->sub, inset, card_h - text_h + 6 + lv_font_get_line_height(tf) + 2);
	lv_obj_set_hidden(it->sub, cols >= 4);
}

static void make_row(item_t *it) {
	item_base(it);
	int w = scr_w - 2 * pad;
	lv_obj_set_size(it->obj, w, ROW_H);
	lv_obj_set_style_radius(it->obj, 18, 0);

	it->thumb_box = plain(it->obj);
	lv_obj_set_size(it->thumb_box, ROW_THUMB, ROW_THUMB);
	lv_obj_set_pos(it->thumb_box, 10, (ROW_H - ROW_THUMB) / 2);
	lv_obj_set_style_radius(it->thumb_box, 12, 0);
	lv_obj_set_style_clip_corner(it->thumb_box, true, 0);
	lv_obj_set_style_bg_opa(it->thumb_box, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(it->thumb_box, lv_color_mix(theme()->accent, theme()->surface, 60), 0);
	it->initial = label(it->thumb_box, &font_ui_24_bold, lv_color_white());
	lv_obj_center(it->initial);
	it->thumb = lv_image_create(it->thumb_box);
	lv_obj_set_size(it->thumb, ROW_THUMB, ROW_THUMB);
	lv_image_set_inner_align(it->thumb, LV_IMAGE_ALIGN_COVER);
	lv_obj_center(it->thumb);

	int tx = 10 + ROW_THUMB + 14;
	it->title = label(it->obj, &font_ui_20_bold, theme()->text_primary);
	lv_obj_set_width(it->title, w - tx - 14);
	lv_obj_set_pos(it->title, tx, 12);
	it->sub = label(it->obj, &font_ui_16, theme()->text_secondary);
	lv_obj_set_width(it->sub, w - tx - 14);
	lv_obj_set_pos(it->sub, tx, 42);
}

// The first letter of a name, as a picture for something with none.
static void set_initial(item_t *it, const char *name) {
	if (!it->initial || mode == MODE_ALBUMS) {
		return;
	}
	char first[8] = "";
	if (name && name[0]) {
		// One UTF-8 character.
		int n = 1;
		unsigned char c = (unsigned char)name[0];
		if (c >= 0xF0) {
			n = 4;
		} else if (c >= 0xE0) {
			n = 3;
		} else if (c >= 0xC0) {
			n = 2;
		}
		snprintf(first, sizeof(first), "%.*s", n, name);
	}
	lv_label_set_text(it->initial, first);
}

static int item_y(int index) {
	if (mode == MODE_ALBUMS) {
		return STRIP_H + (index / cols) * pitch;
	}
	return STRIP_H + index * pitch;
}

static void item_bind(item_t *it, int index) {
	it->index = index;
	if (mode == MODE_ALBUMS) {
		lv_obj_set_pos(it->obj, pad + (index % cols) * (card_w + CARD_GAP), item_y(index));
	} else {
		lv_obj_set_pos(it->obj, pad, item_y(index));
	}
	lv_obj_set_hidden(it->obj, false);

	if (mode == MODE_PLAYLISTS) {
		const char *name = pl_names[index];
		lv_label_set_text(it->title, name);
		if (pl_counts[index] < 0) {
			pl_counts[index] = playlists_count_tracks(name);
		}
		lv_label_set_text_fmt(it->sub, tr("pixellib_songs"), pl_counts[index]);
		set_initial(it, name);
		item_request_cover(it, NULL, THUMB_SIZE);
		return;
	}

	const winrow_t *r = row_at(index);
	if (!r) {
		lv_label_set_text(it->title, "");
		lv_label_set_text(it->sub, "");
		item_request_cover(it, NULL, THUMB_SIZE);
		return;
	}
	lv_label_set_text(it->title, r->name);
	lv_label_set_text(it->sub, r->artist);
	set_initial(it, r->name);
	if (mode == MODE_ALBUMS) {
		card_tint(it, 0);
	}
	item_request_cover(it, r->has_path ? r->path : NULL, mode == MODE_ALBUMS ? card_w : THUMB_SIZE);
}

static int items_total(void) { return mode == MODE_PLAYLISTS ? pl_n : count; }

static void window_update(void) {
	if (mode == MODE_NONE || mode == MODE_BROWSE || pool_n == 0) {
		return;
	}
	int total = items_total();
	int top = lv_obj_get_scroll_y(list) - STRIP_H;
	if (top < 0) {
		top = 0;
	}
	int per = mode == MODE_ALBUMS ? cols : 1;
	int first = top / pitch * per;
	// Leave one row above in hand for a flick back up.
	first -= per;
	if (first < 0) {
		first = 0;
	}
	int last = first + pool_n;
	if (last > total) {
		last = total;
	}
	for (int i = 0; i < pool_n; i++) {
		item_t *it = &pool[i];
		if (it->index >= 0 && (it->index < first || it->index >= last)) {
			item_drop_cover(it);
			lv_obj_set_hidden(it->obj, true);
			it->index = -1;
		}
	}
	for (int index = first; index < last; index++) {
		item_t *it = &pool[index % pool_n];
		if (it->index != index) {
			item_bind(it, index);
		}
	}
}

static void scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

// ---------------------------------------------------------------------------
// What a tap does: the same as on the ordinary lists
// ---------------------------------------------------------------------------

static void open_name(library_filter_t filter, const char *name) {
	if (filter == LIBRARY_FILTER_ARTIST) {
		medialist_open_artist(name);
		return;
	}
	if (medialist_album_view()) {
		medialist_open(name, LIBRARY_LIST_ALBUMS, filter, name);
	} else {
		medialist_open(name, LIBRARY_LIST_TRACKS, filter, name);
	}
}

static void item_click_cb(lv_event_t *e) {
	item_t *it = lv_event_get_user_data(e);
	if (it->index < 0 || player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = it->index;
	if (mode == MODE_PLAYLISTS) {
		char name[128];
		snprintf(name, sizeof(name), "%s", pl_names[index]);
		medialist_open(name, LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, name);
		return;
	}
	const winrow_t *r = row_at(index);
	if (!r) {
		return;
	}
	char name[256];
	snprintf(name, sizeof(name), "%s", r->name);
	switch (ix_kind) {
	case LIBRARY_LIST_TRACKS:
	case LIBRARY_LIST_FAVOURITES:
	case LIBRARY_LIST_MOST_PLAYED: {
		// The list it was tapped in becomes the queue, as on the list pages.
		char path[512];
		snprintf(path, sizeof(path), "%s", r->path);
		if (device_state_play_index(library_index_clone(ix), index)) {
			player_refresh_now_playing();
		} else {
			player_play_file(path);
		}
		switch_screen(player_screen);
		return;
	}
	case LIBRARY_LIST_ALBUMS:
		medialist_open(name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, name);
		return;
	case LIBRARY_LIST_ARTISTS:
		open_name(LIBRARY_FILTER_ARTIST, name);
		return;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		open_name(LIBRARY_FILTER_ALBUM_ARTIST, name);
		return;
	case LIBRARY_LIST_GENRES:
		open_name(LIBRARY_FILTER_GENRE, name);
		return;
	case LIBRARY_LIST_YEARS:
		open_name(LIBRARY_FILTER_YEAR, name);
		return;
	default:
		return;
	}
}

// The chip above the list: Shuffle on music, the full Playlists page (new,
// import, rename, delete) on the playlists, and the folders on Browse.
static void strip_click_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (mode == MODE_PLAYLISTS) {
		playlistpage_open();
		return;
	}
	if (mode == MODE_BROWSE) {
		switch_screen(browser_screen);
		return;
	}
	if (mode == MODE_ALBUMS) {
		if (device_state_play_random_album(LIBRARY_FILTER_NONE, NULL)) {
			player_refresh_now_playing();
			switch_screen(player_screen);
		}
		return;
	}
	if (ix && count > 0) {
		playlist_set_mode(PLAYBACK_MODE_SHUFFLE);
		if (device_state_play_index(library_index_clone(ix), rand() % count)) {
			player_refresh_now_playing();
			player_mode_changed();
			switch_screen(player_screen);
		}
	}
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------

static void pool_clear(void) {
	for (int i = 0; i < pool_n; i++) {
		item_drop_cover(&pool[i]);
	}
	memset(pool, 0, sizeof(pool));
	pool_n = 0;
}

static void grid_geometry(void) {
	long c = config_get_int("library", "album_grid_columns", 2);
	cols = c < 2 ? 2 : c > 4 ? 4 : (int)c;
	card_w = (scr_w - 2 * pad - (cols - 1) * CARD_GAP) / cols;
	text_h = cols == 2 ? CARD_TEXT_H : cols == 3 ? 52 : 34;
	card_h = card_w + text_h;
}

static void content_build(void);

static void sort_changed(void) { content_build(); }

static void sort_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	medialist_sort_menu(lv_event_get_current_target(e), ix_kind, sort_changed);
}

// The columns of the album grid, round from two to four, as the ordinary
// album grid's own button does.
static void cols_click_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int next = cols >= 4 ? 2 : cols + 1;
	config_set_int("library", "album_grid_columns", next);
	config_save();
	content_build();
}

// The button's picture: the grid it would make, as n x n squares.
static void cols_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_current_target(e), &a);
	int n = cols, size = 20, gap = 2;
	int cell = (size - gap * (n - 1)) / n;
	int x0 = (a.x1 + a.x2 - size) / 2, y0 = (a.y1 + a.y2 - size) / 2;
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = theme()->text_primary;
	d.radius = 2;
	for (int r = 0; r < n; r++) {
		for (int c = 0; c < n; c++) {
			lv_area_t sq = {x0 + c * (cell + gap), y0 + r * (cell + gap), x0 + c * (cell + gap) + cell - 1,
							y0 + r * (cell + gap) + cell - 1};
			lv_draw_rect(layer, &d, &sq);
		}
	}
}

// A round button at the right end of the chip row.
static lv_obj_t *strip_round(int slot, lv_event_cb_t cb) {
	lv_obj_t *b = lv_button_create(body);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, 44, 44);
	lv_obj_set_pos(b, scr_w - pad - 44 - slot * (44 + 8), 6);
	lv_obj_set_style_radius(b, 14, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, theme()->surface, 0);
	lv_obj_set_style_bg_color(b, theme()->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_event_bubble(b, true);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void content_build(void) {
	grid_geometry();
	pool_clear();
	lv_obj_clean(body);
	list_close();
	lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);

	// The chip, made again: it was a child of the body.
	strip_btn = lv_button_create(body);
	lv_obj_remove_style_all(strip_btn);
	lv_obj_set_size(strip_btn, LV_SIZE_CONTENT, 44);
	lv_obj_set_pos(strip_btn, pad, 6);
	lv_obj_set_style_radius(strip_btn, 22, 0);
	lv_obj_set_style_bg_opa(strip_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(strip_btn, lv_color_mix(theme()->accent, theme()->surface, 70), 0);
	lv_obj_set_style_pad_hor(strip_btn, 18, 0);
	lv_obj_set_style_pad_column(strip_btn, 8, 0);
	lv_obj_set_flex_flow(strip_btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(strip_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(strip_btn, true);
	lv_obj_add_event_cb(strip_btn, strip_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *strip_icon = lv_image_create(strip_btn);
	lv_obj_add_style(strip_icon, &theme_style_icon, 0);
	lv_image_set_scale(strip_icon, 200);
	strip_label = label(strip_btn, &font_ui_20_bold, theme()->text_primary);
	lv_label_set_long_mode(strip_label, LV_LABEL_LONG_CLIP);

	library_list_t kind = LIBRARY_LIST_TRACKS;
	switch (current_tile) {
	case MUSIC_TILE_ALBUMS:
		mode = MODE_ALBUMS;
		kind = LIBRARY_LIST_ALBUMS;
		break;
	case MUSIC_TILE_ALL:
		mode = MODE_TRACKS;
		kind = LIBRARY_LIST_TRACKS;
		break;
	case MUSIC_TILE_FAVOURITES:
		mode = MODE_TRACKS;
		kind = LIBRARY_LIST_FAVOURITES;
		break;
	case MUSIC_TILE_MOST_PLAYED:
		mode = MODE_TRACKS;
		kind = LIBRARY_LIST_MOST_PLAYED;
		break;
	case MUSIC_TILE_ARTISTS:
		mode = MODE_NAMES;
		kind = LIBRARY_LIST_ARTISTS;
		break;
	case MUSIC_TILE_ALBUM_ARTISTS:
		mode = MODE_NAMES;
		kind = LIBRARY_LIST_ALBUM_ARTISTS;
		break;
	case MUSIC_TILE_GENRES:
		mode = MODE_NAMES;
		kind = LIBRARY_LIST_GENRES;
		break;
	case MUSIC_TILE_YEARS:
		mode = MODE_NAMES;
		kind = LIBRARY_LIST_YEARS;
		break;
	case MUSIC_TILE_PLAYLISTS:
		mode = MODE_PLAYLISTS;
		break;
	case MUSIC_TILE_BROWSE:
	default:
		mode = MODE_BROWSE;
		break;
	}

	bool show_strip = true;
	if (mode == MODE_PLAYLISTS) {
		lv_image_set_src(strip_icon, &icon_list_plus);
		lv_label_set_text(strip_label, tr("pixellib_manage_playlists"));
		pl_n = 0;
		playlists_for_each(pl_collect, NULL);
	} else if (mode == MODE_BROWSE) {
		lv_image_set_src(strip_icon, &icon_folder);
		lv_label_set_text(strip_label, tr("pixellib_open_folders"));
	} else {
		lv_image_set_src(strip_icon, &icon_shuffle);
		lv_label_set_text(strip_label, tr("pixellib_shuffle"));
		ix_kind = kind;
		// In the order the ordinary list for it is sorted in.
		library_order_t order = LIBRARY_ORDER_DEFAULT;
		bool desc = false;
		medialist_list_order(kind, LIBRARY_FILTER_NONE, &order, &desc);
		ix = library_index_open(kind, LIBRARY_FILTER_NONE, NULL, order, desc);
		count = ix ? library_index_count(ix) : 0;
		show_strip = mode != MODE_NAMES && count > 0;
	}
	lv_obj_set_hidden(strip_btn, !show_strip);

	// At the right of the chip: the order of the list, where it has one, and
	// the number of columns on the albums.
	sort_btn = NULL;
	cols_btn = NULL;
	int slot = 0;
	if (ix && medialist_sortable(ix_kind) && (mode == MODE_TRACKS || mode == MODE_NAMES || mode == MODE_ALBUMS)) {
		sort_btn = strip_round(slot++, sort_click_cb);
		lv_obj_t *icon = lv_image_create(sort_btn);
		lv_image_set_src(icon, medialist_sort_icon(ix_kind));
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_image_set_scale(icon, 200);
		lv_obj_center(icon);
	}
	if (mode == MODE_ALBUMS) {
		cols_btn = strip_round(slot++, cols_click_cb);
		lv_obj_add_event_cb(cols_btn, cols_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);
	}

	int total = items_total();
	lv_obj_set_hidden(empty_label, !(mode != MODE_BROWSE && total == 0));

	if (mode == MODE_ALBUMS) {
		pitch = card_h + CARD_GAP;
	} else {
		pitch = ROW_H + ROW_GAP;
	}
	int rows = mode == MODE_ALBUMS ? (total + cols - 1) / cols : total;
	lv_obj_set_height(body, mode == MODE_BROWSE ? STRIP_H : STRIP_H + rows * pitch + pad);

	if (mode != MODE_BROWSE && total > 0) {
		int view_h = lv_obj_get_height(list);
		int per = mode == MODE_ALBUMS ? cols : 1;
		int want = (view_h / pitch + 2) * per;
		pool_n = want < POOL ? want : POOL;
		for (int i = 0; i < pool_n; i++) {
			if (mode == MODE_ALBUMS) {
				make_card(&pool[i]);
			} else {
				make_row(&pool[i]);
			}
		}
	}
	window_update();
}

static void tabs_paint(void) {
	for (int i = 0; i < tab_n; i++) {
		bool on = tab_tiles[i] == current_tile;
		lv_obj_set_style_bg_color(tab_btns[i], on ? theme()->accent : theme()->surface, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(tab_btns[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

static void tab_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	music_tile_t t = (music_tile_t)(intptr_t)lv_event_get_user_data(e);
	if (t == current_tile) {
		return;
	}
	current_tile = t;
	config_set_int("library", "pixel_tab", (int)t);
	config_save();
	tabs_paint();
	lv_obj_scroll_to_view(lv_event_get_current_target(e), LV_ANIM_ON);
	content_build();
}

void pixellibrary_refresh_tabs(void) {
	if (!tabs) {
		return;
	}
	lv_obj_clean(tabs);
	tab_n = music_tiles_in_use_count();
	bool current_there = false;
	for (int i = 0; i < tab_n; i++) {
		music_tile_t t = music_tiles_in_use_at(i);
		tab_tiles[i] = t;
		current_there |= t == current_tile;
		lv_obj_t *b = lv_button_create(tabs);
		lv_obj_remove_style_all(b);
		lv_obj_set_size(b, LV_SIZE_CONTENT, TAB_H);
		lv_obj_set_style_radius(b, TAB_H / 2, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(b, 20, 0);
		lv_obj_set_event_bubble(b, true);
		lv_obj_add_event_cb(b, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)t);
		lv_obj_t *l = lv_label_create(b);
		lv_label_set_text(l, tr(music_tile_tag(t)));
		lv_obj_set_style_text_font(l, &font_ui_18, 0);
		lv_obj_center(l);
		tab_btns[i] = b;
	}
	// The tab that was open may have just been taken off: the first one then.
	if (!current_there && tab_n > 0) {
		current_tile = tab_tiles[0];
	}
	tabs_paint();
	if (shown) {
		content_build();
	}
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static lv_obj_t *head_button(lv_obj_t *parent, int slot, const lv_image_dsc_t *icon) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, HEAD_BTN, HEAD_BTN);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, theme()->surface, 0);
	lv_obj_set_style_bg_color(b, theme()->surface_pressed, LV_STATE_PRESSED);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -pad - slot * (HEAD_BTN + 8), (HEAD_H - HEAD_BTN) / 2);
	lv_obj_t *i = lv_image_create(b);
	lv_image_set_src(i, icon);
	lv_obj_add_style(i, &theme_style_icon, 0);
	lv_image_set_scale(i, 200);
	lv_obj_center(i);
	return b;
}

// The editor's screen is made with the music settings, after this page: it is
// looked up when the button is pressed, not when the button is made.
static void arrange_cb(lv_event_t *e) {
	(void)e;
	if (musictiles_screen && !player_sheet_drag_active() && !switcher_back_drag_active()) {
		switch_screen(musictiles_screen);
	}
}

static void search_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (!shown) {
		return;
	}
	// Nothing held while the page is not on show: the pictures and the list
	// are made again when it comes back.
	pool_clear();
	lv_obj_clean(body);
	list_close();
	mode = MODE_NONE;
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (shown && mode == MODE_NONE) {
		content_build();
	}
}

// Light to dark or back: the colours set on the widgets themselves are set
// again, and the cards and rows are made again in the new ones.
static void theme_changed(void) {
	if (!root) {
		return;
	}
	lv_obj_set_style_text_color(heading, theme()->text_primary, 0);
	lv_obj_set_style_text_color(empty_label, theme()->text_secondary, 0);
	for (int i = 0; i < 3; i++) {
		if (head_btns[i]) {
			lv_obj_set_style_bg_color(head_btns[i], theme()->surface, 0);
			lv_obj_set_style_bg_color(head_btns[i], theme()->surface_pressed, LV_STATE_PRESSED);
		}
	}
	tabs_paint();
	if (shown && mode != MODE_NONE) {
		content_build();
	}
}

void pixellibrary_create(lv_obj_t *screen, gui_config_t *cfg) {
	cfg_saved = cfg;
	scr_w = (int)cfg->screen_width;
	pad = cfg->padding;
	grid_geometry();
	pl_names = calloc(PLAYLISTS_MAX, sizeof(pl_names[0]));
	pl_counts = calloc(PLAYLISTS_MAX, sizeof(int));
	int saved = (int)config_get_int("library", "pixel_tab", MUSIC_TILE_ALBUMS);
	current_tile = saved >= 0 && saved < MUSIC_TILE_COUNT ? (music_tile_t)saved : MUSIC_TILE_ALBUMS;

	int top = cfg->top_bar_height;
	root = lv_obj_create(screen);
	lv_obj_remove_style_all(root);
	lv_obj_add_style(root, &theme_style_screen, 0);
	lv_obj_set_size(root, scr_w, (int)cfg->screen_height - top);
	lv_obj_set_pos(root, 0, top);
	lv_obj_set_scrollable(root, false);
	lv_obj_set_hidden(root, true);

	// "Library", and three round buttons: arrange the tabs, search, settings.
	lv_obj_t *head = plain(root);
	lv_obj_set_size(head, scr_w, HEAD_H);
	heading = lv_label_create(head);
	lv_label_set_text(heading, tr("pixellib_title"));
	lv_obj_set_style_text_font(heading, &font_ui_36_bold, 0);
	lv_obj_set_style_text_color(heading, theme()->text_primary, 0);
	lv_obj_align(heading, LV_ALIGN_LEFT_MID, pad, 0);
	lv_obj_t *settings_btn = head_button(head, 0, &icon_music_settings);
	head_btns[0] = settings_btn;
	lv_obj_add_event_cb(settings_btn, switch_screen_cb, LV_EVENT_CLICKED, musicsettings_screen);
	lv_obj_t *search_btn = head_button(head, 1, &icon_search);
	head_btns[1] = search_btn;
	lv_obj_add_event_cb(search_btn, search_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *arrange_btn = head_button(head, 2, &icon_arrow_down_up);
	head_btns[2] = arrange_btn;
	lv_obj_add_event_cb(arrange_btn, arrange_cb, LV_EVENT_CLICKED, NULL);

	// The tabs, sideways.
	tabs = lv_obj_create(root);
	lv_obj_remove_style_all(tabs);
	lv_obj_set_size(tabs, scr_w, TABS_H);
	lv_obj_set_pos(tabs, 0, HEAD_H);
	lv_obj_set_style_pad_hor(tabs, pad, 0);
	lv_obj_set_style_pad_column(tabs, 8, 0);
	lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(tabs, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(tabs, LV_SCROLLBAR_MODE_OFF);

	// The list under them.
	int list_y = HEAD_H + TABS_H + 4;
	list = lv_obj_create(root);
	lv_obj_remove_style_all(list);
	lv_obj_set_size(list, scr_w, lv_obj_get_height(root) - list_y);
	lv_obj_set_pos(list, 0, list_y);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);
	// The pull from the bottom edge into the album carousel, as on the tiles.
	coverflow_attach_edge(list, cfg);
	body = plain(list);
	lv_obj_set_size(body, scr_w, STRIP_H);
	lv_obj_set_event_bubble(body, true);

	empty_label = lv_label_create(list);
	lv_label_set_text(empty_label, tr("pixellib_empty"));
	lv_obj_set_style_text_font(empty_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(empty_label, theme()->text_secondary, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_MID, 0, STRIP_H + 40);
	lv_obj_set_hidden(empty_label, true);

	cover_poll = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(cover_poll);

	lv_obj_add_event_cb(screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	pixellibrary_refresh_tabs();
	theme_register_refresh(theme_changed);
}

void pixellibrary_show(bool on) {
	if (!root) {
		return;
	}
	if (on == shown) {
		if (on && mode == MODE_NONE) {
			content_build();
		}
		return;
	}
	shown = on;
	lv_obj_set_hidden(root, !on);
	// The heading sits where the chevron would: the page is left with the
	// swipe back instead, as from the home screen's own pages.
	back_btn_hide_on_screen(lv_obj_get_parent(root), on);
	if (on) {
		lv_obj_move_foreground(root);
		content_build();
	} else {
		pool_clear();
		lv_obj_clean(body);
		list_close();
		mode = MODE_NONE;
	}
}
