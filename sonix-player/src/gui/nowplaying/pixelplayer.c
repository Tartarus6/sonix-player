#include "pixelplayer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/lyrics.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

// ---------------------------------------------------------------------------
// Sizes, for the 480-wide panel. The sleeve takes what the rest leaves.
// ---------------------------------------------------------------------------

#define PAD 24
#define HEADER_H 56
#define HEADER_BTN 52
#define TITLE_H 44
#define ARTIST_H 30
#define WAVE_H 32
#define TIME_H 26
#define KEY_H 84
#define TRAY_H 64
#define GAP 12
#define COVER_RADIUS 28
#define TICK_MS 66 // about fifteen frames a second for the ripple: enough to read as moving

// The wave: how tall, how long one ripple is, and how fast it travels.
#define WAVE_AMP 5.0f
#define WAVE_LEN 28.0f
#define WAVE_SPEED 2.2f // radians a second
#define WAVE_STROKE 5

// The lyrics: the line being sung in full ink, the rest faded.
#define LYRIC_DIM LV_OPA_40
#define LYRICS_MAX 400

static lv_obj_t *root;
static lv_timer_t *tick;
static bool active;
static int scr_w, scr_h;
static int cover_size;

static lv_obj_t *close_btn, *queue_btn, *album_label, *album_icon;
static lv_obj_t *cover_box, *cover_img, *cover_note;
static lv_obj_t *lyrics_view;
static lv_obj_t *title_label, *artist_label, *lyrics_btn, *more_btn, *side_pill;
static lv_obj_t *wave_obj;
static lv_obj_t *elapsed_label, *total_label, *format_pill, *format_label;
static lv_obj_t *prev_btn, *play_btn, *next_btn, *play_icon;
static lv_obj_t *tray, *shuffle_chip, *repeat_chip, *fav_chip;
static lv_obj_t *shuffle_icon, *repeat_icon, *fav_icon;
static lv_obj_t *speed_text; // the speed, written in the first chip on a book or an episode
// What the page is dressed for: the tray and the skip keys change with it.
static player_kind_t shown_kind = (player_kind_t)-1;
static lv_obj_t *btn_icons[8];
static int btn_icon_n;

// What is drawn, so a tick only redoes what changed.
static char shown_file[512];
static const void *shown_player_cover;
static bool cover_wanted;	   // a copy of the player's sleeve is still to be taken
static cover_image_t own_cover; // our own copy: the player frees its own at will
static const lyrics_t *shown_lyrics;
// Which words those are. The player keeps them in one place for every track,
// so the pointer alone never changes: the file, the text and the count do.
static char shown_lyrics_file[512];
static const char *shown_lyrics_arena;
static int shown_lyrics_count = -1;
static int shown_lyric_line = -2;
static bool lyrics_on;

static double pos_secs, total_secs;
static bool playing;
static float wave_phase, wave_amp;
static bool seeking;
static float seek_frac;

// ---------------------------------------------------------------------------
// Colours from the sleeve
// ---------------------------------------------------------------------------

typedef struct {
	lv_color_t bg, ink, ink2, key, key_ink, play, play_ink, tray, chip, chip_ink, pill;
} palette_t;

static palette_t pal;

static void rgb_to_hsl(uint32_t rgb, float *h, float *s, float *l) {
	float r = ((rgb >> 16) & 0xff) / 255.0f, g = ((rgb >> 8) & 0xff) / 255.0f, b = (rgb & 0xff) / 255.0f;
	float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
	*l = (mx + mn) / 2.0f;
	if (mx - mn < 1e-4f) {
		*h = 0;
		*s = 0;
		return;
	}
	float d = mx - mn;
	*s = *l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
	if (mx == r) {
		*h = (g - b) / d + (g < b ? 6.0f : 0.0f);
	} else if (mx == g) {
		*h = (b - r) / d + 2.0f;
	} else {
		*h = (r - g) / d + 4.0f;
	}
	*h *= 60.0f;
}

static float hue_part(float p, float q, float t) {
	if (t < 0) {
		t += 1;
	}
	if (t > 1) {
		t -= 1;
	}
	if (t < 1.0f / 6) {
		return p + (q - p) * 6 * t;
	}
	if (t < 0.5f) {
		return q;
	}
	if (t < 2.0f / 3) {
		return p + (q - p) * (2.0f / 3 - t) * 6;
	}
	return p;
}

static lv_color_t hsl(float h, float s, float l) {
	h = fmodf(h, 360.0f);
	if (h < 0) {
		h += 360.0f;
	}
	s = fmaxf(0, fminf(1, s));
	l = fmaxf(0, fminf(1, l));
	float r, g, b;
	if (s < 1e-4f) {
		r = g = b = l;
	} else {
		float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
		float p = 2 * l - q;
		float k = h / 360.0f;
		r = hue_part(p, q, k + 1.0f / 3);
		g = hue_part(p, q, k);
		b = hue_part(p, q, k - 1.0f / 3);
	}
	return lv_color_make((uint8_t)(r * 255 + 0.5f), (uint8_t)(g * 255 + 0.5f), (uint8_t)(b * 255 + 0.5f));
}

