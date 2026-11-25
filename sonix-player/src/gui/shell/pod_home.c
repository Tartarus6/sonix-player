#include "pod_home.h"

#include <stdbool.h>
#include <stddef.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/ios_home_anim.h"
#include "src/gui/shell/pod_layout.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/lang.h"

// ---------------------------------------------------------------------------
// Visual constants
//
// These mirror the GRID_GAP and TILE_RADIUS values in gridpage.c to keep the
// Pod home screen visually consistent with the Classic home screen tiles.
// ---------------------------------------------------------------------------

#define POD_GRID_GAP    14   /* px between grid cells and dock slots        */
#define POD_TILE_RADIUS 12   /* Adwaita card radius, same as gridpage.c     */
#define POD_TILE_PAD     8   /* padding inside each tile card               */
#define POD_ICON_GAP     8   /* vertical gap between icon and label         */
#define POD_DIVIDER_H    1   /* height of the grid/dock separator line      */
#define POD_ART_MAX    112   /* largest app artwork, px (source is 128)     */

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

lv_obj_t *pod_home_screen = NULL;

// Retained so pod_home_refresh() can reach them without searching the tree.
static lv_obj_t *grid_container  = NULL;
static lv_obj_t *dock_container  = NULL;
static gui_config_t *saved_cfg   = NULL;

// ---------------------------------------------------------------------------
// Event helpers
// ---------------------------------------------------------------------------

// Fired when the user taps an occupied tile. The app ID is stored as the
// event's integer user data via lv_event_get_user_data(), cast to intptr_t.
static void tile_tap_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // swipe, not a tap
	}

	pod_app_t app = (pod_app_t)(intptr_t)lv_event_get_user_data(e);

	void (*action)(void) = pod_app_action(app);
	if (action) {
		action();
		return;
	}

	lv_obj_t **target = pod_app_target(app);
	if (target && *target) {
		switch_screen(*target);
	}
}

// ---------------------------------------------------------------------------
// Tile construction
// ---------------------------------------------------------------------------

// Builds one tile card inside `parent` at the given fixed size.  When `app`
// is POD_APP_NONE the slot is a transparent placeholder.
static void build_tile(lv_obj_t *parent, pod_app_t app, int tile_w, int tile_h) {
	lv_obj_t *tile = lv_btn_create(parent);
	lv_obj_set_size(tile, tile_w, tile_h);
	lv_obj_set_style_shadow_width(tile, 0, 0);
	lv_obj_set_style_pad_all(tile, POD_TILE_PAD, 0);
	lv_obj_set_style_radius(tile, POD_TILE_RADIUS, 0);
	lv_obj_set_scrollable(tile, false);

	// Presses bubble up so a swipe that starts on a tile still reaches the
	// grid container (and from there the player-sheet drag handler).
	lv_obj_set_event_bubble(tile, true);

	lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
	                       LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(tile, POD_ICON_GAP, 0);

	if (app == POD_APP_NONE) {
		// Empty slot: fully transparent card, no interaction.
		lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(tile, 0, 0);

		// Placeholder children keep the flex layout stable so grid dimensions
		// do not collapse when all slots in a row are empty.
		lv_obj_t *dummy_img = lv_image_create(tile);
		lv_image_set_src(dummy_img, NULL);
		lv_obj_set_hidden(dummy_img, true);

		lv_obj_t *dummy_lbl = lv_label_create(tile);
		lv_label_set_text(dummy_lbl, "");
		lv_obj_set_hidden(dummy_lbl, true);
		return;
	}

	// Occupied slot, iPod touch style: no card behind the tile, just the app
	// artwork and the name underneath. The tile dims while pressed.
	lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(tile, 0, 0);
	lv_obj_set_style_opa(tile, LV_OPA_60, LV_STATE_PRESSED);

	// The label gets exactly one line; the artwork takes the rest, capped so
	// it stays square and never grows past the tile width.
	int label_h = lv_font_get_line_height(&font_ui_18);
	int inner_w = tile_w - 2 * POD_TILE_PAD;
	int inner_h = tile_h - 2 * POD_TILE_PAD - POD_ICON_GAP - label_h;
	int art = inner_w < inner_h ? inner_w : inner_h;
	if (art > POD_ART_MAX) {
		art = POD_ART_MAX;
	}
	if (art < 16) {
		art = 16;
	}

	// The icon_menu_* pictures are full-colour artwork (the same ones the
	// Classic menu shows), so they are drawn as they are: no recolour.
	// CONTAIN scales the 128 px picture into the box without distortion.
	lv_obj_t *icon_obj = lv_image_create(tile);
	lv_image_set_src(icon_obj, pod_app_icon(app));
	lv_obj_set_size(icon_obj, art, art);
	lv_image_set_inner_align(icon_obj, LV_IMAGE_ALIGN_CONTAIN);
	lv_obj_set_clickable(icon_obj, false);
	lv_obj_set_event_bubble(icon_obj, true);

	lv_obj_t *lbl = lv_label_create(tile);
	lv_label_set_text(lbl, tr(pod_app_label(app)));
	lv_obj_add_style(lbl, &theme_style_text, 0);
	lv_obj_set_style_text_font(lbl, &font_ui_18, 0);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	// One line, like iOS: a fixed width AND height make LONG_DOT end a long
	// translated name in "…" instead of wrapping it onto a second line and
	// pushing the artwork out of the tile.
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_size(lbl, inner_w, label_h);

	// Tap handler.  Store the app ID as a pointer-sized integer so no heap
	// allocation is needed and the pointer stays valid for the life of the
	// screen.
	lv_obj_add_event_cb(tile, tile_tap_cb, LV_EVENT_CLICKED,
	                    (void *)(intptr_t)app);
}

