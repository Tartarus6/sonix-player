#include "gearboypage.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/utils.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/core/lang.h"

lv_obj_t *gearboypage_screen;

static lv_obj_t *list_box;
static lv_obj_t *status_label;
static lv_obj_t *btn_gb;
static lv_obj_t *btn_gbc;

// Which of the two pills is selected. ROMs live in two folders (Games/GB and
// Games/GBC) for two different machines, so they are shown as two lists rather
// than one list needing a subtitle on every entry.
static bool show_color;

// The list the rows are showing. Rows carry the index, not the pointer, so the
// array can be freed and rebuilt without a surviving row dangling.
static gbrom_t *roms;
static int rom_count;

// Bumped on every scan. The thread carries the generation it started with and
// compares it on return: if the page was closed and reopened meanwhile, its
// result is stale and gets dropped instead of overwriting the newer one.
static uint32_t scan_generation;
static bool scanning;
static bool page_visible;

#define COVER_SIZE 72

typedef struct {
	int rom_index;        // index into `roms`, or -1
	lv_obj_t *button;
	lv_obj_t *img;
	lv_obj_t *placeholder;
	lv_obj_t *label;
	char cover_path[GBROM_PATH_MAX];
	cover_image_t cover;
	bool has_cover;
} rom_row_t;

static rom_row_t *rows;
static int row_count;

static uint32_t cover_generation;

typedef struct {
	uint32_t generation;
	int row_index;
	cover_image_t cover;
	bool found;
} cover_result_t;

typedef struct {
	uint32_t generation;
	int count;
	char (*paths)[GBROM_PATH_MAX];
} cover_job_t;

static void free_rows(bool widgets_alive) {
	for (int i = 0; i < row_count; i++) {
		if (rows[i].has_cover) {
			// Stop using pixels before freeing them, but only when the LVGL
			// objects still exist (they may already be gone after lv_obj_clean).
			if (widgets_alive && rows[i].img) {
				lv_image_set_src(rows[i].img, NULL);
			}
			cover_free(&rows[i].cover);
			rows[i].has_cover = false;
		}
	}
	free(rows);
	rows = NULL;
	row_count = 0;
}

static void stop_covers(void) { cover_generation++; }

typedef struct {
	uint32_t generation;
	gbrom_t *list;
	int count;
} scan_result_t;

static void open_rom_cb(lv_event_t *e) {
	// The release of a back swipe lands on whichever row is under the finger:
	// without this, swiping back from here would launch a game.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= rom_count || !roms) {
		return;
	}
	gearboyplay_open(roms[index].path, roms[index].name);
}

static bool path_is_file(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Finds cover art for a ROM: <rom dir>/Cover Art/<rom basename>.<ext>, as
// suggested in the feature request. Basename comes from the ROM file, not the
// database title.
static bool find_rom_cover(const gbrom_t *rom, char *out, size_t out_size) {
	if (!rom || !rom->path[0] || !out || out_size == 0) {
		return false;
	}

	char dir[GBROM_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", rom->path);
	char *slash = strrchr(dir, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';

	const char *base = strrchr(rom->path, '/');
	base = base ? base + 1 : rom->path;
	char stem[GBROM_PATH_MAX];
	if (snprintf(stem, sizeof(stem), "%s", base) >= (int)sizeof(stem)) {
		return false;
	}
	char *dot = strrchr(stem, '.');
	if (dot) {
		*dot = '\0';
	}
	if (!stem[0]) {
		return false;
	}

	static const char *const exts[] = {"png", "jpg", "jpeg", "PNG", "JPG", "JPEG"};
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		int n = snprintf(out, out_size, "%s/Cover Art/%s.%s", dir, stem, exts[i]);
		if (n < 0 || (size_t)n >= out_size) {
			continue;
		}
		if (path_is_file(out)) {
			return true;
		}
	}
	out[0] = '\0';

	// Case-insensitive fallback for the folder/file name on filesystems where the
	// exact spelling differs.
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	char cover_dir_name[256] = "";
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (strcasecmp(de->d_name, "Cover Art") == 0) {
			snprintf(cover_dir_name, sizeof(cover_dir_name), "%s", de->d_name);
			break;
		}
	}
	closedir(d);
	if (!cover_dir_name[0]) {
		return false;
	}

	char cdir[GBROM_PATH_MAX + 256 + 2];
	if (snprintf(cdir, sizeof(cdir), "%s/%s", dir, cover_dir_name) >= (int)sizeof(cdir)) {
		return false;
	}
	d = opendir(cdir);
	if (!d) {
		return false;
	}
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		const char *edot = strrchr(de->d_name, '.');
		if (!edot) {
			continue;
		}
		const char *eext = edot + 1;
		if (strcasecmp(eext, "png") != 0 && strcasecmp(eext, "jpg") != 0 && strcasecmp(eext, "jpeg") != 0) {
			continue;
		}
		size_t nlen = (size_t)(edot - de->d_name);
		if (nlen == strlen(stem) && strncasecmp(de->d_name, stem, nlen) == 0) {
			int n = snprintf(out, out_size, "%s/%s", cdir, de->d_name);
			if (n >= 0 && (size_t)n < out_size && path_is_file(out)) {
				closedir(d);
				return true;
			}
		}
	}
	closedir(d);
	out[0] = '\0';
	return false;
}