// One hue from the sleeve, and every role worked out from it: a light page in
// that hue, dark ink, deep keys, and the play key in the opposite hue so it
// stands out the way the reference does.
static void palette_from(uint32_t tone) {
	float h, s, l;
	rgb_to_hsl(tone ? tone : 0x7b8db0, &h, &s, &l);
	float s0 = s < 0.08f ? 0.12f : fmaxf(0.35f, fminf(0.75f, s));
	float opp = h + 180.0f;
	pal.bg = hsl(h, s0 * 0.85f, 0.68f);
	pal.ink = hsl(h, 0.35f, 0.10f);
	pal.ink2 = hsl(h, 0.25f, 0.32f);
	pal.key = hsl(h, fminf(0.85f, s0 + 0.2f), 0.38f);
	pal.key_ink = hsl(h, 0.6f, 0.95f);
	pal.play = hsl(opp, s < 0.08f ? 0.15f : 0.62f, 0.55f);
	pal.play_ink = hsl(opp, 0.5f, 0.12f);
	pal.tray = hsl(h, s0 * 0.7f, 0.80f);
	pal.chip = hsl(h, s0 * 0.6f, 0.90f);
	pal.chip_ink = hsl(h, 0.35f, 0.12f);
	pal.pill = hsl(h, s0 * 0.7f, 0.75f);
}

static void tint_icon(lv_obj_t *icon, lv_color_t c) {
	lv_obj_set_style_image_recolor(icon, c, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
}

static void paint_modes(void);
static void paint_fav(void);

static void paint(void) {
	lv_obj_set_style_bg_color(root, pal.bg, 0);
	lv_obj_set_style_bg_color(close_btn, pal.chip, 0);
	lv_obj_set_style_bg_color(queue_btn, pal.chip, 0);
	lv_obj_set_style_bg_color(lyrics_btn, pal.chip, 0);
	lv_obj_set_style_bg_color(more_btn, pal.chip, 0);
	lv_obj_set_style_bg_color(cover_box, pal.tray, 0);
	lv_obj_set_style_text_color(album_label, pal.ink, 0);
	lv_obj_set_style_text_color(title_label, pal.ink, 0);
	lv_obj_set_style_text_color(artist_label, pal.ink2, 0);
	lv_obj_set_style_text_color(elapsed_label, pal.ink, 0);
	lv_obj_set_style_text_color(total_label, pal.ink, 0);
	lv_obj_set_style_bg_color(format_pill, pal.pill, 0);
	lv_obj_set_style_text_color(format_label, pal.ink, 0);
	lv_obj_set_style_bg_color(prev_btn, pal.key, 0);
	lv_obj_set_style_bg_color(next_btn, pal.key, 0);
	lv_obj_set_style_bg_color(play_btn, pal.play, 0);
	lv_obj_set_style_bg_color(tray, pal.tray, 0);
	for (int i = 0; i < btn_icon_n; i++) {
		tint_icon(btn_icons[i], pal.ink);
	}
	tint_icon(album_icon, pal.ink);
	tint_icon(cover_note, pal.ink2);
	tint_icon(lv_obj_get_child(prev_btn, 0), pal.key_ink);
	tint_icon(lv_obj_get_child(next_btn, 0), pal.key_ink);
	tint_icon(play_icon, pal.play_ink);
	paint_modes();
	paint_fav();
	if (lyrics_view) {
		uint32_t n = lv_obj_get_child_count(lyrics_view);
		for (uint32_t i = 0; i < n; i++) {
			lv_obj_set_style_text_color(lv_obj_get_child(lyrics_view, (int32_t)i), pal.ink, 0);
		}
	}
	lv_obj_invalidate(root);
}

// ---------------------------------------------------------------------------
// The wavy progress line
// ---------------------------------------------------------------------------

static void line_seg(lv_layer_t *layer, float x1, float y1, float x2, float y2, lv_color_t c, lv_opa_t opa) {
	lv_draw_line_dsc_t d;
	lv_draw_line_dsc_init(&d);
	d.color = c;
	d.opa = opa;
	d.width = WAVE_STROKE;
	d.round_start = 1;
	d.round_end = 1;
	d.p1.x = x1;
	d.p1.y = y1;
	d.p2.x = x2;
	d.p2.y = y2;
	lv_draw_line(layer, &d);
}

static float played_frac(void) {
	if (seeking) {
		return seek_frac;
	}
	if (total_secs <= 0.1) {
		return 0.0f;
	}
	float f = (float)(pos_secs / total_secs);
	return fmaxf(0.0f, fminf(1.0f, f));
}

static void wave_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_current_target(e), &a);
	float left = (float)a.x1 + 8, right = (float)a.x2 - 8;
	float mid = (float)(a.y1 + a.y2) / 2.0f;
	float x_play = left + (right - left) * played_frac();

	// The rest of the track: a straight line, in the keys' colour.
	if (right > x_play) {
		line_seg(layer, x_play, mid, right, mid, pal.key, LV_OPA_COVER);
	}
	// What has played: a ripple that travels while the music plays and lies
	// flat while it is paused.
	float step = 10.0f;
	float px = left, py = mid + sinf(wave_phase) * wave_amp;
	for (float x = left + step; x < x_play + step; x += step) {
		float xe = fminf(x, x_play);
		float ye = mid + sinf((xe - left) / WAVE_LEN * 2.0f * (float)M_PI + wave_phase) * wave_amp;
		line_seg(layer, px, py, xe, ye, pal.ink, LV_OPA_COVER);
		px = xe;
		py = ye;
	}
	// The handle.
	lv_draw_rect_dsc_t r;
	lv_draw_rect_dsc_init(&r);
	r.bg_color = pal.ink;
	r.radius = LV_RADIUS_CIRCLE;
	lv_area_t k = {(int32_t)x_play - 10, (int32_t)mid - 10, (int32_t)x_play + 10, (int32_t)mid + 10};
	lv_draw_rect(layer, &r, &k);
	// The far end, a small dot.
	lv_area_t end = {(int32_t)right - 3, (int32_t)mid - 3, (int32_t)right + 3, (int32_t)mid + 3};
	r.bg_color = pal.ink;
	lv_draw_rect(layer, &r, &end);
}