// ---------------------------------------------------------------------------
// Container construction (called once from pod_home_create)
// ---------------------------------------------------------------------------

// Computes tile dimensions from the available area and builds an
// N-slot flex container.  Returns the flex container.
static lv_obj_t *build_flex_row(lv_obj_t *parent, int area_w, int area_h,
                                 int columns, int rows) {
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_set_size(cont, area_w, area_h);
	lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cont, 0, 0);
	lv_obj_set_style_radius(cont, 0, 0);
	lv_obj_set_style_pad_all(cont, 0, 0);
	lv_obj_set_style_pad_gap(cont, POD_GRID_GAP, 0);
	lv_obj_set_scrollable(cont, false);

	lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
	// START on the main axis so an odd number of slots aligns to the left edge,
	// matching gridpage.c's deliberate choice for the same reason.
	lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
	                       LV_FLEX_ALIGN_START);

	int tile_w = (area_w - (columns - 1) * POD_GRID_GAP) / columns;
	int tile_h = (area_h - (rows    - 1) * POD_GRID_GAP) / rows;

	int total = columns * rows;
	for (int i = 0; i < total; i++) {
		build_tile(cont, POD_APP_NONE, tile_w, tile_h);
	}

	return cont;
}

// ---------------------------------------------------------------------------
// pod_home_create
// ---------------------------------------------------------------------------