static void row_show_placeholder(int row_index) {
	if (row_index < 0 || row_index >= row_count) {
		return;
	}
	rom_row_t *row = &rows[row_index];
	if (row->img) {
		lv_obj_set_hidden(row->img, true);
	}
	if (row->placeholder) {
		lv_obj_set_hidden(row->placeholder, false);
	}
}

static void cover_ready_cb(void *user) {
	cover_result_t *res = user;
	if (!res) {
		return;
	}
	if (res->generation == cover_generation && res->row_index >= 0 && res->row_index < row_count) {
		rom_row_t *row = &rows[res->row_index];
		if (res->found) {
			if (row->has_cover) {
				lv_image_set_src(row->img, NULL);
				cover_free(&row->cover);
			}
			row->cover = res->cover;
			row->has_cover = true;
			lv_image_set_src(row->img, &row->cover.dsc);
			lv_obj_set_hidden(row->img, false);
			lv_obj_set_hidden(row->placeholder, true);
		} else {
			row_show_placeholder(res->row_index);
		}
	} else if (res->found) {
		cover_free(&res->cover);
	}
	free(res);
}

static void *cover_thread(void *arg) {
	cover_job_t *job = arg;
	thread_be_background("gbcovers");

	for (int i = 0; i < job->count; i++) {
		if (job->generation != cover_generation) {
			break;
		}
		if (!job->paths[i][0]) {
			continue;
		}

		cover_result_t *res = calloc(1, sizeof(*res));
		if (!res) {
			continue;
		}
		res->generation = job->generation;
		res->row_index = i;
		res->found = cover_load_image_file(job->paths[i], COVER_SIZE, COVER_SIZE, COVER_FIT_COVER, &res->cover);
		if (!res->found) {
			memset(&res->cover, 0, sizeof(res->cover));
		}

		if (job->generation != cover_generation || !gui_post(cover_ready_cb, res)) {
			if (res->found) {
				cover_free(&res->cover);
			}
			free(res);
		}
	}

	free(job->paths);
	free(job);
	return NULL;
}

static void start_cover_load(void) {
	stop_covers();
	if (row_count <= 0) {
		return;
	}

	cover_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->generation = cover_generation;
	job->count = row_count;
	job->paths = calloc((size_t)row_count, sizeof(job->paths[0]));
	if (!job->paths) {
		free(job);
		return;
	}

	int pending = 0;
	for (int i = 0; i < row_count; i++) {
		if (rows[i].cover_path[0]) {
			snprintf(job->paths[i], GBROM_PATH_MAX, "%s", rows[i].cover_path);
			pending++;
		}
	}
	if (pending == 0) {
		free(job->paths);
		free(job);
		return;
	}

	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&t, &attr, cover_thread, job);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		free(job->paths);
		free(job);
	}
}