static float frac_at_finger(void) {
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_area_t a;
	lv_obj_get_coords(wave_obj, &a);
	float left = (float)a.x1 + 8, right = (float)a.x2 - 8;
	return fmaxf(0.0f, fminf(1.0f, ((float)p.x - left) / (right - left)));
}

static void wave_touch_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (total_secs <= 0.1) {
		return;
	}
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
		seeking = true;
		seek_frac = frac_at_finger();
		lv_obj_invalidate(wave_obj);
	} else if (code == LV_EVENT_RELEASED) {
		seek_frac = frac_at_finger();
		pos_secs = seek_frac * total_secs;
		device_state_seek(pos_secs);
		seeking = false;
		lv_obj_invalidate(wave_obj);
	} else if (code == LV_EVENT_PRESS_LOST) {
		seeking = false;
		lv_obj_invalidate(wave_obj);
	}
}

// ---------------------------------------------------------------------------
// The lyrics
// ---------------------------------------------------------------------------

static void lyrics_build(const lyrics_t *ly) {
	lv_obj_clean(lyrics_view);
	shown_lyric_line = -2;
	if (!ly) {
		return;
	}
	int n = ly->count < LYRICS_MAX ? ly->count : LYRICS_MAX;
	for (int i = 0; i < n; i++) {
		lv_obj_t *l = lv_label_create(lyrics_view);
		lv_obj_set_width(l, lv_pct(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_label_set_text(l, ly->lines[i].text[0] ? ly->lines[i].text : " ");
		lv_obj_set_style_text_font(l, &font_ui_36_bold, 0);
		lv_obj_set_style_text_color(l, pal.ink, 0);
		lv_obj_set_style_text_opa(l, ly->synced ? LYRIC_DIM : LV_OPA_COVER, 0);
		lv_obj_set_clickable(l, false);
	}
}

static void lyrics_follow(void) {
	if (!lyrics_on || !shown_lyrics || !shown_lyrics->synced) {
		return;
	}
	int line = lyrics_line_at(shown_lyrics, (int32_t)(pos_secs * 1000.0));
	if (line == shown_lyric_line) {
		return;
	}
	int n = (int)lv_obj_get_child_count(lyrics_view);
	if (shown_lyric_line >= 0 && shown_lyric_line < n) {
		lv_obj_set_style_text_opa(lv_obj_get_child(lyrics_view, shown_lyric_line), LYRIC_DIM, 0);
	}
	shown_lyric_line = line;
	if (line >= 0 && line < n) {
		lv_obj_t *l = lv_obj_get_child(lyrics_view, line);
		lv_obj_set_style_text_opa(l, LV_OPA_COVER, 0);
		// The line being sung a third of the way down the box.
		lv_obj_update_layout(lyrics_view);
		int32_t y = lv_obj_get_y(l) - lv_obj_get_height(lyrics_view) / 3;
		lv_obj_scroll_to_y(lyrics_view, y > 0 ? y : 0, LV_ANIM_ON);
	}
}

static void lyrics_show(bool on) {
	lyrics_on = on && shown_lyrics && shown_lyrics->count > 0;
	lv_obj_set_hidden(lyrics_view, !lyrics_on);
	lv_obj_set_hidden(cover_box, lyrics_on);
	// The button, filled while the words are up, like the reference.
	lv_obj_set_style_bg_color(lyrics_btn, lyrics_on ? pal.key : pal.chip, 0);
	tint_icon(lv_obj_get_child(lyrics_btn, 0), lyrics_on ? pal.key_ink : pal.ink);
	shown_lyric_line = -2;
	lyrics_follow();
}

// With words, the button shows them or hides them. With none found, it offers
// to download them, as the other arrangements do.
static void lyrics_click_cb(lv_event_t *e) {
	(void)e;
	if (shown_lyrics && shown_lyrics->count > 0) {
		lyrics_show(!lyrics_on);
	} else if (shown_lyrics) {
		player_lyrics_search();
	}
}

// ---------------------------------------------------------------------------
// The buttons
// ---------------------------------------------------------------------------

// The tray holds three chips whose meaning follows what is playing:
//   music:   shuffle, repeat, favourite
//   a book:  speed, chapters, add a bookmark
//   podcast: speed, the episodes, follow the podcast
// The same three chips, as the ordinary player swaps its repeat button for
// the speed and hides the star on a book.
static void chip_paint(lv_obj_t *chip, lv_obj_t *icon, bool on) {
	lv_obj_set_style_bg_color(chip, on ? pal.play : pal.chip, 0);
	tint_icon(icon, on ? pal.play_ink : pal.chip_ink);
}

static void paint_modes(void) {
	player_kind_t kind = player_kind();
	if (kind == PLAYER_KIND_MUSIC) {
		playback_mode_t m = playlist_get_mode();
		bool repeat = m == PLAYBACK_MODE_REPEAT_ALL || m == PLAYBACK_MODE_REPEAT_ONE;
		lv_image_set_src(repeat_icon, m == PLAYBACK_MODE_REPEAT_ONE ? &icon_repeat_one
									  : repeat						 ? &icon_repeat_all
																	 : &icon_repeat_off);
		chip_paint(shuffle_chip, shuffle_icon, m == PLAYBACK_MODE_SHUFFLE);
		chip_paint(repeat_chip, repeat_icon, repeat);
		return;
	}
	// The speed, lit when it is not the normal one.
	int permille = player_speed_permille();
	char t[16];
	snprintf(t, sizeof(t), "%d.%02dx", permille / 1000, (permille % 1000) / 10);
	size_t n = strlen(t); // "1.50x" reads as "1.5x", "1.00x" as "1.0x"
	if (n >= 3 && t[n - 2] == '0' && t[n - 3] != '.') {
		memmove(&t[n - 2], &t[n - 1], 2);
	}
	lv_label_set_text(speed_text, t);
	bool fast = permille != 1000;
	lv_obj_set_style_bg_color(shuffle_chip, fast ? pal.play : pal.chip, 0);
	lv_obj_set_style_text_color(speed_text, fast ? pal.play_ink : pal.chip_ink, 0);
	if (kind == PLAYER_KIND_BOOK) {
		bool chapters = player_book_has_chapters();
		chip_paint(repeat_chip, repeat_icon, false);
		lv_obj_set_style_image_opa(repeat_icon, chapters ? LV_OPA_COVER : LV_OPA_40, 0);
	} else {
		chip_paint(repeat_chip, repeat_icon, false);
		lv_obj_set_style_image_opa(repeat_icon, LV_OPA_COVER, 0);
	}
}

static void paint_fav(void) {
	if (player_kind() == PLAYER_KIND_BOOK) {
		chip_paint(fav_chip, fav_icon, false); // the bookmark chip: an action, never lit
		lv_obj_set_style_image_opa(fav_icon, LV_OPA_COVER, 0);
		return;
	}
	// The favourite on music, following the podcast on an episode: the same
	// question the player's star asks of either.
	bool fav = player_current_is_favourite();
	chip_paint(fav_chip, fav_icon, fav);
	lv_obj_set_style_image_opa(fav_icon, fav ? LV_OPA_COVER : LV_OPA_60, 0);
}

// Puts the right pictures on the keys and chips for what is playing.
static void dress_for_kind(void) {
	player_kind_t kind = player_kind();
	const lv_image_dsc_t *prev = NULL, *next = NULL;
	player_skip_icons(&prev, &next);
	// Set only on a change: setting a picture redraws the key, four times a
	// second for nothing otherwise.
	lv_obj_t *pi = lv_obj_get_child(prev_btn, 0), *ni = lv_obj_get_child(next_btn, 0);
	if (lv_image_get_src(pi) != prev) {
		lv_image_set_src(pi, prev);
	}
	if (lv_image_get_src(ni) != next) {
		lv_image_set_src(ni, next);
	}
	if (kind == shown_kind) {
		return;
	}
	shown_kind = kind;
	bool music = kind == PLAYER_KIND_MUSIC;
	// The queue button in the corner is for music only: a book has no queue,
	// and a podcast's episodes are already in the tray.
	lv_obj_set_hidden(queue_btn, !music);
	// A podcast has no lyrics and its menu is the episodes the tray already
	// offers, so the pill beside the title goes and the title and the name
	// take the whole width.
	bool pill = kind != PLAYER_KIND_PODCAST;
	lv_obj_set_hidden(side_pill, !pill);
	int text_w = scr_w - 2 * PAD - (pill ? lv_obj_get_width(side_pill) + 12 : 0);
	lv_obj_set_width(title_label, text_w);
	lv_obj_set_width(artist_label, text_w);
	lv_obj_set_hidden(shuffle_icon, !music);
	lv_obj_set_hidden(speed_text, music);
	if (kind == PLAYER_KIND_BOOK) {
		lv_image_set_src(repeat_icon, &icon_list_music); // chapters
		lv_image_set_src(fav_icon, &icon_bookmark);
		lv_image_set_scale(fav_icon, 256);
	} else if (kind == PLAYER_KIND_PODCAST) {
		lv_image_set_src(repeat_icon, &icon_podcast_episodes);
		lv_image_set_src(fav_icon, &icon_heart);
		lv_image_set_scale(fav_icon, 256 * 28 / (int)(icon_heart.header.h ? icon_heart.header.h : 28));
	} else {
		lv_image_set_src(fav_icon, &icon_heart);
		lv_image_set_scale(fav_icon, 256 * 28 / (int)(icon_heart.header.h ? icon_heart.header.h : 28));
	}
	// The three dots: a podcast's go straight to its episodes, as on the player.
	lv_image_set_src(lv_obj_get_child(more_btn, 0), kind == PLAYER_KIND_PODCAST ? &icon_podcast_episodes
																				: &icon_ellipsis_vertical);
	lv_image_set_src(cover_note, kind == PLAYER_KIND_BOOK	   ? &icon_book_headphones
								 : kind == PLAYER_KIND_PODCAST ? &icon_podcast_cover
															   : &icon_music_note);
	paint_modes();
	paint_fav();
}

static void more_cb(lv_event_t *e) {
	player_open_more_menu(lv_event_get_current_target(e));
}

static void close_cb(lv_event_t *e) {
	(void)e;
	player_sheet_close(true);
}

static void queue_cb(lv_event_t *e) {
	(void)e;
	trackmenu_open_queue();
}

static void prev_cb(lv_event_t *e) {
	(void)e;
	player_screen_prev();
}

static void next_cb(lv_event_t *e) {
	(void)e;
	player_screen_next();
}

static void play_cb(lv_event_t *e) {
	(void)e;
	player_key_play_pause();
}

static void shuffle_cb(lv_event_t *e) {
	if (player_kind() != PLAYER_KIND_MUSIC) {
		player_open_speed_menu(lv_event_get_current_target(e));
		return;
	}
	playlist_set_mode(playlist_get_mode() == PLAYBACK_MODE_SHUFFLE ? PLAYBACK_MODE_NORMAL : PLAYBACK_MODE_SHUFFLE);
	player_mode_changed();
	paint_modes();
}

static void repeat_cb(lv_event_t *e) {
	(void)e;
	if (player_kind() == PLAYER_KIND_BOOK) {
		player_book_chapters(); // says so when the book has none
		return;
	}
	if (player_kind() == PLAYER_KIND_PODCAST) {
		trackmenu_open_queue();
		return;
	}
	playback_mode_t m = playlist_get_mode();
	playback_mode_t next = m == PLAYBACK_MODE_REPEAT_ALL ? PLAYBACK_MODE_REPEAT_ONE
						   : m == PLAYBACK_MODE_REPEAT_ONE ? PLAYBACK_MODE_NORMAL
														   : PLAYBACK_MODE_REPEAT_ALL;
	playlist_set_mode(next);
	player_mode_changed();
	paint_modes();
}

static void fav_cb(lv_event_t *e) {
	(void)e;
	if (player_kind() == PLAYER_KIND_BOOK) {
		player_book_add_bookmark();
		return;
	}
	player_toggle_favourite();
	paint_fav();
}

// ---------------------------------------------------------------------------
// What is playing
// ---------------------------------------------------------------------------

static void fmt_time(char *out, size_t n, double secs) {
	int s = secs > 0 ? (int)secs : 0;
	snprintf(out, n, "%02d:%02d", s / 60, s % 60);
}

static void format_update(const device_state_t *st) {
	const char *dot = st->live ? NULL : strrchr(st->current_file, '.');
	if (!dot || !dot[1] || strlen(dot + 1) > 5) {
		lv_obj_set_hidden(format_pill, true);
		return;
	}
	char ext[8];
	size_t n = 0;
	for (const char *c = dot + 1; *c && n < sizeof(ext) - 1; c++) {
		ext[n++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
	}
	ext[n] = '\0';
	int rate = 0, bits = 0;
	library_track_quality(st->current_file, &rate, &bits);
	if (rate <= 0) {
		rate = st->stream_sample_rate;
	}
	if (bits > 0 && rate > 0) {
		lv_label_set_text_fmt(format_label, "%s \xE2\x80\xA2 %d/%d", ext, bits, (rate + 500) / 1000);
	} else if (rate > 0) {
		lv_label_set_text_fmt(format_label, "%s \xE2\x80\xA2 %d kHz", ext, (rate + 500) / 1000);
	} else {
		lv_label_set_text(format_label, ext);
	}
	lv_obj_set_hidden(format_pill, false);
}

static void cover_drop(void) {
	lv_image_set_src(cover_img, NULL);
	cover_free(&own_cover);
	memset(&own_cover, 0, sizeof(own_cover));
}

// Takes our own copy of the player's sleeve once it has settled, and the
// colours with it.
static void cover_take(void) {
	if (!cover_wanted) {
		return;
	}
	if (!player_cover_image()) {
		// No sleeve at all: the note, and a neutral page.
		cover_wanted = false;
		cover_drop();
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_note, false);
		palette_from(0);
		paint();
		return;
	}
	cover_image_t copy;
	uint64_t id = 0;
	memset(&copy, 0, sizeof(copy));
	if (!player_cover_copy(cover_size, &copy, &id)) {
		return; // still on its way: asked again next tick
	}
	cover_wanted = false;
	cover_drop();
	own_cover = copy;
	own_cover.dsc.data = own_cover.pixels;
	lv_image_set_src(cover_img, &own_cover.dsc);
	lv_obj_set_hidden(cover_img, false);
	lv_obj_set_hidden(cover_note, true);
	uint32_t tone = 0;
	if (own_cover.has_tone) {
		tone = own_cover.tone;
	} else if (!player_cover_tone(&tone)) {
		tone = 0;
	}
	palette_from(tone);
	paint();
}

static void refresh(bool force) {
	device_state_t st;
	device_state_get(&st);
	pos_secs = st.progress_current_secs;
	total_secs = st.progress_total_secs;
	bool now_playing = st.status == AUDIO_STATUS_PLAYING;
	if (now_playing != playing || force) {
		playing = now_playing;
		lv_image_set_src(play_icon, playing ? &icon_pause_large : &icon_play_large);
	}

	const char *id = st.live ? st.metadata.title : st.current_file;
	const void *pc = player_cover_image();
	if (force || strcmp(id, shown_file) != 0) {
		snprintf(shown_file, sizeof(shown_file), "%s", id);
		const char *slash = strrchr(st.current_file, '/');
		scrolltext_set(title_label, st.metadata.title[0] ? st.metadata.title : (slash ? slash + 1 : st.current_file));
		scrolltext_set(artist_label, st.live ? st.metadata.artist : metadata_shown_artist(&st.metadata));
		scrolltext_set(album_label, st.metadata.album[0] ? st.metadata.album : tr("queue_now_playing"));
		format_update(&st);
		paint_modes();
		paint_fav();
		cover_wanted = true;
	}
	if (pc != shown_player_cover) {
		shown_player_cover = pc;
		cover_wanted = true;
	}
	cover_take();
	dress_for_kind();
	static int shown_speed = -1;
	if (shown_kind != PLAYER_KIND_MUSIC && player_speed_permille() != shown_speed) {
		shown_speed = player_speed_permille(); // changed from its menu
		paint_modes();
	}

	const lyrics_t *ly = player_current_lyrics();
	const char *arena = ly ? ly->arena : NULL;
	int count = ly ? ly->count : -1;
	if (force || strcmp(st.current_file, shown_lyrics_file) != 0 || arena != shown_lyrics_arena ||
		count != shown_lyrics_count) {
		bool same_track = strcmp(st.current_file, shown_lyrics_file) == 0;
		// Read and found empty for this very song, before: what comes now was
		// downloaded. Still loading (-1) is not that, it is the song arriving.
		bool was_empty = same_track && shown_lyrics_count == 0;
		snprintf(shown_lyrics_file, sizeof(shown_lyrics_file), "%s", st.current_file);
		shown_lyrics_arena = arena;
		shown_lyrics_count = count;
		shown_lyrics = ly;
		lyrics_build(ly);
		bool any = ly && ly->count > 0;
		// Still being read: dimmed and asleep. Read and empty: awake, to
		// download them. Read with words: awake, to show them.
		lv_obj_set_style_opa(lyrics_btn, ly ? LV_OPA_COVER : LV_OPA_40, 0);
		lv_obj_set_clickable(lyrics_btn, ly != NULL);
		if (!same_track) {
			lyrics_show(false); // a new song starts on its sleeve
		} else if (any && was_empty) {
			lyrics_show(true); // just downloaded for this one: shown at once
		} else {
			lyrics_show(lyrics_on && any);
		}
	}
	lyrics_follow();

	char t[16];
	fmt_time(t, sizeof(t), seeking ? seek_frac * total_secs : pos_secs);
	if (strcmp(lv_label_get_text(elapsed_label), t) != 0) {
		lv_label_set_text(elapsed_label, t);
	}
	fmt_time(t, sizeof(t), total_secs);
	if (strcmp(lv_label_get_text(total_label), t) != 0) {
		lv_label_set_text(total_label, t);
	}
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!active || lv_obj_is_hidden(root) || !player_sheet_is_open()) {
		return; // nothing to draw with the player put away
	}
	// The player raises some of its own widgets as it goes (the lyrics page,
	// its badges): this layer stays above all of them.
	lv_obj_t *sheet = lv_obj_get_parent(root);
	if (lv_obj_get_index(root) != (int32_t)lv_obj_get_child_count(sheet) - 1) {
		lv_obj_move_foreground(root);
	}
	float dt = TICK_MS / 1000.0f;
	// The ripple grows while it plays and settles flat when it stops.
	float want = playing ? WAVE_AMP : 0.0f;
	wave_amp += (want - wave_amp) * fminf(1.0f, dt * 8.0f);
	if (playing) {
		wave_phase = fmodf(wave_phase + WAVE_SPEED * dt * 2.0f, 2.0f * (float)M_PI);
	}
	static int slow;
	if (++slow >= 5) { // labels, cover and lyrics: four times a second is plenty
		slow = 0;
		refresh(false);
	}
	if (playing || wave_amp > 0.05f) {
		lv_obj_invalidate(wave_obj);
	}
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *button(lv_obj_t *parent, int w, int h, int radius, const lv_image_dsc_t *icon, lv_event_cb_t cb) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, w, h);
	lv_obj_set_style_radius(b, radius, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *i = lv_image_create(b);
	lv_image_set_src(i, icon);
	lv_obj_center(i);
	lv_obj_set_clickable(i, false);
	return b;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, "");
	lv_obj_set_style_text_font(l, font, 0);
	return l;
}