void pod_home_create(lv_obj_t *parent, gui_config_t *cfg) {
	saved_cfg = cfg;

	// An opaque, full-screen layer over the Classic tiles. It covers them and
	// takes every touch, so hiding or showing it is the whole theme switch.
	pod_home_screen = lv_obj_create(parent);
	lv_obj_remove_style_all(pod_home_screen);
	lv_obj_add_style(pod_home_screen, &theme_style_screen, 0);
	lv_obj_set_size(pod_home_screen, (int)cfg->screen_width, (int)cfg->screen_height);
	lv_obj_set_pos(pod_home_screen, 0, 0);
	lv_obj_set_scrollable(pod_home_screen, false);

	int top     = cfg->top_bar_height;
	int pad     = cfg->padding;
	int scr_w   = (int)cfg->screen_width;
	int scr_h   = (int)cfg->screen_height;

	// Total vertical space below the status bar.
	int total_h = scr_h - top;

	// Split the vertical space: grid takes the larger share; dock gets just
	// enough for one row of equal tiles.  With the standard padding and gap the
	// dock tiles end up the same height as the grid tiles, since grid uses a
	// 3×3 layout and the dock uses 1×1.
	//
	// Layout (from the status bar downwards):
	//   [pad] [grid_h] [pad/2] [divider] [pad/2] [dock_h] [pad]
	//
	// dock_h: one tile row.  For a 3×3 grid the natural tile size is
	//   tile_h = (total_h - 2*pad - divider - pad - 3*gap) / (3+1)
	// which simplifies to equal grid tiles and dock tiles when cols==rows==3.
	//
	// In practice we let LVGL size the containers and just reserve a fixed
	// amount for the dock area so it always stays proportional.

	int dock_area_h = (total_h - 2 * pad - POD_DIVIDER_H - 2 * (pad / 2)) / 4;
	int grid_area_h = total_h - 2 * pad - POD_DIVIDER_H - (pad / 2) * 2 - dock_area_h;
	int area_w      = scr_w - 2 * pad;

	// ---- 3×3 grid container ----
	grid_container = build_flex_row(pod_home_screen, area_w, grid_area_h, 3, 3);
	lv_obj_align(grid_container, LV_ALIGN_TOP_LEFT, pad, top + pad);

	// ---- divider ----
	lv_obj_t *divider = lv_obj_create(pod_home_screen);
	lv_obj_set_size(divider, scr_w, POD_DIVIDER_H);
	lv_obj_align_to(divider, grid_container, LV_ALIGN_OUT_BOTTOM_LEFT, -pad,
	                pad / 2);
	lv_obj_set_style_bg_color(divider, theme()->text_secondary, 0);
	lv_obj_set_style_bg_opa(divider, LV_OPA_30, 0);
	lv_obj_set_style_border_width(divider, 0, 0);
	lv_obj_set_style_radius(divider, 0, 0);
	lv_obj_set_style_pad_all(divider, 0, 0);
	lv_obj_set_scrollable(divider, false);

	// ---- dock container (3 slots in a single row) ----
	dock_container = build_flex_row(pod_home_screen, area_w, dock_area_h, 3, 1);
	// The divider spans the full screen width from x = 0, so shift the dock by
	// `pad` to give it the same side margins as the grid.
	lv_obj_align_to(dock_container, divider, LV_ALIGN_OUT_BOTTOM_LEFT, pad,
	                pad / 2);

	// iOS dock: a rounded shelf behind the three slots instead of a line.
	// The divider stays in the tree only to anchor the dock's position. The
	// card style follows dark/light changes on its own.
	lv_obj_set_hidden(divider, true);
	// build_flex_row() set a local transparent background, and local styles
	// beat added ones: drop it, or the card style never shows.
	lv_obj_remove_local_style_prop(dock_container, LV_STYLE_BG_OPA, 0);
	lv_obj_add_style(dock_container, &theme_style_card, 0);
	lv_obj_set_style_radius(dock_container, POD_TILE_RADIUS * 2, 0);
	lv_obj_set_style_pad_all(dock_container, 0, 0);
	lv_obj_set_style_border_width(dock_container, 0, 0);
	lv_obj_set_style_shadow_width(dock_container, 0, 0);

	// The player sheet can be pulled in from the grid and the dock, just as it
	// can from every tiled page.
	player_sheet_attach_drag(grid_container, true);
	player_sheet_attach_drag(dock_container, true);

	// Swipe-back also works from the grid/dock surfaces.
	switcher_attach_back_gesture(grid_container);
	switcher_attach_back_gesture(dock_container);

	pod_home_refresh();
	pod_home_apply_theme();
}

// ---------------------------------------------------------------------------
// The arrival after the screensaver
// ---------------------------------------------------------------------------

static ios_home_anim_controller_t enter_anim;
static lv_timer_t *enter_timer;
static uint32_t enter_last_ms;

// Lays the controller's present frame over the home: the grid's nine tiles
// each on their own, and the dock as one piece, shelf and icons together, so
// the shelf rises with what stands on it.
static void enter_apply_one(lv_obj_t *o, const ios_icon_anim_t *it, bool clear) {
	if (clear) {
		// Hand it back to its own style.
		lv_obj_remove_local_style_prop(o, LV_STYLE_OPA, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSLATE_X, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSLATE_Y, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_X, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_SCALE_Y, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_PIVOT_X, 0);
		lv_obj_remove_local_style_prop(o, LV_STYLE_TRANSFORM_PIVOT_Y, 0);
		return;
	}
	lv_obj_set_style_transform_pivot_x(o, lv_obj_get_width(o) / 2, 0);
	lv_obj_set_style_transform_pivot_y(o, lv_obj_get_height(o) / 2, 0);
	lv_obj_set_style_translate_x(o, it->current_x, 0);
	lv_obj_set_style_translate_y(o, it->current_y, 0);
	lv_obj_set_style_transform_scale(o, (int32_t)(it->current_scale * 256.0f), 0);
	lv_obj_set_style_opa(o, it->current_opacity, 0);
}

