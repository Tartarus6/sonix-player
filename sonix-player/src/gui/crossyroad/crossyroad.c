#include "crossyroad.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "src/gui/crossyroad/crossysound.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *crossyroad_screen;

// ---------------------------------------------------------------------------
// the world
//
// A port of the web prototype. The world is a grid of GRID_COLS columns and
// endless rows; row 0 is where the animal starts and the camera follows it up.
// Lanes are made when they come into view and kept in a ring, which is more
// than the rows ever on screen.
// ---------------------------------------------------------------------------

#define GRID_COLS 11
#define START_COL 5
#define LANE_RING 64
#define TICK_MS 33
#define MAX_DT 0.05f
#define SWIPE_PX 24

typedef enum { LANE_GRASS, LANE_ROAD, LANE_RIVER } lane_type_t;

typedef struct {
	float x, w; // in columns
	lv_color_t color;
} item_t;

typedef struct {
	bool valid;
	int row;
	lane_type_t type;
	int trees[2];
	int tree_count;
	int coin_col; // a coin lying on the grass, or -1
	item_t items[2];
	float speed; // columns a second, signed
	int dir;
} lane_t;

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } hop_dir_t;

typedef enum { ST_START, ST_PLAYING, ST_PAUSED, ST_GAMEOVER } game_state_t;

typedef struct {
	const char *id;
	const char *name_key;
	uint32_t body, accent, snout;
} character_t;

#define CHAR_COUNT 30
#define SELECTOR_ROW_H 98
#define SELECTOR_H (SELECTOR_ROW_H * 3 + 16 + 40)
static const character_t CHARACTERS[CHAR_COUNT] = {
	{"chicken", "crossy_chicken", 0xffffff, 0xef4444, 0xf59e0b},
	{"frog", "crossy_frog", 0xa3e635, 0x064e3b, 0xfef08a},
	{"pig", "crossy_pig", 0xf9a8d4, 0xec4899, 0xf472b6},
	{"fox", "crossy_fox", 0xf97316, 0xffffff, 0x1e293b},
	{"penguin", "crossy_penguin", 0x1e293b, 0xf8fafc, 0xf59e0b},
	{"duck", "crossy_duck", 0xfacc15, 0xeab308, 0xea580c},
	{"bunny", "crossy_bunny", 0xf1f5f9, 0xf472b6, 0xfb7185},
	{"dog", "crossy_dog", 0xd97706, 0x78350f, 0x1e293b},
	{"unicorn", "crossy_unicorn", 0xfaf5ff, 0xc084fc, 0xfacc15},
	{"cat", "crossy_cat", 0x94a3b8, 0xf9a8d4, 0xfda4af},
	{"panda", "crossy_panda", 0xffffff, 0x111827, 0x374151},
	{"bear", "crossy_bear", 0x92400e, 0x78350f, 0xfde68a},
	{"lion", "crossy_lion", 0xfbbf24, 0x92400e, 0xfde68a},
	{"tiger", "crossy_tiger", 0xf97316, 0x1c1917, 0xffffff},
	{"cow", "crossy_cow", 0xffffff, 0x1f2937, 0xfbcfe8},
	{"monkey", "crossy_monkey", 0x92400e, 0x78350f, 0xfde68a},
	{"koala", "crossy_koala", 0x9ca3af, 0xe5e7eb, 0x1f2937},
	{"mouse", "crossy_mouse", 0xd1d5db, 0xf9a8d4, 0xfb7185},
	{"owl", "crossy_owl", 0x78350f, 0xfef3c7, 0xf59e0b},
	{"sheep", "crossy_sheep", 0xf1f5f9, 0x6b7280, 0xe5e7eb},
	{"horse", "crossy_horse", 0xb45309, 0x451a03, 0xfcd9b6},
	{"raccoon", "crossy_raccoon", 0x9ca3af, 0x374151, 0xffffff},
	{"hedgehog", "crossy_hedgehog", 0x78716c, 0x44403c, 0xfcd9b6},
	{"giraffe", "crossy_giraffe", 0xfacc15, 0xb45309, 0xfde68a},
	{"elephant", "crossy_elephant", 0x9ca3af, 0x6b7280, 0xa8b2c0},
	{"turtle", "crossy_turtle", 0x16a34a, 0x166534, 0x86efac},
	{"robot", "crossy_robot", 0xcbd5e1, 0x64748b, 0xef4444},
	{"alien", "crossy_alien", 0x4ade80, 0x14532d, 0xfde047},
	{"ghost", "crossy_ghost", 0xe2e8f0, 0xa5b4fc, 0x1e293b},
	{"dragon", "crossy_dragon", 0x22c55e, 0x14532d, 0x86efac},
};
#define DEFAULT_CHAR 0 // the chicken

static lane_t lanes[LANE_RING];

static struct {
	float col, row, target_col;
	int target_row;
	float progress;
	hop_dir_t dir;
} player;

static int score, best, max_row;
static int coins_total; // kept between games
static int run_coins;	// picked up in this one
static float camera_row;
static game_state_t state = ST_START;
static bool active;
static bool paused_by_modal;
static bool sound_on;
static bool night; // the night palette, with the cars' lights on
static int selected_char = DEFAULT_CHAR; // the one chosen on the start screen
static int run_char = DEFAULT_CHAR;	  // the one playing now: a coin can change it for the run

// A coin lies on the grass every COIN_EVERY rows. It shows the animal it will
// turn you into, picked at random.
#define COIN_EVERY 25

// The animals are locked: the chicken is free, and each of the others costs
// its place in the list times PRICE_STEP coins. A coin on the road can give one
// of the locked ones away, and that one then stays unlocked.
#define PRICE_STEP 20
static uint32_t unlocked_mask = 1;

static bool char_unlocked(int i) { return i == 0 || ((unlocked_mask >> i) & 1u) != 0; }
static int char_price(int i) { return i * PRICE_STEP; }

static void unlock_save(void) {
	config_set_int("crossyroad", "unlocked", (long)unlocked_mask);
}
static struct {
	bool active;
	int row, col, who;
} coin;
static int64_t last_us;

static int view_w, view_h, tile, col_off;

static lv_obj_t *board;
static lv_timer_t *tick_timer;