static lv_obj_t *make_row(int view_index, int rom_index) {
	const gbrom_t *rom = &roms[rom_index];

	lv_obj_t *btn = lv_btn_create(list_box);
	lv_obj_set_width(btn, lv_pct(100));
	lv_obj_set_height(btn, 100);
	lv_obj_set_style_radius(btn, 12, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, theme()->surface, 0);
	lv_obj_set_style_bg_color(btn, theme()->surface_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_pad_left(btn, 14, 0);
	lv_obj_set_style_pad_right(btn, 18, 0);
	lv_obj_set_style_pad_ver(btn, 10, 0);
	lv_obj_set_style_pad_gap(btn, 14, 0);
	lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(btn, open_rom_cb, LV_EVENT_CLICKED, (void *)(intptr_t)rom_index);

	lv_obj_t *thumb = lv_obj_create(btn);
	lv_obj_set_size(thumb, COVER_SIZE, COVER_SIZE);
	lv_obj_set_style_radius(thumb, 8, 0);
	lv_obj_set_style_bg_color(thumb, theme()->surface_pressed, 0);
	lv_obj_set_style_border_width(thumb, 0, 0);
	lv_obj_set_style_pad_all(thumb, 0, 0);
	lv_obj_set_scrollable(thumb, false);
	lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);

	lv_obj_t *img = lv_image_create(thumb);
	lv_obj_center(img);
	lv_obj_set_hidden(img, true);

	lv_obj_t *ph = lv_image_create(thumb);
	lv_image_set_src(ph, &icon_files_game);
	lv_obj_add_style(ph, &theme_style_icon, 0);
	lv_obj_center(ph);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(rom->name));
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, theme()->text_primary, 0);

	rows[view_index].rom_index = rom_index;
	rows[view_index].button = btn;
	rows[view_index].img = img;
	rows[view_index].placeholder = ph;
	rows[view_index].label = label;
	rows[view_index].has_cover = false;
	memset(&rows[view_index].cover, 0, sizeof(rows[view_index].cover));
	snprintf(rows[view_index].cover_path, sizeof(rows[view_index].cover_path),
			 "%s", rom->cover_path);

	return btn;
}

static void refresh_view_buttons(void) {
	lv_obj_t *const buttons[] = {btn_gb, btn_gbc};
	const bool colour[] = {false, true};

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (!buttons[i]) {
			continue;
		}
		bool on = colour[i] == show_color;
		lv_obj_set_style_bg_color(buttons[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), on ? lv_color_white() : theme()->text_primary, 0);
	}
}

static void rebuild_rows(void) {
	stop_covers();
	lv_obj_clean(list_box);
	free_rows(false);

	int shown = 0;
	for (int i = 0; i < rom_count; i++) {
		if (roms[i].color == show_color) {
			shown++;
		}
	}

	if (shown > 0) {
		rows = calloc((size_t)shown, sizeof(*rows));
		if (!rows) {
			lv_label_set_text(status_label, tr("gearboy_search_failed"));
			lv_obj_set_hidden(status_label, false);
			return;
		}
		row_count = shown;
		int vi = 0;
		for (int i = 0; i < rom_count; i++) {
			if (roms[i].color == show_color) {
				make_row(vi, i);
				vi++;
			}
		}
		lv_obj_set_hidden(status_label, true);
		if (page_visible) {
			start_cover_load();
		}
		return;
	}

	lv_label_set_text(status_label, show_color ? tr("gearboy_empty_note")
											   : tr("gearboy_empty_gb_note"));
	lv_obj_set_hidden(status_label, false);
}

static void pick_view_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	show_color = (bool)(uintptr_t)lv_event_get_user_data(e);
	refresh_view_buttons();
	rebuild_rows();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
}

static lv_obj_t *make_view_choice(lv_obj_t *parent, const char *text, bool color) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 26, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_view_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)color);

	// "GB" and "GBC" arrive as tags ("gearboy_gb"/"gearboy_gbc") and go through
	// tr(): each language maps them to the right capitalization, so the pills
	// never show up lowercase.
	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

// ---------------------------------------------------------------------------
// the scan
// ---------------------------------------------------------------------------
//
// On its own thread because the first pass reads every ROM end to end to
// compute its CRC32: eight megabytes of cartridge takes well over a second on
// this card, and doing it on the UI thread freezes the page. Later passes are
// answered from the cache and read nothing, but they take the same path.

static void scan_done_cb(void *user) {
	scan_result_t *res = user;

	if (res->generation != scan_generation) {
		// Stale: the page was closed and reopened while the thread worked.
		free(res->list);
		free(res);
		return;
	}

	free(roms);
	roms = res->list;
	rom_count = res->count;
	scanning = false;

	rebuild_rows();
	free(res);
}