static void enter_apply(bool clear) {
	if (grid_container) {
		int n = (int)lv_obj_get_child_count(grid_container);
		for (int i = 0; i < n && i < GRID_ICON_COUNT; i++) {
			enter_apply_one(lv_obj_get_child(grid_container, i), &enter_anim.items[i], clear);
		}
	}
	if (dock_container) {
		enter_apply_one(dock_container, &enter_anim.items[GRID_ICON_COUNT], clear);
	}
}

static void enter_overflow(bool visible) {
	// The icons come from outside their own container, which would clip them.
	if (grid_container) {
		lv_obj_set_overflow_visible(grid_container, visible);
	}
	if (dock_container) {
		lv_obj_set_overflow_visible(dock_container, visible);
	}
}

static void enter_tick_cb(lv_timer_t *timer) {
	uint32_t now = lv_tick_get();
	float dt = (float)(now - enter_last_ms) / 1000.0f;
	enter_last_ms = now;
	if (dt > 0.05f) {
		dt = 0.05f; // a stall must not skip the flight
	}
	bool playing = ios_home_anim_update(&enter_anim, dt);
	enter_apply(false);
	if (!playing) {
		enter_apply(true);
		enter_overflow(false);
		lv_timer_pause(timer);
	}
}

void pod_home_play_enter(void) {
	if (!pod_home_screen || lv_obj_is_hidden(pod_home_screen) ||
	    lv_screen_active() != lv_obj_get_parent(pod_home_screen)) {
		return;
	}
	if (!enter_timer) {
		enter_timer = lv_timer_create(enter_tick_cb, 16, NULL);
	}
	ios_home_anim_init(&enter_anim, (int16_t)lv_obj_get_width(pod_home_screen),
	                   (int16_t)lv_obj_get_height(pod_home_screen));
	ios_home_anim_trigger_unlock(&enter_anim);
	enter_overflow(true);
	enter_apply(false); // hidden before the first frame, not after it
	enter_last_ms = lv_tick_get();
	lv_timer_resume(enter_timer);
}

void pod_home_apply_theme(void) {
	if (!pod_home_screen) {
		return;
	}
	bool pod = home_theme_get() == HOME_THEME_POD;
	lv_obj_set_hidden(pod_home_screen, !pod);
	if (pod) {
		lv_obj_move_foreground(pod_home_screen);
	}
}

// ---------------------------------------------------------------------------
// pod_home_refresh
// ---------------------------------------------------------------------------

// Repaints one container (grid or dock) from an array of app IDs.  Iterates
// the existing tile children in order and either recreates them in place or
// updates the icon/label.  Recreating is simpler and safe because LVGL
// garbage-collects deleted child objects immediately.
static void refresh_container(lv_obj_t *cont, const pod_app_t *apps, int count,
                               int columns, int rows) {
	if (!cont || !saved_cfg) {
		return;
	}

	int area_w  = (int)lv_obj_get_width(cont);
	int area_h  = (int)lv_obj_get_height(cont);
	int tile_w  = (area_w - (columns - 1) * POD_GRID_GAP) / columns;
	int tile_h  = (area_h - (rows    - 1) * POD_GRID_GAP) / rows;

	// Remove all existing tiles and rebuild from scratch.  This is the
	// approach gridpage_set_entries() uses and avoids partial-update edge
	// cases when visibility changes alter which slots are occupied.
	lv_obj_clean(cont);

	for (int i = 0; i < count; i++) {
		build_tile(cont, apps[i], tile_w, tile_h);
	}
}

void pod_home_refresh(void) {
	if (!pod_home_screen) {
		return;
	}

	// Collect the current grid state.
	pod_app_t grid_apps[POD_GRID_SLOTS];
	for (int i = 0; i < POD_GRID_SLOTS; i++) {
		grid_apps[i] = pod_layout_grid_at(i);
	}

	// Collect the current dock state.
	pod_app_t dock_apps[POD_DOCK_SLOTS];
	for (int i = 0; i < POD_DOCK_SLOTS; i++) {
		dock_apps[i] = pod_layout_dock_at(i);
	}

	refresh_container(grid_container, grid_apps, POD_GRID_SLOTS, 3, 3);
	refresh_container(dock_container, dock_apps, POD_DOCK_SLOTS, 3, 1);
}