static float rnd(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int ring_slot(int row) { return ((row % LANE_RING) + LANE_RING) % LANE_RING; }

static lane_t *lane_peek(int row) {
	lane_t *l = &lanes[ring_slot(row)];
	return l->valid && l->row == row ? l : NULL;
}

static lane_t *lane_make(int row) {
	lane_t *l = &lanes[ring_slot(row)];
	memset(l, 0, sizeof(*l));
	l->valid = true;
	l->row = row;
	l->dir = 1;
	l->coin_col = -1;
	l->type = LANE_GRASS;
	if (row <= 2) {
		return l;
	}

	lane_t *p1 = lane_peek(row - 1);
	lane_t *p2 = lane_peek(row - 2);
	lane_t *p3 = lane_peek(row - 3);

	lane_type_t type;
	float roll = rnd();
	if (p1 && p1->type == LANE_RIVER && p2 && p2->type == LANE_RIVER) {
		type = LANE_GRASS;
	} else if (p1 && p1->type == LANE_ROAD && p2 && p2->type == LANE_ROAD && p3 && p3->type == LANE_ROAD) {
		type = LANE_GRASS;
	} else if (roll < 0.32f) {
		type = LANE_GRASS;
	} else if (roll < 0.72f) {
		type = LANE_ROAD;
	} else {
		type = LANE_RIVER;
	}

	// The difficulty steps up at 50, 100, 200 and 300 rows, and stops there.
	int tier = row >= 300 ? 4 : row >= 200 ? 3 : row >= 100 ? 2 : row >= 50 ? 1 : 0;
	float boost = tier * 0.18f;
	l->type = type;

	if (type == LANE_GRASS) {
		int candidates[GRID_COLS - 1];
		int n = 0;
		for (int c = 0; c < GRID_COLS; c++) {
			if (c != START_COL) {
				candidates[n++] = c;
			}
		}
		for (int i = n - 1; i > 0; i--) {
			int j = (int)(rnd() * (float)(i + 1));
			int t = candidates[i];
			candidates[i] = candidates[j];
			candidates[j] = t;
		}
		for (int i = 0; i < 2; i++) {
			if (rnd() < 0.75f) {
				l->trees[l->tree_count++] = candidates[i];
			}
		}
		// Coins are strewn over the grass; the squares left after the trees are
		// the first ones of the shuffled list that are not trees.
		if (rnd() < 0.35f) {
			for (int i = 0; i < n; i++) {
				bool tree = false;
				for (int t = 0; t < l->tree_count; t++) {
					tree = tree || l->trees[t] == candidates[i];
				}
				if (!tree) {
					l->coin_col = candidates[i];
					break;
				}
			}
		}
		return l;
	}

	if (type == LANE_ROAD) {
		l->dir = rnd() < 0.5f ? 1 : -1;
		if (p1 && p1->type == LANE_ROAD) {
			l->dir = -p1->dir;
		}
		l->speed = (1.45f + rnd() * 0.4f + boost) * (float)l->dir;
		float car_w = 1.65f;
		float gap = fmaxf(4.4f, 5.8f - tier * 0.3f) + rnd() * 1.4f;
		static const uint32_t palette[] = {0xef4444, 0xf59e0b, 0x3b82f6, 0xec4899};
		lv_color_t color = lv_color_hex(palette[(int)(rnd() * 4.0f) & 3]);
		float start = rnd() * 4.0f;
		for (int i = 0; i < 2; i++) {
			l->items[i].x = start + i * (car_w + gap);
			l->items[i].w = car_w;
			l->items[i].color = color;
		}
		return l;
	}

	l->dir = rnd() < 0.5f ? 1 : -1;
	if (p1 && p1->type == LANE_RIVER) {
		l->dir = -p1->dir;
	}
	l->speed = (1.2f + rnd() * 0.35f + boost * 0.7f) * (float)l->dir;
	float log_w = fmaxf(2.6f, 3.2f - tier * 0.12f);
	float gap = 3.8f + rnd() * 1.0f;
	float start = rnd() * 3.0f;
	for (int i = 0; i < 2; i++) {
		l->items[i].x = start + i * (log_w + gap);
		l->items[i].w = log_w;
		l->items[i].color = lv_color_hex(0x92400e);
	}
	return l;
}

static lane_t *lane_get(int row) {
	lane_t *l = lane_peek(row);
	return l ? l : lane_make(row);
}

static int rows_on_screen(void) { return (view_h + tile - 1) / tile + 8; }

// ---------------------------------------------------------------------------
// the screens over the board
// ---------------------------------------------------------------------------

static lv_obj_t *score_value, *best_value, *coins_value, *over_coins;
static lv_obj_t *start_overlay, *over_overlay, *exit_overlay;
static lv_obj_t *over_reason, *over_score, *over_best;
static lv_obj_t *char_buttons[CHAR_COUNT];
static lv_obj_t *char_names[CHAR_COUNT];
static lv_obj_t *buy_overlay, *buy_title, *buy_text, *buy_ok;
static int pending_buy;

static void overlay_show(lv_obj_t *o, bool show) { lv_obj_set_hidden(o, !show); }

static void score_labels_update(void) {
	lv_label_set_text_fmt(score_value, "%d", score);
	lv_label_set_text_fmt(best_value, "%d", best);
	lv_label_set_text_fmt(coins_value, "%d", coins_total);
}

static void board_refresh(void) {
	if (board) {
		lv_obj_invalidate(board);
	}
}

static void init_game(bool show_start) {
	memset(lanes, 0, sizeof(lanes));
	player.col = player.target_col = START_COL;
	player.row = 0;
	player.target_row = 0;
	player.progress = 1.0f;
	player.dir = DIR_UP;
	score = 0;
	run_coins = 0;
	max_row = 0;
	camera_row = 0;
	state = show_start ? ST_START : ST_PLAYING;
	paused_by_modal = false;
	run_char = selected_char;
	coin.active = false;
	last_us = now_us();

	for (int r = -4; r <= rows_on_screen(); r++) {
		lane_get(r);
	}

	score_labels_update();
	overlay_show(over_overlay, false);
	overlay_show(start_overlay, show_start);
	board_refresh();
}

static void game_over(const char *reason_key) {
	if (state == ST_GAMEOVER) {
		return;
	}
	state = ST_GAMEOVER;
	lv_label_set_text(over_reason, tr(reason_key));
	lv_label_set_text_fmt(over_score, "%d", score);
	lv_label_set_text_fmt(over_best, "%d", best);
	lv_label_set_text_fmt(over_coins, tr("crossy_coins_run"), run_coins);
	config_save();
	overlay_show(over_overlay, true);
	board_refresh();
}

// Puts a coin a few rows ahead, on the first grass with a free square, showing
// an animal other than the one you are.
static void coin_spawn(void) {
	int locked[CHAR_COUNT];
	int locked_n = 0;
	for (int i = 0; i < CHAR_COUNT; i++) {
		if (!char_unlocked(i)) {
			locked[locked_n++] = i;
		}
	}
	if (locked_n == 0) {
		return; // nothing left to give away
	}
	for (int r = max_row + 3; r <= max_row + 14; r++) {
		lane_t *lane = lane_get(r);
		if (lane->type != LANE_GRASS) {
			continue;
		}
		int free_cols[GRID_COLS];
		int n = 0;
		for (int c = 0; c < GRID_COLS; c++) {
			bool tree = c == lane->coin_col;
			for (int i = 0; i < lane->tree_count; i++) {
				tree = tree || lane->trees[i] == c;
			}
			if (!tree) {
				free_cols[n++] = c;
			}
		}
		if (n == 0) {
			continue;
		}
		coin.col = free_cols[(int)(rnd() * (float)n) % n];
		coin.row = r;
		coin.who = locked[(int)(rnd() * (float)locked_n) % locked_n];
		coin.active = true;
		return;
	}
}

static void move_player(int d_col, int d_row, hop_dir_t dir) {
	if (state != ST_PLAYING) {
		return;
	}
	int base_col = (int)lroundf(player.target_col);
	int base_row = player.target_row;
	int next_col = base_col + d_col;
	int next_row = base_row + d_row;

	player.dir = dir;

	if (next_col < 0 || next_col >= GRID_COLS) {
		return;
	}
	if (next_row < (int)floorf(camera_row) - 2) {
		return;
	}

	lane_t *target = lane_get(next_row);
	if (target->type == LANE_GRASS) {
		for (int i = 0; i < target->tree_count; i++) {
			if (target->trees[i] == next_col) {
				return;
			}
		}
	}

	player.col = player.target_col;
	player.row = (float)player.target_row;
	player.target_col = (float)next_col;
	player.target_row = next_row;
	player.progress = 0.0f;

	crossysound_play(CROSSY_SFX_HOP);

	if (next_row > max_row) {
		max_row = next_row;
		score = max_row;
		if (score > best) {
			best = score;
			config_set_int("crossyroad", "best", best);
		}
		score_labels_update();
		if (score % COIN_EVERY == 0) {
			coin_spawn();
		}
	}
}

// ---------------------------------------------------------------------------
// one step of the world
// ---------------------------------------------------------------------------

static void update(float dt) {
	int min_row = (int)floorf(camera_row) - 4;
	int max_visible = (int)floorf(camera_row) + rows_on_screen();

	for (int r = min_row; r <= max_visible; r++) {
		lane_t *lane = lane_get(r);
		if (lane->type == LANE_GRASS) {
			continue;
		}
		float span = fmaxf(GRID_COLS + 4, 14);
		for (int i = 0; i < 2; i++) {
			item_t *it = &lane->items[i];
			it->x += lane->speed * dt;
			if (lane->speed > 0 && it->x > GRID_COLS + 1) {
				it->x -= span;
			} else if (lane->speed < 0 && it->x + it->w < -1) {
				it->x += span;
			}
		}
	}

	if (state != ST_PLAYING) {
		return;
	}

	if (player.progress < 1.0f) {
		player.progress = fminf(1.0f, player.progress + dt * 11.0f);
		if (player.progress >= 1.0f) {
			player.col = player.target_col;
			player.row = (float)player.target_row;
		}
	}

	if (coin.active && player.progress >= 0.5f && player.target_row == coin.row &&
		(int)lroundf(player.target_col) == coin.col) {
		run_char = coin.who;
		unlocked_mask |= 1u << coin.who;
		unlock_save();
		coin.active = false;
		crossysound_play(CROSSY_SFX_HOP);
	}
	if (player.progress >= 0.5f) {
		lane_t *here = lane_get(player.target_row);
		if (here->type == LANE_GRASS && here->coin_col == (int)lroundf(player.target_col)) {
			here->coin_col = -1;
			coins_total++;
			run_coins++;
			config_set_int("crossyroad", "coins", coins_total);
			lv_label_set_text_fmt(coins_value, "%d", coins_total);
			crossysound_play(CROSSY_SFX_COIN);
		}
	}
	if (coin.active && coin.row < (int)floorf(camera_row) - 3) {
		coin.active = false; // left behind
	}

	float desired = fmaxf(camera_row, (float)player.target_row - 3.0f);
	camera_row += (desired - camera_row) * fminf(1.0f, dt * 6.0f);

	float cur_col = player.col + (player.target_col - player.col) * player.progress;
	int cur_row = (int)lroundf(player.row + ((float)player.target_row - player.row) * player.progress);
	lane_t *lane = lane_get(cur_row);

	if (lane->type == LANE_ROAD) {
		float left = cur_col + 0.2f, right = cur_col + 0.8f;
		for (int i = 0; i < 2; i++) {
			const item_t *car = &lane->items[i];
			if (right > car->x + 0.08f && left < car->x + car->w - 0.08f) {
				crossysound_play(CROSSY_SFX_CRASH);
				game_over("crossy_death_car");
				return;
			}
		}
	} else if (lane->type == LANE_RIVER && player.progress >= 0.85f) {
		float centre = cur_col + 0.5f;
		bool on_log = false;
		for (int i = 0; i < 2; i++) {
			const item_t *log = &lane->items[i];
			if (centre >= log->x - 0.05f && centre <= log->x + log->w + 0.05f) {
				on_log = true;
				break;
			}
		}
		if (!on_log) {
			crossysound_play(CROSSY_SFX_SPLASH);
			game_over("crossy_death_water");
			return;
		}
		player.col += lane->speed * dt;
		player.target_col += lane->speed * dt;
		if (player.target_col < -0.4f || player.target_col > GRID_COLS - 0.6f) {
			crossysound_play(CROSSY_SFX_SPLASH);
			game_over("crossy_death_swept");
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static lv_layer_t *cur_layer;
static int org_x, org_y;

static lv_color_t hex(uint32_t c) { return lv_color_hex(c); }

// The look of depth is a darker colour for the side that faces the viewer and
// a lighter one for the top, each a single flat rectangle: a few more fills a
// frame and no gradients.
static lv_color_t shade(uint32_t c) { return lv_color_mix(lv_color_black(), hex(c), 70); }
static lv_color_t tint(uint32_t c) { return lv_color_mix(lv_color_white(), hex(c), 60); }
static lv_color_t shade_c(lv_color_t c) { return lv_color_mix(lv_color_black(), c, 70); }
static lv_color_t tint_c(lv_color_t c) { return lv_color_mix(lv_color_white(), c, 60); }

// A colour of the scenery: as it is by day, and pulled toward deep blue at night.
static lv_color_t sc(uint32_t c) { return night ? lv_color_mix(hex(0x0a1233), hex(c), 135) : hex(c); }

static void rect_px(float x, float y, float w, float h, int radius, lv_color_t color, lv_opa_t opa) {
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = color;
	d.bg_opa = opa;
	d.radius = radius;
	int x1 = org_x + (int)floorf(x + 0.5f);
	int y1 = org_y + (int)floorf(y + 0.5f);
	int x2 = org_x + (int)floorf(x + w + 0.5f) - 1;
	int y2 = org_y + (int)floorf(y + h + 0.5f) - 1;
	if (x2 < x1) {
		x2 = x1;
	}
	if (y2 < y1) {
		y2 = y1;
	}
	lv_area_t a = {x1, y1, x2, y2};
	lv_draw_rect(cur_layer, &d, &a);
}

static void outline_px(float x, float y, float w, float h, int radius, lv_color_t color, int width) {
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_opa = LV_OPA_TRANSP;
	d.border_color = color;
	d.border_opa = LV_OPA_COVER;
	d.border_width = width;
	d.radius = radius;
	lv_area_t a = {org_x + (int)floorf(x + 0.5f), org_y + (int)floorf(y + 0.5f),
				   org_x + (int)floorf(x + w + 0.5f) - 1, org_y + (int)floorf(y + h + 0.5f) - 1};
	lv_draw_rect(cur_layer, &d, &a);
}

static void tri_alpha(float x1, float y1, float x2, float y2, float x3, float y3, lv_color_t color, lv_opa_t opa) {
	lv_draw_triangle_dsc_t d;
	lv_draw_triangle_dsc_init(&d);
	d.color = color;
	d.opa = opa;
	d.p[0].x = org_x + x1;
	d.p[0].y = org_y + y1;
	d.p[1].x = org_x + x2;
	d.p[1].y = org_y + y2;
	d.p[2].x = org_x + x3;
	d.p[2].y = org_y + y3;
	lv_draw_triangle(cur_layer, &d);
}

static void tri_px(float x1, float y1, float x2, float y2, float x3, float y3, lv_color_t color) {
	tri_alpha(x1, y1, x2, y2, x3, y3, color, LV_OPA_COVER);
}

static void line_px(float x1, float y1, float x2, float y2, lv_color_t color, int width) {
	lv_draw_line_dsc_t d;
	lv_draw_line_dsc_init(&d);
	d.color = color;
	d.width = width;
	d.opa = LV_OPA_COVER;
	d.p1.x = org_x + x1;
	d.p1.y = org_y + y1;
	d.p2.x = org_x + x2;
	d.p2.y = org_y + y2;
	lv_draw_line(cur_layer, &d);
}

static void draw_car(const lane_t *lane, const item_t *car, int sy) {
	float tl = (float)tile;
	float cx = (float)col_off + car->x * tl;
	float cw = car->w * tl;
	bool right = lane->dir > 0;
	float front = right ? cx + cw : cx;
	float fwd = right ? 1.0f : -1.0f;
	uint32_t body = lv_color_to_u32(car->color) & 0xffffff;
	// The cars stay bright at night, so they read against the dark road.
	lv_color_t paint = night ? lv_color_mix(hex(0x0a1233), car->color, 60) : car->color;
	lv_color_t side = shade_c(paint);
	lv_color_t roof = tint_c(paint);
	(void)body;

	// Headlights first: the beam lies on the road ahead of the car.
	if (night) {
		float reach = tl * 1.5f;
		tri_alpha(front, sy + tl * 0.46f, front + fwd * reach, sy + tl * 0.04f, front + fwd * reach, sy + tl * 0.96f,
				  hex(0xfef9c3), 46);
	}

	// Shadow on the road.
	rect_px(cx + cw * 0.04f, sy + tl * 0.84f, cw * 0.94f, tl * 0.12f, 6, lv_color_black(), night ? 90 : 70);
	// Lower body: its side, then the bonnet and boot surface above it.
	rect_px(cx, sy + tl * 0.4f, cw, tl * 0.44f, 6, side, LV_OPA_COVER);
	rect_px(cx, sy + tl * 0.26f, cw, tl * 0.4f, 6, paint, LV_OPA_COVER);
	rect_px(cx + cw * 0.05f, sy + tl * 0.42f, cw * 0.9f, 2, 0, lv_color_white(), 60);
	// Cabin: a block on top, with its roof lit and a band of windows.
	float cab_x = cx + cw * 0.22f, cab_w = cw * 0.52f;
	rect_px(cab_x, sy + tl * 0.1f, cab_w, tl * 0.34f, 6, side, LV_OPA_COVER);
	rect_px(cab_x, sy + tl * 0.06f, cab_w, tl * 0.2f, 6, roof, LV_OPA_COVER);
	rect_px(cab_x + cab_w * 0.07f, sy + tl * 0.24f, cab_w * 0.86f, tl * 0.15f, 3,
			night ? hex(0x1e3a5f) : hex(0x38557a), LV_OPA_COVER);
	rect_px(cab_x + cab_w * 0.07f, sy + tl * 0.24f, cab_w * 0.3f, 2, 0, lv_color_white(), 80);
	// Lights: white at the front, red at the back; bright and glowing at night.
	float fx = right ? front - tl * 0.1f : front;
	float bx = right ? cx : cx + cw - tl * 0.1f;
	lv_color_t head = night ? hex(0xfefce8) : hex(0xe2e8f0);
	rect_px(fx, sy + tl * 0.46f, tl * 0.1f, tl * 0.1f, 2, head, LV_OPA_COVER);
	rect_px(fx, sy + tl * 0.62f, tl * 0.1f, tl * 0.1f, 2, head, LV_OPA_COVER);
	rect_px(bx, sy + tl * 0.46f, tl * 0.1f, tl * 0.1f, 2, night ? hex(0xff3b3b) : hex(0x991b1b), LV_OPA_COVER);
	rect_px(bx, sy + tl * 0.62f, tl * 0.1f, tl * 0.1f, 2, night ? hex(0xff3b3b) : hex(0x991b1b), LV_OPA_COVER);
	if (night) {
		rect_px(fx - fwd * tl * 0.1f - (right ? 0 : tl * 0.2f), sy + tl * 0.4f, tl * 0.3f, tl * 0.38f, 8, hex(0xfef9c3), 60);
		rect_px(bx - (right ? tl * 0.1f : tl * 0.1f), sy + tl * 0.4f, tl * 0.3f, tl * 0.38f, 8, hex(0xff3b3b), 40);
	}
	// Wheels: a dark tyre and a grey hub.
	for (int w = 0; w < 2; w++) {
		float wx = w == 0 ? cx + cw * 0.12f : cx + cw * 0.88f - tl * 0.26f;
		rect_px(wx, sy + tl * 0.72f, tl * 0.26f, tl * 0.2f, 5, hex(0x0b1020), LV_OPA_COVER);
		rect_px(wx + tl * 0.08f, sy + tl * 0.77f, tl * 0.1f, tl * 0.1f, 3, hex(0x94a3b8), LV_OPA_COVER);
	}
}

static void draw_tree(float tx, int sy) {
	float tl = (float)tile;
	rect_px(tx + tl * 0.14f, sy + tl * 0.84f, tl * 0.72f, tl * 0.1f, 5, lv_color_black(), night ? 70 : 45);
	// Trunk: a lit face and a shaded one.
	rect_px(tx + tl * 0.4f, sy + tl * 0.6f, tl * 0.1f, tl * 0.28f, 1, sc(0x92400e), LV_OPA_COVER);
	rect_px(tx + tl * 0.5f, sy + tl * 0.6f, tl * 0.1f, tl * 0.28f, 1, shade_c(sc(0x92400e)), LV_OPA_COVER);
	// Leaves: a wide shaded block in front, a narrower lit one on top of it.
	rect_px(tx + tl * 0.08f, sy + tl * 0.34f, tl * 0.84f, tl * 0.4f, 8, shade_c(sc(0x15803d)), LV_OPA_COVER);
	rect_px(tx + tl * 0.16f, sy + tl * 0.06f, tl * 0.68f, tl * 0.44f, 8, sc(0x16a34a), LV_OPA_COVER);
	rect_px(tx + tl * 0.24f, sy + tl * 0.1f, tl * 0.3f, tl * 0.1f, 4, tint_c(sc(0x16a34a)), 120);
}

static void draw_log(const item_t *log, int sy) {
	float tl = (float)tile;
	float lx = (float)col_off + log->x * tl;
	float lw = log->w * tl;
	lv_color_t wood = sc(0x92400e);
	// Reflection on the water, then the trunk as three bands: underside, middle, lit top.
	rect_px(lx + 4, sy + tl * 0.8f, lw - 4, tl * 0.1f, 5, lv_color_black(), 40);
	rect_px(lx, sy + tl * 0.48f, lw, tl * 0.34f, 9, shade_c(wood), LV_OPA_COVER);
	rect_px(lx, sy + tl * 0.28f, lw, tl * 0.38f, 9, wood, LV_OPA_COVER);
	rect_px(lx + 6, sy + tl * 0.14f, lw - 12, tl * 0.24f, 8, sc(0xb45309), LV_OPA_COVER);
	rect_px(lx + 12, sy + tl * 0.19f, lw - 24, 2, 0, lv_color_white(), 70);
	rect_px(lx + 10, sy + tl * 0.46f, lw - 20, 2, 0, shade_c(wood), 120);
	// The cut ends, each a pale ring around a darker core.
	for (int e = 0; e < 2; e++) {
		float ex = e == 0 ? lx - 1 : lx + lw - tl * 0.22f + 1;
		rect_px(ex, sy + tl * 0.18f, tl * 0.22f, tl * 0.58f, 99, sc(0xd97706), LV_OPA_COVER);
		rect_px(ex + tl * 0.05f, sy + tl * 0.28f, tl * 0.12f, tl * 0.38f, 99, wood, LV_OPA_COVER);
	}
}

// A coin on the grass: it spins, so its width swings and the face goes dark
// when it is edge-on.
static void draw_lane_coin(int col, int sy) {
	float tl = (float)tile;
	float phase = (float)(now_us() / 1000 % 900) / 900.0f * 2.0f * (float)M_PI;
	float squash = fmaxf(0.18f, fabsf(cosf(phase)));
	float d = tl * 0.5f, w = d * squash;
	float cx = (float)col_off + (float)col * tl + tl * 0.5f;
	float top = sy + tl * 0.26f;
	rect_px(cx - tl * 0.18f, sy + tl * 0.8f, tl * 0.36f, tl * 0.08f, 5, lv_color_black(), 40);
	rect_px(cx - w / 2 - 1, top + tl * 0.05f, w + 2, d, 99, sc(0xb45309), LV_OPA_COVER);
	rect_px(cx - w / 2, top, w, d, 99, sc(0xfbbf24), LV_OPA_COVER);
	if (squash > 0.5f) {
		rect_px(cx - w * 0.28f, top + d * 0.22f, w * 0.56f, d * 0.56f, 99, sc(0xfde68a), LV_OPA_COVER);
	}
	if (night) {
		rect_px(cx - d * 0.7f, top - d * 0.2f, d * 1.4f, d * 1.4f, 99, hex(0xfde68a), 30);
	}
}

static void draw_lanes(void) {
	float tl = (float)tile;
	float base_y = (float)view_h - tl * 2.5f;
	int min_row = (int)floorf(camera_row) - 4;
	int max_row_v = (int)floorf(camera_row) + rows_on_screen();

	for (int r = min_row; r <= max_row_v; r++) {
		lane_t *lane = lane_get(r);
		int sy = (int)floorf(base_y - ((float)r - camera_row) * tl);
		if (sy > view_h || sy + tile < 0) {
			continue;
		}
		float vw = (float)view_w;

		if (lane->type == LANE_GRASS) {
			rect_px(0, (float)sy, vw, tl + 1, 0, sc(r % 2 == 0 ? 0x22c55e : 0x16a34a), LV_OPA_COVER);
			rect_px(0, (float)sy, vw, 3, 0, lv_color_white(), night ? 14 : 36);
			rect_px(0, sy + tl - 4, vw, 4, 0, lv_color_black(), 40);
			for (int i = 0; i < lane->tree_count; i++) {
				draw_tree((float)(col_off + lane->trees[i] * tile), sy);
			}
			if (lane->coin_col >= 0) {
				draw_lane_coin(lane->coin_col, sy);
			}
		} else if (lane->type == LANE_ROAD) {
			rect_px(0, (float)sy, vw, tl + 1, 0, sc(0x334155), LV_OPA_COVER);
			// Kerbs along both edges: a lit top face and a shaded face below.
			rect_px(0, (float)sy, vw, 5, 0, sc(0x94a3b8), LV_OPA_COVER);
			rect_px(0, sy + 5.0f, vw, 3, 0, lv_color_black(), 45);
			rect_px(0, sy + tl - 5, vw, 5, 0, sc(0x64748b), LV_OPA_COVER);
			rect_px(0, sy + tl - 5, vw, 2, 0, lv_color_white(), 40);
			for (int lx = 8; lx < view_w; lx += tile) {
				rect_px((float)lx, sy + tl * 0.5f - 1, tl * 0.45f, 3, 1, hex(0xffffff), night ? 70 : 50);
			}
			for (int i = 0; i < 2; i++) {
				draw_car(lane, &lane->items[i], sy);
			}
		} else {
			rect_px(0, (float)sy, vw, tl + 1, 0, sc(0x0284c7), LV_OPA_COVER);
			rect_px(0, sy + tl * 0.12f, vw, tl * 0.3f, 0, sc(0x38bdf8), 45);
			// Banks: sand on both edges, with a darker face where it meets the water.
			rect_px(0, (float)sy, vw, 5, 0, sc(0xd6b97a), LV_OPA_COVER);
			rect_px(0, sy + 5.0f, vw, 3, 0, lv_color_black(), 45);
			rect_px(0, sy + tl - 5, vw, 5, 0, sc(0xb8996a), LV_OPA_COVER);
			rect_px(0, sy + tl - 5, vw, 2, 0, lv_color_white(), 40);
			// A few ripples, fixed to the row so they do not flicker.
			for (int k = 0; k < 3; k++) {
				float wx = (float)((r * 37 + k * 151) & 511) / 511.0f * vw;
				rect_px(wx, sy + tl * (0.28f + 0.24f * (float)k), tl * 0.5f, 2, 1, lv_color_white(), 50);
			}
			for (int i = 0; i < 2; i++) {
				draw_log(&lane->items[i], sy);
			}
		}
	}
}

// Every shape below is placed as a fraction of a tile from the animal's top
// left, so it scales with the screen the way the web version does.
static float anim_unit; // the size of a tile the animal is drawn at
#define T (anim_unit)
#define RR(a, b, w, h, rad, col) rect_px(px + T * (a), py + T * (b), T * (w), T * (h), (rad), hex(col), LV_OPA_COVER)
#define BOX(a, b, w, h, col) RR(a, b, w, h, 0, col)
#define DOT(a, b, w, h, col) rect_px(px + T * (a), py + T * (b), (w), (h), 0, hex(col), LV_OPA_COVER)
#define TRI(x1, y1, x2, y2, x3, y3, col) \
	tri_px(px + T * (x1), py + T * (y1), px + T * (x2), py + T * (y2), px + T * (x3), py + T * (y3), hex(col))

static void draw_animal(const character_t *ch, float px, float py) {
	const uint32_t accent = ch->accent, snout = ch->snout, body = ch->body;

	// The few that wear something behind the body: a mane, a fleece.
	if (!strcmp(ch->id, "lion")) {
		RR(0.03f, 0.03f, 0.94f, 0.94f, 16, accent);
	} else if (!strcmp(ch->id, "sheep")) {
		RR(0.05f, 0.07f, 0.9f, 0.86f, 18, 0xf8fafc);
		RR(0.1f, 0.02f, 0.3f, 0.2f, 8, 0xf8fafc);
		RR(0.6f, 0.02f, 0.3f, 0.2f, 8, 0xf8fafc);
	}
	// A block rather than a flat square: the darker side under it, the body, and
	// a light edge along the top.
	rect_px(px + T * 0.14f, py + T * 0.24f, T * 0.72f, T * 0.7f, 8, shade(body), LV_OPA_COVER);
	RR(0.14f, 0.14f, 0.72f, 0.72f, 8, body);
	rect_px(px + T * 0.2f, py + T * 0.16f, T * 0.6f, T * 0.08f, 3, tint(body), LV_OPA_60);

	if (!strcmp(ch->id, "chicken")) {
		RR(0.08f, 0.36f, 0.08f, 0.26f, 3, 0xe2e8f0);
		RR(0.84f, 0.36f, 0.08f, 0.26f, 3, 0xe2e8f0);
		RR(0.36f, 0.03f, 0.14f, 0.14f, 3, accent);
		RR(0.48f, 0.06f, 0.14f, 0.11f, 3, accent);
		RR(0.38f, 0.36f, 0.24f, 0.16f, 4, snout);
		RR(0.43f, 0.51f, 0.14f, 0.12f, 3, accent);
	} else if (!strcmp(ch->id, "frog")) {
		outline_px(px + T * 0.14f, py + T * 0.14f, T * 0.72f, T * 0.72f, 8, hex(accent), 3);
		RR(0.16f, 0.02f, 0.24f, 0.2f, 5, body);
		RR(0.6f, 0.02f, 0.24f, 0.2f, 5, body);
		RR(0.26f, 0.44f, 0.48f, 0.34f, 6, snout);
		DOT(0.2f, 0.32f, 4, 3, 0xfb7185);
		DOT(0.72f, 0.32f, 4, 3, 0xfb7185);
		rect_px(px + T * 0.36f, py + T * 0.35f, T * 0.28f, 3, 0, hex(accent), LV_OPA_COVER);
	} else if (!strcmp(ch->id, "pig")) {
		RR(0.13f, 0.05f, 0.2f, 0.16f, 4, accent);
		RR(0.67f, 0.05f, 0.2f, 0.16f, 4, accent);
		DOT(0.19f, 0.42f, 4, 3, 0xfb7185);
		DOT(0.72f, 0.42f, 4, 3, 0xfb7185);
		RR(0.3f, 0.38f, 0.4f, 0.24f, 6, snout);
		DOT(0.4f, 0.46f, 3, 4, 0xbe185d);
		DOT(0.54f, 0.46f, 3, 4, 0xbe185d);
	} else if (!strcmp(ch->id, "fox")) {
		RR(0.78f, 0.44f, 0.2f, 0.26f, 7, body);
		RR(0.86f, 0.44f, 0.12f, 0.14f, 5, 0xffffff);
		TRI(0.15f, 0.2f, 0.24f, -0.04f, 0.36f, 0.18f, body);
		TRI(0.85f, 0.2f, 0.76f, -0.04f, 0.64f, 0.18f, body);
		TRI(0.21f, 0.04f, 0.24f, -0.04f, 0.28f, 0.04f, 0x1e293b);
		TRI(0.79f, 0.04f, 0.76f, -0.04f, 0.72f, 0.04f, 0x1e293b);
		RR(0.16f, 0.44f, 0.68f, 0.36f, 8, 0xffffff);
		TRI(0.34f, 0.36f, 0.66f, 0.36f, 0.5f, 0.58f, body);
		RR(0.44f, 0.52f, 0.12f, 0.09f, 3, 0x0f172a);
	} else if (!strcmp(ch->id, "penguin")) {
		RR(0.07f, 0.34f, 0.09f, 0.32f, 4, body);
		RR(0.84f, 0.34f, 0.09f, 0.32f, 4, body);
		RR(0.24f, 0.24f, 0.52f, 0.56f, 7, accent);
		BOX(0.44f, 0.22f, 0.12f, 0.12f, body);
		RR(0.37f, 0.38f, 0.26f, 0.15f, 4, snout);
		BOX(0.26f, 0.82f, 0.16f, 0.06f, snout);
		BOX(0.58f, 0.82f, 0.16f, 0.06f, snout);
	} else if (!strcmp(ch->id, "duck")) {
		RR(0.42f, 0.05f, 0.16f, 0.11f, 3, accent);
		RR(0.08f, 0.36f, 0.08f, 0.26f, 3, accent);
		RR(0.84f, 0.36f, 0.08f, 0.26f, 3, accent);
		DOT(0.2f, 0.36f, 4, 3, 0xfb7185);
		DOT(0.72f, 0.36f, 4, 3, 0xfb7185);
		RR(0.3f, 0.38f, 0.4f, 0.2f, 5, snout);
		rect_px(px + T * 0.34f, py + T * 0.47f, T * 0.32f, 2, 0, hex(0xc2410c), LV_OPA_COVER);
	} else if (!strcmp(ch->id, "bunny")) {
		RR(0.21f, -0.1f, 0.17f, 0.3f, 5, body);
		RR(0.62f, -0.1f, 0.17f, 0.3f, 5, body);
		BOX(0.25f, -0.05f, 0.09f, 0.2f, accent);
		BOX(0.66f, -0.05f, 0.09f, 0.2f, accent);
		DOT(0.2f, 0.38f, 4, 3, 0xfda4af);
		DOT(0.72f, 0.38f, 4, 3, 0xfda4af);
		RR(0.42f, 0.4f, 0.16f, 0.11f, 3, snout);
		DOT(0.44f, 0.51f, 4, 5, 0xffffff);
		DOT(0.51f, 0.51f, 4, 5, 0xffffff);
	} else if (!strcmp(ch->id, "dog")) {
		RR(0.07f, 0.16f, 0.15f, 0.38f, 5, accent);
		RR(0.78f, 0.16f, 0.15f, 0.38f, 5, accent);
		RR(0.56f, 0.2f, 0.22f, 0.2f, 5, accent);
		RR(0.32f, 0.38f, 0.36f, 0.26f, 6, 0xfef3c7);
		RR(0.43f, 0.41f, 0.14f, 0.1f, 3, snout);
		RR(0.46f, 0.56f, 0.1f, 0.12f, 3, 0xfb7185);
	} else if (!strcmp(ch->id, "unicorn")) {
		rect_px(px + T * 0.08f, py + T * 0.08f, T * 0.84f, T * 0.84f, 12, hex(0xe879f9), LV_OPA_30);
		RR(0.14f, 0.14f, 0.72f, 0.72f, 8, body);
		RR(0.16f, 0.02f, 0.16f, 0.18f, 4, body);
		RR(0.68f, 0.02f, 0.16f, 0.18f, 4, body);
		BOX(0.2f, 0.06f, 0.08f, 0.1f, 0xf472b6);
		BOX(0.72f, 0.06f, 0.08f, 0.1f, 0xf472b6);
		static const uint32_t mane[3] = {0xec4899, 0xa855f7, 0x38bdf8};
		for (int i = 0; i < 3; i++) {
			BOX(0.07f, 0.24f + i * 0.16f, 0.09f, 0.15f, mane[i]);
			BOX(0.84f, 0.24f + i * 0.16f, 0.09f, 0.15f, mane[i]);
		}
		RR(0.34f, 0.1f, 0.32f, 0.1f, 3, 0xec4899);
		TRI(0.5f, -0.18f, 0.41f, 0.12f, 0.59f, 0.12f, 0xfacc15);
		line_px(px + T * 0.45f, py + T * 0.04f, px + T * 0.55f, py + T * 0.01f, hex(0xca8a04), 2);
		line_px(px + T * 0.47f, py - T * 0.05f, px + T * 0.53f, py - T * 0.08f, hex(0xca8a04), 2);
		RR(0.3f, 0.44f, 0.4f, 0.24f, 6, 0xfbcfe8);
		DOT(0.39f, 0.51f, 3, 3, 0xdb2777);
		DOT(0.56f, 0.51f, 3, 3, 0xdb2777);
	} else if (!strcmp(ch->id, "cat")) {
		TRI(0.15f, 0.2f, 0.2f, -0.02f, 0.36f, 0.16f, body);
		TRI(0.85f, 0.2f, 0.8f, -0.02f, 0.64f, 0.16f, body);
		TRI(0.2f, 0.16f, 0.22f, 0.04f, 0.3f, 0.14f, accent);
		TRI(0.8f, 0.16f, 0.78f, 0.04f, 0.7f, 0.14f, accent);
		RR(0.44f, 0.4f, 0.12f, 0.08f, 3, snout);
		line_px(px + T * 0.1f, py + T * 0.5f, px + T * 0.3f, py + T * 0.48f, hex(0xe2e8f0), 2);
		line_px(px + T * 0.1f, py + T * 0.58f, px + T * 0.3f, py + T * 0.54f, hex(0xe2e8f0), 2);
		line_px(px + T * 0.9f, py + T * 0.5f, px + T * 0.7f, py + T * 0.48f, hex(0xe2e8f0), 2);
		line_px(px + T * 0.9f, py + T * 0.58f, px + T * 0.7f, py + T * 0.54f, hex(0xe2e8f0), 2);
	} else if (!strcmp(ch->id, "panda")) {
		RR(0.1f, 0.04f, 0.22f, 0.22f, 7, accent);
		RR(0.68f, 0.04f, 0.22f, 0.22f, 7, accent);
		RR(0.18f, 0.22f, 0.24f, 0.24f, 8, accent);
		RR(0.58f, 0.22f, 0.24f, 0.24f, 8, accent);
		RR(0.43f, 0.46f, 0.14f, 0.1f, 3, accent);
		BOX(0.49f, 0.56f, 0.02f, 0.06f, accent);
	} else if (!strcmp(ch->id, "bear")) {
		RR(0.1f, 0.04f, 0.22f, 0.22f, 7, body);
		RR(0.68f, 0.04f, 0.22f, 0.22f, 7, body);
		RR(0.16f, 0.1f, 0.1f, 0.1f, 3, accent);
		RR(0.74f, 0.1f, 0.1f, 0.1f, 3, accent);
		RR(0.3f, 0.42f, 0.4f, 0.3f, 9, snout);
		RR(0.44f, 0.44f, 0.12f, 0.09f, 3, 0x1c1917);
	} else if (!strcmp(ch->id, "lion")) {
		RR(0.3f, 0.42f, 0.4f, 0.3f, 9, snout);
		RR(0.44f, 0.44f, 0.12f, 0.09f, 3, 0x7c2d12);
		BOX(0.49f, 0.53f, 0.02f, 0.06f, 0x7c2d12);
	} else if (!strcmp(ch->id, "tiger")) {
		RR(0.14f, 0.06f, 0.16f, 0.16f, 5, body);
		RR(0.7f, 0.06f, 0.16f, 0.16f, 5, body);
		BOX(0.3f, 0.14f, 0.05f, 0.14f, accent);
		BOX(0.475f, 0.14f, 0.05f, 0.16f, accent);
		BOX(0.65f, 0.14f, 0.05f, 0.14f, accent);
		BOX(0.14f, 0.5f, 0.1f, 0.05f, accent);
		BOX(0.14f, 0.62f, 0.1f, 0.05f, accent);
		BOX(0.76f, 0.5f, 0.1f, 0.05f, accent);
		BOX(0.76f, 0.62f, 0.1f, 0.05f, accent);
		RR(0.3f, 0.44f, 0.4f, 0.28f, 8, snout);
		RR(0.44f, 0.46f, 0.12f, 0.08f, 3, 0xfb7185);
	} else if (!strcmp(ch->id, "cow")) {
		RR(0.56f, 0.14f, 0.3f, 0.26f, 8, accent);
		RR(0.14f, 0.5f, 0.2f, 0.26f, 8, accent);
		RR(0.16f, 0.02f, 0.1f, 0.12f, 3, 0xfef3c7);
		RR(0.74f, 0.02f, 0.1f, 0.12f, 3, 0xfef3c7);
		RR(0.04f, 0.22f, 0.12f, 0.12f, 4, snout);
		RR(0.84f, 0.22f, 0.12f, 0.12f, 4, snout);
		RR(0.26f, 0.52f, 0.48f, 0.28f, 9, snout);
		DOT(0.38f, 0.62f, 3, 4, 0x9d174d);
		DOT(0.58f, 0.62f, 3, 4, 0x9d174d);
	} else if (!strcmp(ch->id, "monkey")) {
		RR(0.04f, 0.28f, 0.16f, 0.16f, 6, body);
		RR(0.8f, 0.28f, 0.16f, 0.16f, 6, body);
		RR(0.08f, 0.32f, 0.08f, 0.08f, 3, snout);
		RR(0.84f, 0.32f, 0.08f, 0.08f, 3, snout);
		RR(0.22f, 0.24f, 0.56f, 0.5f, 11, snout);
		DOT(0.44f, 0.46f, 3, 3, 0x78350f);
		DOT(0.54f, 0.46f, 3, 3, 0x78350f);
		RR(0.38f, 0.6f, 0.24f, 0.06f, 3, 0x78350f);
	} else if (!strcmp(ch->id, "koala")) {
		RR(0.02f, 0.04f, 0.28f, 0.28f, 10, body);
		RR(0.7f, 0.04f, 0.28f, 0.28f, 10, body);
		RR(0.08f, 0.1f, 0.16f, 0.16f, 6, accent);
		RR(0.76f, 0.1f, 0.16f, 0.16f, 6, accent);
		RR(0.42f, 0.38f, 0.16f, 0.22f, 7, snout);
	} else if (!strcmp(ch->id, "mouse")) {
		RR(0.08f, 0.02f, 0.24f, 0.24f, 9, body);
		RR(0.68f, 0.02f, 0.24f, 0.24f, 9, body);
		RR(0.13f, 0.07f, 0.14f, 0.14f, 6, accent);
		RR(0.73f, 0.07f, 0.14f, 0.14f, 6, accent);
		RR(0.45f, 0.42f, 0.1f, 0.08f, 3, snout);
		line_px(px + T * 0.88f, py + T * 0.74f, px + T * 1.02f, py + T * 0.62f, hex(accent), 2);
		line_px(px + T * 0.1f, py + T * 0.5f, px + T * 0.3f, py + T * 0.5f, hex(0x9ca3af), 2);
		line_px(px + T * 0.9f, py + T * 0.5f, px + T * 0.7f, py + T * 0.5f, hex(0x9ca3af), 2);
	} else if (!strcmp(ch->id, "owl")) {
		TRI(0.14f, 0.2f, 0.18f, 0.0f, 0.34f, 0.16f, body);
		TRI(0.86f, 0.2f, 0.82f, 0.0f, 0.66f, 0.16f, body);
		RR(0.28f, 0.6f, 0.44f, 0.2f, 7, 0xd6a671);
		RR(0.16f, 0.2f, 0.28f, 0.28f, 14, accent);
		RR(0.56f, 0.2f, 0.28f, 0.28f, 14, accent);
		RR(0.26f, 0.3f, 0.1f, 0.1f, 5, 0x0f172a);
		RR(0.64f, 0.3f, 0.1f, 0.1f, 5, 0x0f172a);
		RR(0.44f, 0.46f, 0.12f, 0.14f, 4, snout);
	} else if (!strcmp(ch->id, "sheep")) {
		RR(0.24f, 0.22f, 0.52f, 0.54f, 11, accent);
		RR(0.08f, 0.3f, 0.14f, 0.1f, 4, accent);
		RR(0.78f, 0.3f, 0.14f, 0.1f, 4, accent);
		RR(0.42f, 0.5f, 0.16f, 0.1f, 4, 0x4b5563);
	} else if (!strcmp(ch->id, "horse")) {
		TRI(0.2f, 0.18f, 0.24f, -0.04f, 0.36f, 0.16f, body);
		TRI(0.8f, 0.18f, 0.76f, -0.04f, 0.64f, 0.16f, body);
		RR(0.34f, 0.0f, 0.32f, 0.22f, 5, accent);
		BOX(0.46f, 0.16f, 0.08f, 0.26f, 0xffffff);
		RR(0.28f, 0.5f, 0.44f, 0.3f, 9, snout);
		DOT(0.4f, 0.62f, 3, 4, 0x78350f);
		DOT(0.57f, 0.62f, 3, 4, 0x78350f);
	} else if (!strcmp(ch->id, "raccoon")) {
		TRI(0.15f, 0.2f, 0.2f, -0.02f, 0.36f, 0.16f, body);
		TRI(0.85f, 0.2f, 0.8f, -0.02f, 0.64f, 0.16f, body);
		RR(0.82f, 0.5f, 0.16f, 0.3f, 6, body);
		BOX(0.86f, 0.56f, 0.12f, 0.05f, accent);
		BOX(0.86f, 0.68f, 0.12f, 0.05f, accent);
		RR(0.14f, 0.2f, 0.72f, 0.22f, 7, accent);
		RR(0.32f, 0.44f, 0.36f, 0.32f, 9, snout);
		RR(0.44f, 0.46f, 0.12f, 0.08f, 3, 0x111827);
	} else if (!strcmp(ch->id, "hedgehog")) {
		RR(0.06f, 0.06f, 0.88f, 0.54f, 8, accent);
		for (int i = 0; i < 5; i++) {
			TRI(0.1f + i * 0.17f, 0.2f, 0.18f + i * 0.17f, -0.04f, 0.26f + i * 0.17f, 0.2f, accent);
		}
		RR(0.2f, 0.34f, 0.6f, 0.46f, 9, snout);
		RR(0.44f, 0.5f, 0.12f, 0.1f, 4, 0x111827);
	} else if (!strcmp(ch->id, "giraffe")) {
		BOX(0.34f, 0.0f, 0.06f, 0.16f, 0x92400e);
		RR(0.31f, -0.04f, 0.12f, 0.08f, 3, 0x92400e);
		BOX(0.6f, 0.0f, 0.06f, 0.16f, 0x92400e);
		RR(0.57f, -0.04f, 0.12f, 0.08f, 3, 0x92400e);
		RR(0.04f, 0.2f, 0.14f, 0.09f, 4, body);
		RR(0.82f, 0.2f, 0.14f, 0.09f, 4, body);
		RR(0.18f, 0.2f, 0.12f, 0.12f, 4, accent);
		RR(0.68f, 0.18f, 0.1f, 0.1f, 4, accent);
		RR(0.2f, 0.6f, 0.1f, 0.12f, 4, accent);
		RR(0.7f, 0.56f, 0.12f, 0.14f, 4, accent);
		RR(0.3f, 0.48f, 0.4f, 0.3f, 9, snout);
		DOT(0.4f, 0.6f, 3, 4, 0x92400e);
		DOT(0.57f, 0.6f, 3, 4, 0x92400e);
	} else if (!strcmp(ch->id, "elephant")) {
		RR(-0.02f, 0.18f, 0.22f, 0.42f, 9, snout);
		RR(0.8f, 0.18f, 0.22f, 0.42f, 9, snout);
		RR(0.4f, 0.46f, 0.2f, 0.34f, 7, body);
		RR(0.4f, 0.74f, 0.2f, 0.1f, 4, accent);
		BOX(0.32f, 0.6f, 0.05f, 0.14f, 0xffffff);
		BOX(0.63f, 0.6f, 0.05f, 0.14f, 0xffffff);
	} else if (!strcmp(ch->id, "turtle")) {
		RR(0.08f, 0.2f, 0.14f, 0.14f, 5, snout);
		RR(0.78f, 0.2f, 0.14f, 0.14f, 5, snout);
		RR(0.08f, 0.66f, 0.14f, 0.14f, 5, snout);
		RR(0.78f, 0.66f, 0.14f, 0.14f, 5, snout);
		RR(0.2f, 0.0f, 0.6f, 0.26f, 8, snout);
		RR(0.2f, 0.3f, 0.6f, 0.5f, 12, accent);
		BOX(0.48f, 0.34f, 0.04f, 0.42f, 0x14532d);
		BOX(0.26f, 0.5f, 0.48f, 0.04f, 0x14532d);
	} else if (!strcmp(ch->id, "robot")) {
		RR(0.44f, -0.06f, 0.12f, 0.1f, 5, snout);
		BOX(0.48f, 0.04f, 0.04f, 0.12f, accent);
		RR(0.04f, 0.3f, 0.1f, 0.2f, 3, accent);
		RR(0.86f, 0.3f, 0.1f, 0.2f, 3, accent);
		RR(0.2f, 0.2f, 0.6f, 0.3f, 6, 0x0f172a);
		BOX(0.3f, 0.6f, 0.4f, 0.04f, accent);
		BOX(0.3f, 0.68f, 0.4f, 0.04f, accent);
	} else if (!strcmp(ch->id, "alien")) {
		line_px(px + T * 0.3f, py + T * 0.16f, px + T * 0.24f, py + T * 0.0f, hex(accent), 2);
		line_px(px + T * 0.7f, py + T * 0.16f, px + T * 0.76f, py + T * 0.0f, hex(accent), 2);
		RR(0.18f, -0.04f, 0.12f, 0.1f, 5, snout);
		RR(0.7f, -0.04f, 0.12f, 0.1f, 5, snout);
		RR(0.18f, 0.22f, 0.28f, 0.24f, 10, 0x0f172a);
		RR(0.54f, 0.22f, 0.28f, 0.24f, 10, 0x0f172a);
		BOX(0.4f, 0.62f, 0.2f, 0.03f, accent);
	} else if (!strcmp(ch->id, "ghost")) {
		RR(0.04f, 0.4f, 0.12f, 0.16f, 5, body);
		RR(0.84f, 0.4f, 0.12f, 0.16f, 5, body);
		RR(0.14f, 0.76f, 0.2f, 0.16f, 6, body);
		RR(0.4f, 0.76f, 0.2f, 0.16f, 6, body);
		RR(0.66f, 0.76f, 0.2f, 0.16f, 6, body);
		DOT(0.2f, 0.4f, 4, 3, accent);
		DOT(0.72f, 0.4f, 4, 3, accent);
		RR(0.42f, 0.5f, 0.16f, 0.12f, 6, snout);
	} else if (!strcmp(ch->id, "dragon")) {
		// Wings: a membrane with a darker bone, spread to each side.
		TRI(0.16f, 0.4f, -0.14f, 0.12f, -0.04f, 0.66f, 0x15803d);
		TRI(0.84f, 0.4f, 1.14f, 0.12f, 1.04f, 0.66f, 0x15803d);
		TRI(0.14f, 0.42f, -0.08f, 0.22f, -0.01f, 0.52f, 0x4ade80);
		TRI(0.86f, 0.42f, 1.08f, 0.22f, 1.01f, 0.52f, 0x4ade80);
		line_px(px + T * 0.14f, py + T * 0.4f, px - T * 0.14f, py + T * 0.12f, hex(0x14532d), 2);
		line_px(px + T * 0.86f, py + T * 0.4f, px + T * 1.14f, py + T * 0.12f, hex(0x14532d), 2);
		// A tail curling off to the side, ending in a spade.
		RR(0.72f, 0.74f, 0.26f, 0.1f, 4, body);
		TRI(0.94f, 0.68f, 1.06f, 0.79f, 0.94f, 0.9f, 0xf97316);
		// Horns, and a crest of spikes along the top.
		TRI(0.2f, 0.18f, 0.1f, -0.12f, 0.34f, 0.14f, 0xfde68a);
		TRI(0.8f, 0.18f, 0.9f, -0.12f, 0.66f, 0.14f, 0xfde68a);
		TRI(0.4f, 0.16f, 0.44f, -0.04f, 0.48f, 0.16f, 0xf97316);
		TRI(0.48f, 0.16f, 0.52f, -0.08f, 0.56f, 0.16f, 0xf97316);
		TRI(0.56f, 0.16f, 0.6f, -0.04f, 0.64f, 0.16f, 0xf97316);
		// Brow ridges, a wide snout with nostrils, and two fangs.
		BOX(0.2f, 0.22f, 0.24f, 0.04f, accent);
		BOX(0.56f, 0.22f, 0.24f, 0.04f, accent);
		RR(0.24f, 0.44f, 0.52f, 0.34f, 10, snout);
		RR(0.3f, 0.5f, 0.1f, 0.08f, 4, 0x4ade80);
		RR(0.6f, 0.5f, 0.1f, 0.08f, 4, 0x4ade80);
		DOT(0.34f, 0.53f, 3, 3, accent);
		DOT(0.62f, 0.53f, 3, 3, accent);
		TRI(0.33f, 0.76f, 0.37f, 0.76f, 0.35f, 0.84f, 0xffffff);
		TRI(0.63f, 0.76f, 0.67f, 0.76f, 0.65f, 0.84f, 0xffffff);
		// A belly of pale scales between the legs.
		BOX(0.4f, 0.8f, 0.2f, 0.04f, 0xfde68a);
	}

	// Eyes: light ones where the face is dark, and up in the head for the two
	// that have one apart from the body.
	static const char *const light_eyes[] = {"penguin", "raccoon", "panda", "sheep", "alien"};
	uint32_t eye = 0x0f172a;
	for (size_t i = 0; i < sizeof(light_eyes) / sizeof(light_eyes[0]); i++) {
		if (!strcmp(ch->id, light_eyes[i])) {
			eye = 0xffffff;
		}
	}
	if (!strcmp(ch->id, "robot")) {
		eye = 0x22d3ee;
	}
	float ey = (!strcmp(ch->id, "frog") || !strcmp(ch->id, "turtle")) ? 0.1f : 0.28f;
	bool plain = eye != 0x0f172a; // light or glowing eyes on a dark face stay simple dots
	if (plain) {
		DOT(0.26f, ey, 3, 3, eye);
		DOT(0.66f, ey, 3, 3, eye);
	} else {
		// A white of the eye with a pupil in it: the dragon's and the cat's are
		// yellow, and the dragon's pupil is a slit.
		bool dragon = !strcmp(ch->id, "dragon");
		uint32_t white = dragon ? 0xfde047 : !strcmp(ch->id, "cat") ? 0xd9f99d : 0xffffff;
		float size = fmaxf(5.0f, T * 0.17f), pupil = fmaxf(3.0f, T * 0.09f);
		for (int k = 0; k < 2; k++) {
			float ex = px + T * (k ? 0.64f : 0.24f), ey_px = py + T * ey - 1.0f;
			rect_px(ex, ey_px, size, size, 3, hex(white), LV_OPA_COVER);
			if (dragon) {
				rect_px(ex + size * 0.38f, ey_px, size * 0.24f + 1, size, 1, hex(0x0f172a), LV_OPA_COVER);
			} else {
				rect_px(ex + (size - pupil) / 2 + 1, ey_px + (size - pupil) / 2 + 1, pupil, pupil, 2, hex(0x0f172a),
						LV_OPA_COVER);
			}
		}
	}
}

static void draw_player(void) {
	float tl = (float)tile;
	float base_y = (float)view_h - tl * 2.5f;
	float col = player.col + (player.target_col - player.col) * player.progress;
	float row = player.row + ((float)player.target_row - player.row) * player.progress;
	float hop = sinf(player.progress * (float)M_PI) * (tl * 0.28f);

	float px = (float)col_off + col * tl;
	float ground_y = base_y - (row - camera_row) * tl;
	float py = ground_y - hop;

	// The shadow stays on the ground and shrinks as the animal rises.
	float shrink = 1.0f - sinf(player.progress * (float)M_PI) * 0.25f;
	rect_px(px + tl * (0.5f - 0.34f * shrink), ground_y + tl * 0.72f, tl * 0.68f * shrink, tl * 0.18f * shrink, 6,
			hex(0x0f172a), 71);

	anim_unit = tl;
	draw_animal(&CHARACTERS[run_char], px, py);
}

// The coin: a gold disc with the animal it turns you into on its face, floating.
static void draw_coin(void) {
	float tl = (float)tile;
	float base_y = (float)view_h - tl * 2.5f;
	float x = (float)col_off + (float)coin.col * tl;
	float y = base_y - ((float)coin.row - camera_row) * tl;
	if (y > (float)view_h || y + tl < 0) {
		return;
	}
	float bob = sinf((float)(now_us() / 1000 % 1200) / 1200.0f * 2.0f * (float)M_PI) * tl * 0.06f;
	float d = tl * 0.84f, cx = x + tl * 0.5f, top = y + tl * 0.08f - bob;
	rect_px(cx - d * 0.4f, y + tl * 0.84f, d * 0.8f, tl * 0.1f, 6, lv_color_black(), 50);
	rect_px(cx - d / 2, top + tl * 0.07f, d, d, 99, hex(0xb45309), LV_OPA_COVER); // the coin's edge
	rect_px(cx - d / 2, top, d, d, 99, hex(0xfbbf24), LV_OPA_COVER);
	rect_px(cx - d * 0.42f, top + d * 0.08f, d * 0.84f, d * 0.84f, 99, hex(0xfde68a), LV_OPA_COVER);
	rect_px(cx - d * 0.3f, top + d * 0.02f, d * 0.4f, d * 0.1f, 3, lv_color_white(), 140);
	float u = tl * 0.5f;
	anim_unit = u;
	draw_animal(&CHARACTERS[coin.who], cx - u / 2, top + d / 2 - u / 2);
}

static void board_draw_cb(lv_event_t *e) {
	cur_layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	org_x = area.x1;
	org_y = area.y1;
	draw_lanes();
	if (coin.active) {
		draw_coin();
	}
	draw_player();
}

// The animal on its selector button, drawn by the same code as in the game.
#define PREVIEW_BOX 52
#define PREVIEW_UNIT 38.0f

static void preview_draw_cb(lv_event_t *e) {
	cur_layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	org_x = area.x1;
	org_y = area.y1;
	anim_unit = PREVIEW_UNIT;
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	// Room above for ears and the horn, and a little at the sides for the fox's tail.
	draw_animal(&CHARACTERS[index], (PREVIEW_BOX - PREVIEW_UNIT) / 2.0f - 1.0f, 11.0f);
	if (!char_unlocked(index)) {
		// Dimmed, with a padlock over it.
		rect_px(0, 0, PREVIEW_BOX, PREVIEW_BOX, 10, lv_color_black(), 170);
		outline_px(20, 11, 12, 16, 6, hex(0xe2e8f0), 3);
		rect_px(15, 24, 22, 17, 4, hex(0xfbbf24), LV_OPA_COVER);
		rect_px(24, 29, 4, 7, 2, hex(0x78350f), LV_OPA_COVER);
	}
}

// ---------------------------------------------------------------------------
// the loop and the touch
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *timer) {
	if (!active) {
		lv_timer_pause(timer); // woken by power.c at the end of a standby
		return;
	}
	int64_t now = now_us();
	float dt = (float)(now - last_us) / 1e6f;
	last_us = now;
	if (dt > MAX_DT) {
		dt = MAX_DT;
	}
	// Under the start and game-over cards the world stands still: nothing
	// there needs the frames.
	if (state != ST_PLAYING) {
		return;
	}
	update(dt);
	board_refresh();
}

static lv_point_t press_point;

static void board_press_cb(lv_event_t *e) {
	(void)e;
	if (!active) {
		return;
	}
	lv_indev_get_point(lv_indev_active(), &press_point);
}

static void board_release_cb(lv_event_t *e) {
	(void)e;
	if (!active || state != ST_PLAYING) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	int dx = p.x - press_point.x;
	int dy = p.y - press_point.y;

	if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) {
		move_player(0, 1, DIR_UP);
	} else if (abs(dx) > abs(dy)) {
		if (dx > 0) {
			move_player(1, 0, DIR_RIGHT);
		} else {
			move_player(-1, 0, DIR_LEFT);
		}
	} else if (dy > 0) {
		move_player(0, -1, DIR_DOWN);
	} else {
		move_player(0, 1, DIR_UP);
	}
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------

static void chars_refresh(void) {
	for (int i = 0; i < CHAR_COUNT; i++) {
		bool on = i == selected_char;
		bool open = char_unlocked(i);
		lv_obj_set_style_bg_color(char_buttons[i], hex(on ? 0x0b4d3a : 0x1e293b), 0);
		lv_obj_set_style_border_width(char_buttons[i], on ? 2 : 1, 0);
		lv_obj_set_style_border_color(char_buttons[i], on ? hex(0x34d399) : lv_color_white(), 0);
		lv_obj_set_style_border_opa(char_buttons[i], on ? LV_OPA_COVER : LV_OPA_20, 0);
		// An open animal shows its name; a locked one, what it costs.
		if (open) {
			lv_label_set_text(char_names[i], tr(CHARACTERS[i].name_key));
			lv_obj_set_style_text_color(char_names[i], hex(0xe2e8f0), 0);
		} else {
			lv_label_set_text_fmt(char_names[i], "%d", char_price(i));
			lv_obj_set_style_text_color(char_names[i], hex(0xfbbf24), 0);
		}
		lv_obj_invalidate(char_buttons[i]);
	}
}

static void char_click_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!char_unlocked(i)) {
		pending_buy = i;
		lv_label_set_text_fmt(buy_title, tr("crossy_buy_title"), tr(CHARACTERS[i].name_key));
		if (coins_total >= char_price(i)) {
			lv_label_set_text_fmt(buy_text, tr("crossy_buy_text"), char_price(i), coins_total);
			lv_obj_set_hidden(buy_ok, false);
		} else {
			lv_label_set_text_fmt(buy_text, tr("crossy_buy_short"), char_price(i) - coins_total);
			lv_obj_set_hidden(buy_ok, true);
		}
		overlay_show(buy_overlay, true);
		return;
	}
	selected_char = i;
	config_set_int("crossyroad", "character", selected_char);
	crossysound_play(CROSSY_SFX_HOP);
	chars_refresh();
	board_refresh();
}

static void buy_cancel_cb(lv_event_t *e) {
	(void)e;
	overlay_show(buy_overlay, false);
}

static void buy_confirm_cb(lv_event_t *e) {
	(void)e;
	int price = char_price(pending_buy);
	if (!char_unlocked(pending_buy) && coins_total >= price) {
		coins_total -= price;
		unlocked_mask |= 1u << pending_buy;
		selected_char = pending_buy;
		config_set_int("crossyroad", "coins", coins_total);
		config_set_int("crossyroad", "character", selected_char);
		unlock_save();
		config_save();
		score_labels_update();
		crossysound_play(CROSSY_SFX_COIN);
		chars_refresh();
		board_refresh();
	}
	overlay_show(buy_overlay, false);
}

static void start_click_cb(lv_event_t *e) {
	(void)e;
	config_save();
	init_game(false);
}

static void restart_click_cb(lv_event_t *e) {
	(void)e;
	init_game(false);
}

static void over_tap_cb(lv_event_t *e) {
	(void)e;
	init_game(false);
}

static void change_char_cb(lv_event_t *e) {
	(void)e;
	init_game(true);
}

static void close_click_cb(lv_event_t *e) {
	(void)e;
	if (state == ST_PLAYING) {
		state = ST_PAUSED;
		paused_by_modal = true;
	}
	overlay_show(exit_overlay, true);
}

static void cancel_exit_cb(lv_event_t *e) {
	(void)e;
	overlay_show(exit_overlay, false);
	if (paused_by_modal) {
		paused_by_modal = false;
		state = ST_PLAYING;
		last_us = now_us();
	}
}

static void confirm_exit_cb(lv_event_t *e) {
	(void)e;
	overlay_show(exit_overlay, false);
	back_btn_cb(NULL); // unloading the page stops everything
}

// ---------------------------------------------------------------------------
// building the page
// ---------------------------------------------------------------------------

static lv_obj_t *label_make(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, hex(color), 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	return l;
}

static lv_obj_t *button_make(lv_obj_t *parent, const char *text, int w, int h, uint32_t bg, uint32_t fg,
							 lv_event_cb_t cb) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, w, h);
	lv_obj_set_style_bg_color(b, hex(bg), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(b, 16, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l = label_make(b, text, &font_ui_22, fg);
	lv_obj_center(l);
	return b;
}

static lv_obj_t *overlay_make(lv_obj_t *parent, lv_opa_t opa) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_size(o, view_w, view_h);
	lv_obj_set_pos(o, 0, 0);
	lv_obj_set_style_bg_color(o, hex(0x020617), 0);
	lv_obj_set_style_bg_opa(o, opa, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_radius(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(o, true);
	return o;
}

static lv_obj_t *card_make(lv_obj_t *parent, int width) {
	lv_obj_t *c = lv_obj_create(parent);
	lv_obj_set_size(c, width, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(c, hex(0x0f172a), 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(c, 1, 0);
	lv_obj_set_style_border_color(c, lv_color_white(), 0);
	lv_obj_set_style_border_opa(c, LV_OPA_20, 0);
	lv_obj_set_style_radius(c, 24, 0);
	lv_obj_set_style_pad_all(c, 24, 0);
	lv_obj_set_style_pad_gap(c, 10, 0);
	lv_obj_set_scrollable(c, false);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return c;
}

static lv_obj_t *hud_box(lv_obj_t *parent, const char *caption, uint32_t color, lv_obj_t **value) {
	lv_obj_t *box = lv_obj_create(parent);
	lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(box, hex(0x0f172a), 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_70, 0);
	lv_obj_set_style_border_width(box, 1, 0);
	lv_obj_set_style_border_color(box, lv_color_white(), 0);
	lv_obj_set_style_border_opa(box, LV_OPA_20, 0);
	lv_obj_set_style_radius(box, 16, 0);
	lv_obj_set_style_pad_hor(box, 8, 0);
	lv_obj_set_style_pad_ver(box, 6, 0);
	lv_obj_set_style_pad_gap(box, 0, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_clickable(box, false);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	label_make(box, caption, &font_ui_14, 0xcbd5e1);
	*value = label_make(box, "0", &font_ui_32, color);
	return box;
}

static lv_obj_t *mode_btn;

// The button's picture: the sun by day, a crescent moon at night.
static void mode_icon_draw_cb(lv_event_t *e) {
	cur_layer = lv_event_get_layer(e);
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	org_x = 0;
	org_y = 0;
	float cx = (float)(area.x1 + area.x2) / 2.0f;
	float cy = (float)(area.y1 + area.y2) / 2.0f;
	if (night) {
		rect_px(cx - 9, cy - 9, 18, 18, 9, hex(0xe2e8f0), LV_OPA_COVER);
		rect_px(cx - 3, cy - 12, 17, 17, 9, hex(0x0f172a), LV_OPA_COVER);
	} else {
		rect_px(cx - 6, cy - 6, 12, 12, 6, hex(0xfbbf24), LV_OPA_COVER);
		for (int i = 0; i < 8; i++) {
			float a = (float)i * (float)M_PI / 4.0f;
			rect_px(cx + cosf(a) * 12.0f - 1.5f, cy + sinf(a) * 12.0f - 1.5f, 3, 3, 1, hex(0xfbbf24), LV_OPA_COVER);
		}
	}
}

static void mode_click_cb(lv_event_t *e) {
	(void)e;
	night = !night;
	config_set_bool("crossyroad", "night", night);
	config_save();
	lv_obj_invalidate(mode_btn);
	board_refresh();
}

static void build_hud(lv_obj_t *screen) {
	const int margin = 16;

	lv_obj_t *close = lv_button_create(screen);
	lv_obj_set_size(close, 44, 44);
	lv_obj_set_pos(close, margin, margin);
	lv_obj_set_style_bg_color(close, hex(0x0f172a), 0);
	lv_obj_set_style_bg_opa(close, LV_OPA_70, 0);
	lv_obj_set_style_radius(close, 16, 0);
	lv_obj_set_style_border_width(close, 1, 0);
	lv_obj_set_style_border_color(close, lv_color_white(), 0);
	lv_obj_set_style_border_opa(close, LV_OPA_20, 0);
	lv_obj_set_style_shadow_width(close, 0, 0);
	lv_obj_add_event_cb(close, close_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *x = lv_image_create(close);
	lv_image_set_src(x, &icon_close);
	lv_obj_set_style_image_recolor(x, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(x, LV_OPA_COVER, 0);
	lv_obj_center(x);

	mode_btn = lv_button_create(screen);
	lv_obj_set_size(mode_btn, 44, 44);
	lv_obj_set_pos(mode_btn, margin, margin + 44 + 8);
	lv_obj_set_style_bg_color(mode_btn, hex(0x0f172a), 0);
	lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(mode_btn, 16, 0);
	lv_obj_set_style_border_width(mode_btn, 1, 0);
	lv_obj_set_style_border_color(mode_btn, lv_color_white(), 0);
	lv_obj_set_style_border_opa(mode_btn, LV_OPA_20, 0);
	lv_obj_set_style_shadow_width(mode_btn, 0, 0);
	lv_obj_add_event_cb(mode_btn, mode_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(mode_btn, mode_icon_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

	// The three markers side by side along the top, after the two buttons.
	const int gap = 8;
	int left = margin + 44 + gap;
	int box_w = (view_w - left - margin - 2 * gap) / 3;
	lv_obj_t *boxes[3] = {hud_box(screen, tr("crossy_score"), 0xfbbf24, &score_value),
						  hud_box(screen, tr("crossy_record"), 0x34d399, &best_value),
						  hud_box(screen, tr("crossy_coins"), 0xfde047, &coins_value)};
	for (int i = 0; i < 3; i++) {
		lv_obj_set_width(boxes[i], box_w);
		lv_obj_set_pos(boxes[i], left + i * (box_w + gap), margin);
	}
}

static void build_start(lv_obj_t *screen) {
	start_overlay = overlay_make(screen, LV_OPA_70);
	int card_w = view_w - 48 < 420 ? view_w - 48 : 420;
	lv_obj_t *card = card_make(start_overlay, card_w);

	label_make(card, tr("crossy_road"), &font_ui_36_bold, 0xffffff);
	label_make(card, tr("crossy_hint"), &font_ui_16, 0x94a3b8);
	label_make(card, tr("crossy_choose"), &font_ui_14, 0x94a3b8);

	lv_obj_t *grid = lv_obj_create(card);
	// Three rows and a bit, so the next row shows and the list is seen to scroll.
	lv_obj_set_size(grid, lv_pct(100), SELECTOR_H);
	lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(grid, 0, 0);
	lv_obj_set_style_pad_all(grid, 0, 0);
	lv_obj_set_style_pad_gap(grid, 8, 0);
	lv_obj_set_scrollable(grid, true);
	lv_obj_set_scroll_dir(grid, LV_DIR_VER);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	// The rows start at the top: centred, a list taller than its box would run
	// off above the top, where it cannot be scrolled to.
	lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	int inner = card_w - 48 - 2; // padding and border
	int bw = (inner - 16) / 3;
	for (int i = 0; i < CHAR_COUNT; i++) {
		lv_obj_t *b = lv_button_create(grid);
		lv_obj_set_size(b, bw, SELECTOR_ROW_H);
		lv_obj_set_style_radius(b, 16, 0);
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_pad_all(b, 4, 0);
		lv_obj_set_style_pad_gap(b, 2, 0);
		lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(b, char_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *pic = lv_obj_create(b);
		lv_obj_set_size(pic, PREVIEW_BOX, PREVIEW_BOX);
		lv_obj_set_style_bg_opa(pic, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(pic, 0, 0);
		lv_obj_set_style_pad_all(pic, 0, 0);
		lv_obj_set_scrollable(pic, false);
		lv_obj_set_clickable(pic, false);
		lv_obj_add_event_cb(pic, preview_draw_cb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);

		char_names[i] = label_make(b, tr(CHARACTERS[i].name_key), &font_ui_16, 0xe2e8f0);
		char_buttons[i] = b;
	}

	lv_obj_t *start = button_make(card, tr("crossy_start"), lv_pct(100), 56, 0x10b981, 0x020617, start_click_cb);
	lv_obj_set_style_radius(start, 28, 0);
}

static void build_over(lv_obj_t *screen) {
	over_overlay = overlay_make(screen, LV_OPA_70);
	// Tapping anywhere that is not a button plays again.
	lv_obj_add_event_cb(over_overlay, over_tap_cb, LV_EVENT_CLICKED, NULL);
	int card_w = view_w - 48 < 380 ? view_w - 48 : 380;
	lv_obj_t *card = card_make(over_overlay, card_w);
	lv_obj_set_event_bubble(card, true);

	lv_obj_t *l = label_make(card, tr("crossy_game_over"), &font_ui_32, 0xf43f5e);
	lv_obj_set_style_text_font(l, &font_ui_36_bold, 0);
	over_reason = label_make(card, "", &font_ui_20, 0xcbd5e1);
	lv_obj_set_width(over_reason, lv_pct(100));

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 12, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_event_bubble(row, true);
	const char *caps[2] = {tr("crossy_score"), tr("crossy_record")};
	lv_obj_t **vals[2] = {&over_score, &over_best};
	uint32_t cols[2] = {0xffffff, 0x34d399};
	for (int i = 0; i < 2; i++) {
		lv_obj_t *cell = lv_obj_create(row);
		lv_obj_set_size(cell, (card_w - 48 - 2 - 12) / 2, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_color(cell, hex(0x1e293b), 0);
		lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(cell, 0, 0);
		lv_obj_set_style_radius(cell, 16, 0);
		lv_obj_set_style_pad_all(cell, 10, 0);
		lv_obj_set_style_pad_gap(cell, 0, 0);
		lv_obj_set_scrollable(cell, false);
		lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_event_bubble(cell, true);
		label_make(cell, caps[i], &font_ui_14, 0x94a3b8);
		*vals[i] = label_make(cell, "0", &font_ui_32, cols[i]);
	}

	over_coins = label_make(card, "", &font_ui_18, 0xfde047);
	button_make(card, tr("crossy_play_again"), lv_pct(100), 56, 0x10b981, 0x020617, restart_click_cb);
	button_make(card, tr("crossy_change_character"), lv_pct(100), 44, 0x1e293b, 0xe2e8f0, change_char_cb);
	label_make(card, tr("crossy_tap_anywhere"), &font_ui_14, 0x94a3b8);
}

static void build_buy(lv_obj_t *screen) {
	buy_overlay = overlay_make(screen, LV_OPA_80);
	int card_w = view_w - 48 < 340 ? view_w - 48 : 340;
	lv_obj_t *card = card_make(buy_overlay, card_w);

	buy_title = label_make(card, "", &font_ui_24, 0xffffff);
	lv_obj_set_width(buy_title, lv_pct(100));
	buy_text = label_make(card, "", &font_ui_18, 0xfde047);
	lv_obj_set_width(buy_text, lv_pct(100));

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 12, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	int bw = (card_w - 48 - 2 - 12) / 2;
	button_make(row, tr("crossy_cancel"), bw, 52, 0x1e293b, 0xe2e8f0, buy_cancel_cb);
	buy_ok = button_make(row, tr("crossy_unlock"), bw, 52, 0x10b981, 0x020617, buy_confirm_cb);
}

static void build_exit(lv_obj_t *screen) {
	exit_overlay = overlay_make(screen, LV_OPA_80);
	int card_w = view_w - 48 < 340 ? view_w - 48 : 340;
	lv_obj_t *card = card_make(exit_overlay, card_w);

	label_make(card, tr("crossy_exit_title"), &font_ui_28, 0xffffff);
	lv_obj_t *text = label_make(card, tr("crossy_exit_text"), &font_ui_18, 0xcbd5e1);
	lv_obj_set_width(text, lv_pct(100));

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 12, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	int bw = (card_w - 48 - 2 - 12) / 2;
	button_make(row, tr("crossy_continue"), bw, 52, 0x1e293b, 0xe2e8f0, cancel_exit_cb);
	button_make(row, tr("crossy_close"), bw, 52, 0xe11d48, 0xffffff, confirm_exit_cb);
}

// ---------------------------------------------------------------------------
// opening and leaving
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	volume_overlay_allow_outside_player(true);
	active = true;
	last_us = now_us();
	lv_timer_resume(tick_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	active = false;
	lv_timer_pause(tick_timer);
	crossysound_stop();
	config_save();
	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

void crossyroad_open(void) {
	srand((unsigned)now_us());
	best = (int)config_get_int("crossyroad", "best", 0);
	selected_char = (int)config_get_int("crossyroad", "character", DEFAULT_CHAR);
	if (selected_char < 0 || selected_char >= CHAR_COUNT || !char_unlocked(selected_char)) {
		selected_char = DEFAULT_CHAR;
	}
	sound_on = config_get_bool("crossyroad", "sound", true);
	night = config_get_bool("crossyroad", "night", false);
	coins_total = (int)config_get_int("crossyroad", "coins", 0);
	unlocked_mask = (uint32_t)config_get_int("crossyroad", "unlocked", 1) | 1u;
	lv_obj_invalidate(mode_btn);
	if (sound_on) {
		crossysound_start();
	}
	overlay_show(exit_overlay, false);
	overlay_show(buy_overlay, false);
	chars_refresh();
	init_game(true);
	lv_obj_update_layout(crossyroad_screen);
	lv_obj_scroll_to_view(char_buttons[selected_char], LV_ANIM_OFF);
	switch_screen(crossyroad_screen);
}

void crossyroad_init(gui_config_t *cfg) {
	view_w = (int)cfg->screen_width;
	view_h = (int)cfg->screen_height;
	tile = view_w / GRID_COLS;
	if (tile < 24) {
		tile = 24;
	}
	col_off = (view_w - GRID_COLS * tile) / 2;

	lv_obj_set_style_bg_color(crossyroad_screen, hex(0x0f172a), 0);
	lv_obj_set_style_bg_opa(crossyroad_screen, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(crossyroad_screen, 0, 0);
	lv_obj_set_scrollable(crossyroad_screen, false);

	board = lv_obj_create(crossyroad_screen);
	lv_obj_set_size(board, view_w, view_h);
	lv_obj_set_pos(board, 0, 0);
	lv_obj_set_style_bg_opa(board, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(board, 0, 0);
	lv_obj_set_style_radius(board, 0, 0);
	lv_obj_set_style_pad_all(board, 0, 0);
	lv_obj_set_scrollable(board, false);
	lv_obj_add_event_cb(board, board_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(board, board_press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(board, board_release_cb, LV_EVENT_RELEASED, NULL);

	build_hud(crossyroad_screen);
	build_start(crossyroad_screen);
	build_over(crossyroad_screen);
	build_buy(crossyroad_screen);
	build_exit(crossyroad_screen);

	lv_obj_add_event_cb(crossyroad_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(crossyroad_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
}