static void *scan_thread(void *arg) {
	uint32_t generation = (uint32_t)(uintptr_t)arg;

	gbrom_t *list = NULL;
	int count = gbdb_scan(&list, NULL, NULL);
	if (count < 0) {
		count = 0;
	}

	// Cover discovery stats and reads directories. Do it here, off the UI
	// thread, so switching views or finishing a scan cannot stall the page.
	for (int i = 0; i < count; i++) {
		list[i].cover_path[0] = '\0';
		if (!find_rom_cover(&list[i], list[i].cover_path, sizeof(list[i].cover_path))) {
			list[i].cover_path[0] = '\0';
		}
	}

	scan_result_t *res = malloc(sizeof(*res));
	if (!res) {
		free(list);
		return NULL;
	}
	res->generation = generation;
	res->list = list;
	res->count = count;

	gui_post(scan_done_cb, res);
	return NULL;
}

static void start_scan(void) {
	if (scanning) {
		return;
	}
	scanning = true;
	scan_generation++;

	lv_label_set_text(status_label, tr("gearboy_looking_for_games"));
	lv_obj_set_hidden(status_label, false);

	pthread_t t;
	if (pthread_create(&t, NULL, scan_thread, (void *)(uintptr_t)scan_generation) != 0) {
		scanning = false;
		lv_label_set_text(status_label, tr("gearboy_search_failed"));
		return;
	}
	pthread_detach(t);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = true;
	refresh_view_buttons();
	lv_obj_scroll_to_y(list_box, 0, LV_ANIM_OFF);
	// Rescan on every entry: the card may have been removed, reinserted or
	// filled elsewhere. With a warm cache it costs one directory read.
	start_scan();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
	stop_covers();
}

// The top-right gear, like the one on the Music page: opens Gearboy settings.
static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(gearboysettings_screen);
}

void gearboypage_init(gui_config_t *cfg) {
	lv_obj_add_style(gearboypage_screen, &theme_style_screen, 0);
	lv_obj_t *page_title = settingsrow_title(gearboypage_screen, cfg, "gearboy");
	settingsrow_title_corner_slots(page_title, cfg, 1);

	{
		lv_obj_t *button = lv_btn_create(gearboypage_screen);
		lv_obj_set_size(button, 56, 56);
		lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(button, 0, 0);
		lv_obj_set_style_shadow_width(button, 0, 0);
		lv_obj_set_style_pad_all(button, 0, 0);
		lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
		lv_obj_add_event_cb(button, open_settings_cb, LV_EVENT_CLICKED, NULL);

		lv_obj_t *icon = lv_image_create(button);
		lv_image_set_src(icon, &icon_music_settings);
		lv_obj_add_style(icon, &theme_style_icon, 0);
		lv_obj_center(icon);
	}

	int content_top = settingsrow_content_top(cfg);

	// The pills stay put while only the list below them scrolls. Same
	// construction as the Audiobooks page: a fixed column holding the chooser
	// row and, under it, the list.
	lv_obj_t *container = lv_obj_create(gearboypage_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, cfg->padding, 0);
	lv_obj_set_style_pad_ver(container, 0, 0);
	lv_obj_set_style_pad_gap(container, 12, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	switcher_attach_back_gesture(container);

	lv_obj_t *chooser = lv_obj_create(container);
	lv_obj_set_size(chooser, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(chooser, 0, 0);
	lv_obj_set_style_border_width(chooser, 0, 0);
	lv_obj_set_style_pad_all(chooser, 0, 0);
	lv_obj_set_style_pad_gap(chooser, 10, 0);
	lv_obj_set_scrollable(chooser, false);
	lv_obj_set_flex_flow(chooser, LV_FLEX_FLOW_ROW);

	btn_gb = make_view_choice(chooser, "gearboy_gb", false);
	btn_gbc = make_view_choice(chooser, "gearboy_gbc", true);

	list_box = lv_obj_create(container);
	lv_obj_set_width(list_box, lv_pct(100));
	lv_obj_set_flex_grow(list_box, 1);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_radius(list_box, 0, 0);
	lv_obj_set_style_pad_all(list_box, 0, 0);
	lv_obj_set_style_pad_bottom(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list_box, 10, 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	switcher_attach_back_gesture(list_box);

	status_label = lv_label_create(gearboypage_screen);
	lv_label_set_text(status_label, "");
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(80));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_center(status_label);

	refresh_view_buttons();
	theme_register_refresh(refresh_view_buttons);

	lv_obj_add_event_cb(gearboypage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(gearboypage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(gearboypage_screen);
}