void pixelplayer_init(lv_obj_t *sheet, gui_config_t *cfg) {
	scr_w = (int)cfg->screen_width;
	scr_h = (int)cfg->screen_height;
	int top = cfg->top_bar_height + 4;
	int inner = scr_w - 2 * PAD;
	int fixed = HEADER_H + GAP + GAP + TITLE_H + ARTIST_H + 8 + WAVE_H + TIME_H + GAP + KEY_H + GAP + TRAY_H + 14;
	cover_size = scr_h - top - fixed;
	if (cover_size > inner) {
		cover_size = inner;
	}
	if (cover_size < 120) {
		cover_size = 120;
	}
	palette_from(0);

	root = lv_obj_create(sheet);
	lv_obj_remove_style_all(root);
	lv_obj_set_size(root, scr_w, scr_h);
	lv_obj_set_pos(root, 0, 0);
	lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(root, false);
	lv_obj_set_clickable(root, true); // nothing underneath answers while this is up
	player_sheet_attach_drag(root, false);
	lv_obj_set_hidden(root, true);

	// The row along the top: close, the album, the queue.
	int y = top;
	close_btn = button(root, HEADER_BTN, HEADER_BTN, LV_RADIUS_CIRCLE, &icon_chevron_down, close_cb);
	lv_obj_set_pos(close_btn, PAD - 4, y + (HEADER_H - HEADER_BTN) / 2);
	btn_icons[btn_icon_n++] = lv_obj_get_child(close_btn, 0);
	queue_btn = button(root, HEADER_BTN, HEADER_BTN, 16, &icon_list_queue, queue_cb);
	lv_obj_set_pos(queue_btn, scr_w - PAD + 4 - HEADER_BTN, y + (HEADER_H - HEADER_BTN) / 2);
	btn_icons[btn_icon_n++] = lv_obj_get_child(queue_btn, 0);
	album_icon = lv_image_create(root);
	lv_image_set_src(album_icon, &icon_list_music);
	lv_obj_set_pos(album_icon, PAD + HEADER_BTN + 6, y + (HEADER_H - 30) / 2);
	album_label = text(root, &font_ui_24_bold);
	lv_obj_set_width(album_label, scr_w - 2 * PAD - 2 * HEADER_BTN - 56);
	lv_obj_set_pos(album_label, PAD + HEADER_BTN + 44, y + (HEADER_H - 30) / 2);
	scrolltext_apply(album_label);
	y += HEADER_H + GAP;

	// The sleeve, rounded, and in the same place the words when they are up.
	cover_box = plain(root);
	lv_obj_set_size(cover_box, cover_size, cover_size);
	lv_obj_set_pos(cover_box, (scr_w - cover_size) / 2, y);
	lv_obj_set_style_radius(cover_box, COVER_RADIUS, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	cover_img = lv_image_create(cover_box);
	lv_obj_center(cover_img);
	cover_note = lv_image_create(cover_box);
	lv_image_set_src(cover_note, &icon_music_note);
	lv_obj_center(cover_note);

	lyrics_view = lv_obj_create(root);
	lv_obj_remove_style_all(lyrics_view);
	lv_obj_set_size(lyrics_view, inner, cover_size);
	lv_obj_set_pos(lyrics_view, PAD, y);
	lv_obj_set_flex_flow(lyrics_view, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(lyrics_view, 14, 0);
	lv_obj_set_style_pad_ver(lyrics_view, cover_size / 3, 0);
	lv_obj_set_scroll_dir(lyrics_view, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(lyrics_view, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_hidden(lyrics_view, true);
	y += cover_size + GAP;

	// Title, the lyrics button beside it, and the artist under it.
	// Beside the title, one pill split in two: the lyrics, and the three dots
	// with the player's own menu. Square buttons inside a rounded box that
	// clips them, with a gap between showing the page, so only the outer ends
	// are round.
	int half_w = 56, pill_w = 2 * half_w + 4;
	side_pill = plain(root);
	lv_obj_set_size(side_pill, pill_w, HEADER_BTN);
	lv_obj_set_pos(side_pill, scr_w - PAD - pill_w, y + (TITLE_H + ARTIST_H - HEADER_BTN) / 2);
	lv_obj_set_style_radius(side_pill, HEADER_BTN / 2, 0);
	lv_obj_set_style_clip_corner(side_pill, true, 0);
	lyrics_btn = button(side_pill, half_w, HEADER_BTN, 6, &icon_lyrics_search, lyrics_click_cb);
	lv_obj_set_pos(lyrics_btn, 0, 0);
	more_btn = button(side_pill, half_w, HEADER_BTN, 6, &icon_ellipsis_vertical, more_cb);
	lv_obj_set_pos(more_btn, half_w + 4, 0);
	btn_icons[btn_icon_n++] = lv_obj_get_child(more_btn, 0);

	title_label = text(root, &font_ui_36_bold);
	lv_obj_set_width(title_label, inner - pill_w - 12);
	lv_obj_set_pos(title_label, PAD, y);
	scrolltext_apply(title_label);
	y += TITLE_H;
	artist_label = text(root, &font_ui_24);
	lv_obj_set_width(artist_label, inner - pill_w - 12);
	lv_obj_set_pos(artist_label, PAD, y);
	scrolltext_apply(artist_label);
	y += ARTIST_H + 8;

	// The progress line, and the row of numbers under it.
	wave_obj = plain(root);
	lv_obj_set_size(wave_obj, inner + 8, WAVE_H);
	lv_obj_set_pos(wave_obj, PAD - 4, y);
	lv_obj_set_clickable(wave_obj, true);
	lv_obj_set_ext_click_area(wave_obj, 12);
	lv_obj_add_event_cb(wave_obj, wave_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(wave_obj, wave_touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(wave_obj, wave_touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(wave_obj, wave_touch_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(wave_obj, wave_touch_cb, LV_EVENT_PRESS_LOST, NULL);
	y += WAVE_H;
	elapsed_label = text(root, &font_ui_16);
	lv_obj_set_pos(elapsed_label, PAD, y + 2);
	total_label = text(root, &font_ui_16);
	lv_obj_align(total_label, LV_ALIGN_TOP_RIGHT, -PAD, y + 2);
	format_pill = plain(root);
	lv_obj_set_size(format_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(format_pill, 12, 0);
	lv_obj_set_style_bg_opa(format_pill, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(format_pill, 12, 0);
	lv_obj_set_style_pad_ver(format_pill, 2, 0);
	lv_obj_align(format_pill, LV_ALIGN_TOP_MID, 0, y);
	format_label = text(format_pill, &font_ui_16);
	y += TIME_H + GAP;

	// The three keys: deep pills either side of the play key.
	int play_w = inner * 30 / 100, side_w = (inner - play_w - 2 * 8) / 2;
	prev_btn = button(root, side_w, KEY_H, KEY_H / 2, &icon_skip_back_large, prev_cb);
	lv_obj_set_pos(prev_btn, PAD, y);
	play_btn = button(root, play_w, KEY_H, 24, &icon_play_large, play_cb);
	lv_obj_set_pos(play_btn, PAD + side_w + 8, y);
	play_icon = lv_obj_get_child(play_btn, 0);
	next_btn = button(root, side_w, KEY_H, KEY_H / 2, &icon_skip_forward_large, next_cb);
	lv_obj_set_pos(next_btn, PAD + side_w + 8 + play_w + 8, y);
	y += KEY_H + GAP;

	// The tray: shuffle, repeat and the favourite, in a pill of their own.
	int tray_w = inner * 82 / 100;
	tray = plain(root);
	lv_obj_set_size(tray, tray_w, TRAY_H);
	lv_obj_set_pos(tray, (scr_w - tray_w) / 2, y);
	lv_obj_set_style_radius(tray, TRAY_H / 2, 0);
	lv_obj_set_style_bg_opa(tray, LV_OPA_COVER, 0);
	int chip_h = TRAY_H - 16, chip_w = (tray_w - 16 - 2 * 6) / 3;
	shuffle_chip = button(tray, chip_w, chip_h, chip_h / 2, &icon_shuffle, shuffle_cb);
	lv_obj_set_pos(shuffle_chip, 8, 8);
	repeat_chip = button(tray, chip_w, chip_h, 12, &icon_repeat_off, repeat_cb);
	lv_obj_set_pos(repeat_chip, 8 + chip_w + 6, 8);
	fav_chip = button(tray, chip_w, chip_h, chip_h / 2, &icon_heart, fav_cb);
	lv_obj_set_pos(fav_chip, 8 + 2 * (chip_w + 6), 8);
	shuffle_icon = lv_obj_get_child(shuffle_chip, 0);
	speed_text = text(shuffle_chip, &font_ui_20_bold);
	lv_obj_center(speed_text);
	lv_obj_set_hidden(speed_text, true);
	repeat_icon = lv_obj_get_child(repeat_chip, 0);
	fav_icon = lv_obj_get_child(fav_chip, 0);
	// The heart is drawn large for the easter egg; here it sits in a chip.
	lv_image_set_scale(fav_icon, 256 * 28 / (int)(icon_heart.header.h ? icon_heart.header.h : 28));

	paint();
	tick = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick);
}

void pixelplayer_set_active(bool on) {
	if (!root || on == active) {
		return;
	}
	active = on;
	lv_obj_set_hidden(root, !on);
	// The round button at the top left is this layout's way out, so the
	// player's own chevron stands down while it is up.
	back_btn_hidden_in_player(on);
	if (on) {
		lv_obj_move_foreground(root);
		shown_file[0] = '\0';
		shown_player_cover = NULL;
		shown_lyrics = NULL;
		lyrics_on = false;
		shown_kind = (player_kind_t)-1;
		refresh(true);
		lv_timer_resume(tick);
	} else {
		lv_timer_pause(tick);
		cover_drop(); // its pixels are not wanted while nothing shows them
	}
}

bool pixelplayer_is_active(void) { return active; }
