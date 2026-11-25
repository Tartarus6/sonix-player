#include "screensaver.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/clocklabel.h"
#include "src/gui/shell/pod_home.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/theme.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/power.h"
#include "src/system/device/screensaverpics.h"
#include "src/system/core/lang.h"

// Movement that turns a press into a drag, and how long the slide that follows
// the release lasts. The dismiss threshold itself is a quarter of the screen,
// applied in drag_cb; short of it the panel slides back down, like the control
// centre.
#define DRAG_ENGAGE_PX 8
#define DISMISS_ANIM_MS 180
#define FADE_ANIM_MS 260 // the slider style's way out
// The slide-to-unlock bar's height, and its knob's, which leaves 4 px around it.
#define SLIDER_H 76
#define SLIDER_KNOB (SLIDER_H - 8)

static lv_obj_t *saver;		 // the whole thing, full screen on the top layer
static lv_obj_t *cover_img;	 // the artwork, filling what is above the strip
static lv_obj_t *cover_placeholder; // the note, for a track that has none
static lv_obj_t *info_strip; // the blurred block along the bottom
static lv_obj_t *cover_box;	 // the artwork's frame, everything above the strip
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *clock_label;
static lv_obj_t *clock_bar;   // slider style: the clock and date, across the top
static lv_obj_t *date_label;
static lv_timer_t *clock_timer;

static screensaver_unlock_style_t unlock_style;
static lv_obj_t *chevron_btn;
static lv_obj_t *slider_bar;
static lv_obj_t *slider_label;
static lv_obj_t *shine_box;	  // a band of light that sweeps across the slider's text
static lv_obj_t *shine_label; // ...the same words in white, shown through it
static int shine_lx;
static lv_obj_t *quality_badge; // the format over the artwork's corner, in the slider style
static lv_obj_t *quality_label;
static int art_top; // where the artwork starts below the clock band, in the slider style
static lv_obj_t *slider_knob;
static lv_obj_t *slider_knob_icon;

typedef enum {
	SLIDER_DIR_IDLE = 0,
	SLIDER_DIR_RIGHT,
	SLIDER_DIR_LEFT,
} slider_drag_dir_t;

static slider_drag_dir_t slider_drag_dir = SLIDER_DIR_IDLE;
static int slider_start_touch_x;
static int slider_start_knob_x;
static bool slider_tracking;
static void anim_knob_x_cb(void *obj, int32_t v);
static void shine_cb(void *obj, int32_t v);
static void slider_text_opa(lv_opa_t opa);
static void shine_start(void);
static bool enabled;
static int screen_w, screen_h;
static int strip_h;

// What the strip falls back to when there is no artwork to size it from, and
// the height it always has over a picture from the folder -- there the block
// is built to order rather than borrowed, so there is nothing to measure.
#define DEFAULT_STRIP_H 250

// ---------------------------------------------------------------------------
// Pictures from the card
//
// The other thing the screensaver can show: one of the JPEGs in the card's
// Screensaver folder, a different one each time the screen lights up. Which
// files qualify and which one comes up is screensaverpics.h; what is left here
// is the drawing and the pixels it owns.
//
// Nothing is decoded on this thread. The picture for the next wake is asked of
// the cover loader as the panel goes dark and is waiting, decoded, by the time
// the screen comes back -- so a wake costs a pointer swap rather than a card
// read, a JPEG and a blur with the interface thread standing still and the
// audio decoder sharing the one core with it.
// ---------------------------------------------------------------------------

static screensaver_source_t source;
// The picture on screen and the blurred block over its lower part. Owned here,
// not by the player: freed here whenever a different one is chosen or the
// screensaver goes away.
static cover_image_t image_pic, image_strip;
static char image_file[512]; // what image_pic was built from

// The one being prepared for the next wake: `next_file` while it is out with
// the loader, and the pixels once they have been collected.
static cover_image_t next_pic, next_strip;
static char next_file[512];
static bool next_ready;	  // next_pic/next_strip hold a collected picture
static bool next_pending; // a request is out with the loader

// Resizes the strip and gives the picture whatever it should have.
//
// LVGL draws a background image at its own size, centred, and paints whatever
// is left over in the background colour, so a strip even a few pixels taller
// than the blurred block would show a black bar above it and another below.
// The height is therefore taken from the picture, leaving nothing to paint.
//
// Over an album cover the picture's box stops where the strip starts. Over a
// photograph from the folder it is the whole screen: there the strip is drawn
// ON TOP of a picture that fills the panel, which is what "the blurred part
// hides the bottom of the image" means.
//
// Cheap, and a no-op unless something has actually changed.
static int cover_box_h;
static int art_top_now(void) {
	return unlock_style == SCREENSAVER_UNLOCK_SLIDER && source == SCREENSAVER_SOURCE_ALBUM ? art_top : 0;
}
static void set_strip_height(int height) {
	if (height <= 0 || height >= screen_h) {
		return;
	}
	if (height != strip_h) {
		strip_h = height;
		lv_obj_set_height(info_strip, strip_h);
		lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
	// With the slider, the clock has a band of its own across the top and the
	// artwork sits between it and the strip, rather than under the clock.
	int top = art_top_now();
	int want = source == SCREENSAVER_SOURCE_IMAGES ? screen_h : screen_h - strip_h - top;
	if (want != cover_box_h || lv_obj_get_y(cover_box) != top) {
		cover_box_h = want;
		lv_obj_set_height(cover_box, cover_box_h);
		lv_obj_set_y(cover_box, top);
	}
	if (clock_bar) {
		// Solid over an album's artwork; see-through over a photograph.
		lv_obj_set_style_bg_opa(clock_bar, source == SCREENSAVER_SOURCE_IMAGES ? LV_OPA_50 : LV_OPA_COVER, 0);
	}
}

bool screensaver_is_visible(void) { return saver && !lv_obj_is_hidden(saver); }
bool screensaver_get_enabled(void) { return enabled; }

screensaver_source_t screensaver_source(void) { return source; }

screensaver_unlock_style_t screensaver_unlock_style(void) { return unlock_style; }

static void screensaver_apply_unlock_style(void);

void screensaver_set_unlock_style(screensaver_unlock_style_t style) {
	unlock_style = (style == SCREENSAVER_UNLOCK_SLIDER) ? SCREENSAVER_UNLOCK_SLIDER : SCREENSAVER_UNLOCK_SWIPE_UP;
	config_set_int("screen", "screensaver_unlock_style", (int)unlock_style);
	config_save();
	screensaver_apply_unlock_style();
}

static void request_next(void);
static void drop_next(void);

void screensaver_set_source(screensaver_source_t chosen) {
	source = chosen == SCREENSAVER_SOURCE_IMAGES ? SCREENSAVER_SOURCE_IMAGES : SCREENSAVER_SOURCE_ALBUM;
	config_set_int("screen", "screensaver_source", (int)source);
	config_save();
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Asked for now rather than at the first screen-off, so the first wake
		// after the option is chosen already has a picture to show.
		request_next();
	} else {
		drop_next();
	}
}

void screensaver_set_card_root(const char *root) { screensaverpics_set_root(root); }

bool screensaver_has_images(void) { return screensaverpics_any(screen_w, screen_h); }

// Picks the next picture and puts it out to decode. Called as the panel goes
// dark, which is the whole point: the card read and the JPEG happen on the
// loader thread at background priority over a screen nobody is looking at.
//
// A no-op while one is already in hand or in flight, so the repeated wake-hook
// calls cost nothing.
static void request_next(void) {
	if (source != SCREENSAVER_SOURCE_IMAGES || next_ready || next_pending) {
		return;
	}
	char chosen[sizeof(next_file)];
	if (!screensaverpics_pick(image_file, screen_w, screen_h, chosen, sizeof(chosen))) {
		// The folder gave nothing at all -- emptied, or the card taken out.
		// What is already up stays up: it is still a picture, and black is not.
		return;
	}
	snprintf(next_file, sizeof(next_file), "%s", chosen);
	next_pending = true;
	coverloader_request_screensaver(next_file, screen_w, screen_h, screen_w, DEFAULT_STRIP_H);
}

// Takes delivery of a finished decode, if there is one. Cheap and non-blocking:
// called at every wake and on the tick while the screensaver is up.
static void collect_next(void) {
	if (!next_pending) {
		return;
	}
	bool finished = false;
	cover_image_t pic = {0}, strip = {0};
	bool found = coverloader_take_screensaver(&pic, &strip, &finished);
	if (!finished) {
		return;
	}
	next_pending = false;
	if (!found) {
		// It passed the header check and still would not decode. Forgotten, so
		// the next request picks another rather than retrying this one.
		next_file[0] = '\0';
		return;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_pic = pic;
	next_strip = strip;
	next_ready = true;
}

static void drop_next(void) {
	if (next_pending) {
		coverloader_release_screensaver();
		next_pending = false;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_ready = false;
	next_file[0] = '\0';
}

static void drop_image(void) {
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_file[0] = '\0';
}

void screensaver_set_enabled(bool on) {
	enabled = on;
	config_set_bool("screen", "screensaver", on);
	config_save();
	if (!on) {
		if (screensaver_is_visible()) {
			screensaver_hide(); // frees what is held on its way out
		} else {
			drop_next();
		}
	}
}

// ---------------------------------------------------------------------------
// what it shows
// ---------------------------------------------------------------------------

static void refresh_clock(void) {
	clocklabel_refresh(clock_label);
	if (date_label) {
		char text[48] = "";
		time_t now = time(NULL);
		struct tm tm_now;
		if (now > 1000000000 && localtime_r(&now, &tm_now)) {
			strftime(text, sizeof(text), "%A, %B %-d", &tm_now);
		}
		lv_label_set_text(date_label, text);
	}
}

// What refresh_content() last drew, so the tick below can tell an idle moment
// from a track that changed while the screensaver was up.
static char shown_file[512];
static const void *shown_cover;

static void refresh_content(void);

// Runs while the screensaver is up. The clock is the obvious reason, but the
// important one is the track: the player frees its artwork at every track
// change and calls screensaver_release_artwork(), which blanks the picture, so
// this check is what puts the new one back.
static void clock_timer_cb(lv_timer_t *timer) {
	(void)timer;

	// Takes delivery of a decode that landed after the wake had already given
	// up on it -- a short dark spell, or a slow card. It is only stored, never
	// swapped in: the picture must not change while it is being looked at.
	collect_next();

	if (!screensaver_is_visible()) {
		refresh_clock();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	const void *cover = player_cover_image();

	const char *now = state.live ? state.metadata.title : state.current_file;
	if (strcmp(now, shown_file) != 0 || cover != shown_cover) {
		refresh_content(); // refreshes the clock on its way through
		return;
	}

	refresh_clock();
}

// Puts the prepared picture on screen. Unlike the album source this owns its
// pixels, so the swap is the careful part: LVGL keeps drawing from the buffer
// it was given, and freeing the old one while it is still the widget's source
// is a use-after-free at the next redraw. Off the screen first, then free.
static void promote_next(void) {
	if (!next_ready) {
		return;
	}
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_pic = next_pic;
	image_strip = next_strip;
	snprintf(image_file, sizeof(image_file), "%s", next_file);

	memset(&next_pic, 0, sizeof(next_pic));
	memset(&next_strip, 0, sizeof(next_strip));
	next_ready = false;
	next_file[0] = '\0';
}

// Draws whatever is currently held. Nothing is decoded here.
static void draw_from_folder(void) {
	set_strip_height(DEFAULT_STRIP_H);

	if (image_pic.pixels) {
		lv_image_set_src(cover_img, &image_pic.dsc);
		// Already built at screen size by cover_load_screensaver_images: no
		// zoom to apply, and none to leave over from the album source.
		lv_image_set_scale(cover_img, 256);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		// An empty folder, or a card that is not there. The note in the middle
		// of a black screen, which is what the album source shows for a track
		// with no artwork -- the same screen, missing the same thing.
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (image_strip.pixels) {
		lv_obj_set_style_bg_image_src(info_strip, &image_strip.dsc, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}
}

// The artwork is the player's own, already decoded for this track: borrowed
// for as long as the screensaver is up and dropped the moment it goes down,
// so a track change while it is hidden can never leave a stale pointer here.
// A picture from the folder is the other way round -- owned here, decoded here
// and freed here.
// The format badge over the artwork: the file's extension, and its bit depth
// when the library has it ("FLAC 24bit"). Only in the slider style, over an
// album's artwork, for a file.
static void badge_update(const device_state_t *state) {
	if (!quality_badge) {
		return;
	}
	bool want = unlock_style == SCREENSAVER_UNLOCK_SLIDER && source == SCREENSAVER_SOURCE_ALBUM && !state->live &&
				state->current_file[0] != '\0';
	const char *dot = want ? strrchr(state->current_file, '.') : NULL;
	if (!dot || !dot[1] || strlen(dot + 1) > 5) {
		want = false;
	}
	if (want) {
		char ext[8];
		size_t n = 0;
		for (const char *c = dot + 1; *c && n < sizeof(ext) - 1; c++) {
			ext[n++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
		}
		ext[n] = '\0';
		int bits = 0;
		library_track_quality(state->current_file, NULL, &bits);
		if (bits > 0) {
			lv_label_set_text_fmt(quality_label, "%s %dbit", ext, bits);
		} else {
			lv_label_set_text(quality_label, ext);
		}
	}
	lv_obj_set_hidden(quality_badge, !want);
}

static void refresh_content(void) {
	device_state_t state;
	device_state_get(&state);

	// Recorded before anything is drawn from it, so what the tick above
	// compares against is exactly what ended up on screen. A live stream has no
	// file, so the station name stands in; without it every radio would look
	// identical to the tick and a change of station would never redraw.
	snprintf(shown_file, sizeof(shown_file), "%s",
			 state.live ? state.metadata.title : state.current_file);

	// The placeholder behind a missing cover says what kind of thing is
	// playing: a note for a track, a radio for a stream.
	if (cover_placeholder) {
		lv_image_set_src(cover_placeholder, state.live ? &icon_radio_player : &icon_music_note);
	}

	if (state.live) {
		scrolltext_set(title_label, state.metadata.title);
		scrolltext_set(artist_label, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		scrolltext_set(title_label, state.metadata.title[0] ? state.metadata.title
															: (slash ? slash + 1 : state.current_file));
		const char *artist = metadata_shown_artist(&state.metadata);
		scrolltext_set(artist_label, artist);
	} else {
		scrolltext_set(title_label, "");
		scrolltext_set(artist_label, "");
	}

	// Recorded whatever is being drawn: the tick above compares it to notice a
	// track change, and in Images mode a track change still has to redraw the
	// title -- it just must not change the picture.
	shown_cover = player_cover_image();
	badge_update(&state);

	if (source == SCREENSAVER_SOURCE_IMAGES) {
		draw_from_folder();
		refresh_clock();
		return;
	}

	// The strip first: the artwork below is scaled to whatever is left above
	// it, so its height has to be settled before that sum is done.
	const lv_image_dsc_t *backdrop = player_backdrop_image();
	// The player's blurred copy is as tall as whatever shows it, and one
	// arrangement shows it behind the whole screen: a strip that tall is the
	// screen, so that one keeps the default height and takes the top of the
	// picture instead.
	int strip_from_backdrop = backdrop ? (int)backdrop->header.h : 0;
	set_strip_height(strip_from_backdrop > 0 && strip_from_backdrop < screen_h ? strip_from_backdrop
																			  : DEFAULT_STRIP_H);

	const lv_image_dsc_t *cover = shown_cover;
	if (cover) {
		lv_image_set_src(cover_img, cover);
		// Fill the whole area above the strip, cropping the overflow: a
		// letterboxed cover with black bars would look like a bug, not a
		// screensaver.
		int32_t zoom_w = (256 * screen_w) / (int32_t)cover->header.w;
		int32_t zoom_h = (256 * (screen_h - strip_h - art_top_now())) / (int32_t)cover->header.h;
		lv_image_set_scale(cover_img, zoom_w > zoom_h ? zoom_w : zoom_h);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (backdrop) {
		lv_obj_set_style_bg_image_src(info_strip, backdrop, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		// No artwork at all: a plain dark strip carries the text instead.
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}

	refresh_clock();
}

// ---------------------------------------------------------------------------
// raising and dismissing
// ---------------------------------------------------------------------------

static void anim_y_cb(void *obj, int32_t v);
static void fade_cb(void *obj, int32_t v);
static bool fading_out;

void screensaver_show(void) {
	if (!saver || !enabled || screensaver_is_visible()) {
		return;
	}

	collect_next();
	promote_next();
	screensaver_apply_unlock_style();
	refresh_content();
	lv_anim_delete(saver, anim_y_cb);
	lv_anim_delete(saver, fade_cb);
	fading_out = false;
	lv_obj_set_style_opa(saver, LV_OPA_COVER, 0);
	lv_obj_set_y(saver, 0);
	if (slider_knob) {
		lv_anim_delete(slider_knob, anim_knob_x_cb);
		lv_obj_set_x(slider_knob, 4);
		if (slider_knob_icon) {
			lv_image_set_src(slider_knob_icon, &icon_chevron_right);
		}
	}
	if (slider_label) {
		lv_label_set_text(slider_label, tr("screensaver_slide_to_unlock"));
		lv_label_set_text(shine_label, tr("screensaver_slide_to_unlock"));
		slider_text_opa(LV_OPA_70);
	}
	slider_drag_dir = SLIDER_DIR_IDLE;
	slider_tracking = false;
	screensaver_apply_unlock_style();
	lv_obj_set_hidden(saver, false);
	lv_obj_move_foreground(saver);
	shine_start();
	lv_timer_resume(clock_timer);
}

// Everything that has to happen once it is really gone, whether it was
// dismissed by a tap, by a finished swipe, or by switching the option off.
static void finish_hide(void) {
	if (shine_box) {
		lv_anim_delete(shine_box, shine_cb);
	}
	lv_obj_set_hidden(saver, true);
	lv_obj_set_y(saver, 0);
	lv_timer_pause(clock_timer);
	// Let go of the player's pixels: it frees them at the next track change,
	// and nothing here may still be pointing at them by then. The picture owned
	// here goes too -- about a megabyte, and the device is in use again, which
	// is exactly when that megabyte is worth having back.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	drop_image();
	// The one being prepared goes too. It is another megabyte, the device is in
	// use again, and the next screen-off asks for a fresh one anyway.
	drop_next();
	power_notify_activity(); // dismissing it counts as using the device
}

static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// Where the slide currently running is headed. Held explicitly rather than read
// back from the final coordinate: LVGL can finish an animation a pixel or four
// short of its end value, which would answer "did it leave?" with a no and
// leave the screensaver technically visible off-screen forever.
static bool sliding_out;

static void slide_done_cb(lv_anim_t *a) {
	(void)a;
	if (sliding_out) {
		finish_hide();
	} else {
		lv_obj_set_y(saver, 0); // exactly home, whatever the last frame did
	}
}

// Animates the last stretch after the finger lets go: up and away, or back
// down into place.
static void slide_to(int y) {
	lv_anim_delete(saver, anim_y_cb);
	sliding_out = (y != 0);
	if (sliding_out) {
		pod_home_play_enter(); // the icons fly in underneath as the screensaver leaves
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, saver);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(saver), y);
	lv_anim_set_duration(&a, DISMISS_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	lv_anim_start(&a);
}

bool screensaver_prepare(void) {
	if (!saver || !enabled) {
		return false;
	}
	// Called once as the panel goes dark and again as it lights up, and the two
	// do different work. Going dark is when the next picture is asked for: the
	// screen is about to be off, the loader thread has it to itself, and nobody
	// is waiting. Lighting up is when it is collected and swapped in -- no card,
	// no decode, nothing between the button and the backlight.
	bool waking = power_screen_is_on();

	if (screensaver_is_visible()) {
		collect_next();
		if (waking) {
			promote_next();
		}
		refresh_content();
	} else {
		screensaver_show(); // collects and promotes on its way through
	}

	if (!waking) {
		request_next();
	}
	return screensaver_is_visible();
}

void screensaver_release_artwork(void) {
	if (!cover_img) {
		return;
	}
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Nothing here is the player's: it can free its artwork without this
		// screen losing anything, and blanking the picture would put a black
		// screen up for no reason.
		return;
	}

	// Whatever was on screen is gone; the tick above has to see a difference
	// so it puts the new track's picture up rather than leaving black.
	shown_cover = NULL;
	shown_file[0] = '\0';
	// Whatever happens next, nothing here may still be pointing at the
	// player's buffers when it frees them.
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_hidden(cover_img, true);
	lv_obj_set_hidden(cover_placeholder, false);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
}

static void fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void fade_done_cb(lv_anim_t *an) {
	(void)an;
	fading_out = false;
	finish_hide();
	lv_obj_set_style_opa(saver, LV_OPA_COVER, 0); // ready for the next time it is shown
}

// Dismisses it by fading to nothing over the home screen, where the icons are
// already arriving.
static void fade_out(void) {
	if (fading_out) {
		return;
	}
	fading_out = true;
	lv_anim_delete(saver, anim_y_cb);
	pod_home_play_enter();
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, saver);
	lv_anim_set_exec_cb(&a, fade_cb);
	lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
	lv_anim_set_duration(&a, FADE_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, fade_done_cb);
	lv_anim_start(&a);
}

void screensaver_hide(void) {
	if (!saver || !screensaver_is_visible()) {
		return;
	}
	// The slider's way out is a fade, like the iPod's: there is no finger to
	// follow, so nothing slides. The swipe-up style keeps its slide.
	if (unlock_style == SCREENSAVER_UNLOCK_SLIDER) {
		fade_out();
	} else {
		slide_to(-screen_h);
	}
}

// The swipe up, following the finger the whole way: the screensaver slides
// with it, and the release decides whether it carries on off the top or falls
// back into place. Same feel as the control centre.
static void drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	if (unlock_style == SCREENSAVER_UNLOCK_SLIDER) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		lv_anim_delete(saver, anim_y_cb);
		tracking = true;
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dy = point.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (dy > -DRAG_ENGAGE_PX) {
				return; // not (yet) an upward drag
			}
			engaged = true;
		}
		// Only upward: dragging down does not pull it off the bottom.
		lv_obj_set_y(saver, dy < 0 ? dy : 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		// A quarter of the screen of travel and it goes; short of that it
		// falls back.
		slide_to(dy < -screen_h / 4 ? -screen_h : 0);
	}
}

// The words, and the light passing over them, fade together as the knob moves.
static void slider_text_opa(lv_opa_t opa) {
	lv_obj_set_style_text_opa(slider_label, opa, 0);
	if (shine_label) {
		lv_obj_set_style_text_opa(shine_label, opa, 0);
	}
}

static void shine_cb(void *obj, int32_t v) {
	(void)obj;
	lv_obj_set_x(shine_box, v);
	lv_obj_set_x(shine_label, shine_lx - v); // the white words stay where the grey ones are
}

static void shine_start(void) {
	if (!shine_box || unlock_style != SCREENSAVER_UNLOCK_SLIDER) {
		return;
	}
	lv_obj_update_layout(slider_bar);
	shine_lx = lv_obj_get_x(slider_label);
	int w = lv_obj_get_width(slider_label);
	lv_obj_set_y(shine_label, lv_obj_get_y(slider_label));
	lv_anim_delete(shine_box, shine_cb);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, shine_box);
	lv_anim_set_exec_cb(&a, shine_cb);
	lv_anim_set_values(&a, shine_lx - 80, shine_lx + w);
	lv_anim_set_duration(&a, 1900);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_repeat_delay(&a, 700);
	lv_anim_set_path_cb(&a, lv_anim_path_linear);
	lv_anim_start(&a);
}

static void anim_knob_x_cb(void *obj, int32_t v) {
	lv_obj_set_x((lv_obj_t *)obj, v);
}

static void anim_knob_done_cb(lv_anim_t *a) {
	(void)a;
	if (slider_knob) {
		lv_obj_set_x(slider_knob, 4);
	}
	if (slider_knob_icon) {
		lv_image_set_src(slider_knob_icon, &icon_chevron_right);
	}
	if (slider_label) {
		slider_text_opa(LV_OPA_70);
	}
	slider_drag_dir = SLIDER_DIR_IDLE;
}

static void anim_knob_to(int target_x) {
	if (!slider_knob) {
		return;
	}
	lv_anim_delete(slider_knob, anim_knob_x_cb);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, slider_knob);
	lv_anim_set_exec_cb(&a, anim_knob_x_cb);
	lv_anim_set_values(&a, lv_obj_get_x(slider_knob), target_x);
	lv_anim_set_duration(&a, 180);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, anim_knob_done_cb);
	lv_anim_start(&a);

	if (slider_label) {
		slider_text_opa(LV_OPA_70);
	}
}

static void slider_drag_cb(lv_event_t *e) {
	lv_indev_t *indev = lv_indev_active();
	if (!indev || !slider_bar || !slider_knob) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	lv_point_t p;
	lv_indev_get_point(indev, &p);

	int bar_w = lv_obj_get_width(slider_bar);
	int knob_w = lv_obj_get_width(slider_knob);
	int min_x = 4;
	int max_x = bar_w - knob_w - 4;
	int total_travel = max_x - min_x;
	if (total_travel <= 0) {
		total_travel = 1;
	}

	if (code == LV_EVENT_PRESSED) {
		lv_area_t bar_coords;
		lv_obj_get_coords(slider_bar, &bar_coords);
		int touch_x = p.x - bar_coords.x1;

		lv_anim_delete(slider_knob, anim_knob_x_cb);
		slider_tracking = true;

		if (touch_x > bar_w / 2) {
			slider_drag_dir = SLIDER_DIR_LEFT;
			slider_start_touch_x = p.x;
			slider_start_knob_x = max_x;
			lv_obj_set_x(slider_knob, max_x);
			if (slider_knob_icon) {
				lv_image_set_src(slider_knob_icon, &icon_chevron_left);
			}
		} else {
			slider_drag_dir = SLIDER_DIR_RIGHT;
			slider_start_touch_x = p.x;
			slider_start_knob_x = min_x;
			lv_obj_set_x(slider_knob, min_x);
			if (slider_knob_icon) {
				lv_image_set_src(slider_knob_icon, &icon_chevron_right);
			}
		}
		if (slider_label) {
			slider_text_opa(LV_OPA_70);
		}
		return;
	}

	if (!slider_tracking) {
		return;
	}

	if (code == LV_EVENT_PRESSING) {
		int dx = p.x - slider_start_touch_x;
		int cur_x = slider_start_knob_x + dx;
		if (cur_x < min_x) {
			cur_x = min_x;
		}
		if (cur_x > max_x) {
			cur_x = max_x;
		}
		lv_obj_set_x(slider_knob, cur_x);

		int travel = (slider_drag_dir == SLIDER_DIR_RIGHT) ? (cur_x - min_x) : (max_x - cur_x);
		int progress = (travel * 255) / total_travel;
		int opa = (int)LV_OPA_70 - (progress * (int)LV_OPA_70) / 180;
		if (opa < 0) {
			opa = 0;
		}
		if (slider_label) {
			slider_text_opa((lv_opa_t)opa);
		}
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		slider_tracking = false;
		int cur_x = lv_obj_get_x(slider_knob);
		bool unlocked = false;

		if (slider_drag_dir == SLIDER_DIR_RIGHT) {
			if (cur_x >= min_x + (total_travel * 7) / 10) {
				unlocked = true;
			}
		} else if (slider_drag_dir == SLIDER_DIR_LEFT) {
			if (cur_x <= min_x + (total_travel * 3) / 10) {
				unlocked = true;
			}
		}

		if (unlocked) {
			screensaver_hide();
			lv_obj_set_x(slider_knob, min_x);
			if (slider_knob_icon) {
				lv_image_set_src(slider_knob_icon, &icon_chevron_right);
			}
			if (slider_label) {
				slider_text_opa(LV_OPA_70);
			}
			slider_drag_dir = SLIDER_DIR_IDLE;
		} else {
			anim_knob_to(min_x);
		}
	}
}

static void screensaver_apply_unlock_style(void) {
	if (!saver) {
		return;
	}
	if (unlock_style == SCREENSAVER_UNLOCK_SLIDER) {
		if (chevron_btn) {
			lv_obj_set_hidden(chevron_btn, true);
		}
		if (slider_bar) {
			lv_obj_set_hidden(slider_bar, false);
		}
		if (info_strip) {
			lv_obj_set_style_pad_bottom(info_strip, SLIDER_H + 32, 0);
		}
		// Like the iPod touch lock screen: the time and date at the top.
		if (clock_bar && clock_label) {
			lv_obj_set_parent(clock_label, clock_bar);
			lv_obj_move_to_index(clock_label, 0);
			lv_obj_set_hidden(clock_bar, false);
			lv_obj_update_layout(clock_bar);
			art_top = lv_obj_get_height(clock_bar);
		}
	} else {
		art_top = 0;
		if (clock_bar && clock_label) {
			lv_obj_set_parent(clock_label, info_strip);
			lv_obj_set_hidden(clock_bar, true);
		}
		if (chevron_btn) {
			lv_obj_set_hidden(chevron_btn, false);
		}
		if (slider_bar) {
			lv_obj_set_hidden(slider_bar, true);
		}
		if (info_strip) {
			lv_obj_set_style_pad_bottom(info_strip, 18, 0);
		}
	}
	// The artwork's frame depends on whether the clock has a band of its own.
	cover_box_h = -1;
	set_strip_height(strip_h);
}

void screensaver_init(gui_config_t *cfg) {
	screen_w = cfg->screen_width;
	screen_h = cfg->screen_height;
	strip_h = DEFAULT_STRIP_H;

	screensaver_set_card_root(cfg->sd_root_path);
	enabled = config_get_bool("screen", "screensaver", false);
	source = config_get_int("screen", "screensaver_source", SCREENSAVER_SOURCE_ALBUM) == SCREENSAVER_SOURCE_IMAGES
				 ? SCREENSAVER_SOURCE_IMAGES
				 : SCREENSAVER_SOURCE_ALBUM;
	unlock_style = config_get_int("screen", "screensaver_unlock_style", SCREENSAVER_UNLOCK_SWIPE_UP) == SCREENSAVER_UNLOCK_SLIDER
					   ? SCREENSAVER_UNLOCK_SLIDER
					   : SCREENSAVER_UNLOCK_SWIPE_UP;
	cover_box_h = screen_h - strip_h;

	saver = lv_obj_create(lv_layer_top());
	lv_obj_set_size(saver, screen_w, screen_h);
	lv_obj_set_pos(saver, 0, 0);
	lv_obj_set_style_bg_color(saver, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(saver, 0, 0);
	lv_obj_set_style_radius(saver, 0, 0);
	lv_obj_set_style_pad_all(saver, 0, 0);
	lv_obj_set_scrollable(saver, false);
	lv_obj_set_clickable(saver, true);
	lv_obj_set_hidden(saver, true);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The artwork: everything above the strip, centre-cropped to fill it.
	cover_box = lv_obj_create(saver);
	lv_obj_set_size(cover_box, screen_w, screen_h - strip_h);
	lv_obj_set_pos(cover_box, 0, 0);
	lv_obj_set_style_bg_color(cover_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cover_box, 0, 0);
	lv_obj_set_style_radius(cover_box, 0, 0);
	lv_obj_set_style_pad_all(cover_box, 0, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_set_scrollable(cover_box, false);
	lv_obj_set_event_bubble(cover_box, true);
	lv_obj_set_clickable(cover_box, false);

	// Nothing to show for a track with no artwork: the same note the player
	// puts in the same place, so the screensaver is recognisably the player
	// with the controls taken away rather than a different screen that has
	// gone wrong.
	cover_placeholder = lv_image_create(cover_box);
	lv_image_set_src(cover_placeholder, &icon_music_note);
	lv_obj_set_style_image_recolor(cover_placeholder, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder);

	cover_img = lv_image_create(cover_box);
	lv_obj_center(cover_img);
	lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);

	// The format of the file, small, in the artwork's lower right corner.
	quality_badge = lv_obj_create(cover_box);
	lv_obj_set_size(quality_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_align(quality_badge, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
	lv_obj_set_style_bg_color(quality_badge, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(quality_badge, LV_OPA_70, 0);
	lv_obj_set_style_border_width(quality_badge, 0, 0);
	lv_obj_set_style_radius(quality_badge, 6, 0);
	lv_obj_set_style_pad_hor(quality_badge, 8, 0);
	lv_obj_set_style_pad_ver(quality_badge, 3, 0);
	lv_obj_set_scrollable(quality_badge, false);
	lv_obj_set_clickable(quality_badge, false);
	lv_obj_set_hidden(quality_badge, true);
	quality_label = lv_label_create(quality_badge);
	lv_label_set_text(quality_label, "");
	lv_obj_set_style_text_font(quality_label, &font_ui_16, 0);
	lv_obj_set_style_text_color(quality_label, lv_color_white(), 0);

	// The blurred block: the same picture the player draws its controls on.
	info_strip = lv_obj_create(saver);
	lv_obj_set_size(info_strip, screen_w, strip_h);
	lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(info_strip, 0, 0);
	lv_obj_set_style_radius(info_strip, 0, 0);
	lv_obj_set_style_pad_hor(info_strip, cfg->padding + 6, 0);
	lv_obj_set_style_pad_ver(info_strip, 18, 0);
	lv_obj_set_style_pad_gap(info_strip, 2, 0);
	lv_obj_set_scrollable(info_strip, false);
	lv_obj_set_event_bubble(info_strip, true);
	lv_obj_set_clickable(info_strip, false);
	lv_obj_set_flex_flow(info_strip, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The blurred backdrop is always the dark treatment, so the text on it is
	// light in both themes.
	title_label = lv_label_create(info_strip);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_set_style_text_font(title_label, &font_ui_28, 0);
	lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(info_strip);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(artist_label, lv_color_make(200, 200, 200), 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(artist_label);

	// The time, in the largest figures the screen has.
	clock_label = clocklabel_create(info_strip, &font_ui_72, 72, &font_ui_32, 32);
	lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
	lv_obj_set_style_pad_top(clock_label, 6, 0);

	// The way out, spelled out: swipe up. A sign, not a button -- tapping it
	// does nothing, since the screensaver is only dismissed by dragging -- so it
	// is not clickable and a finger landing on it talks to the saver underneath,
	// where the drag gesture lives.
	chevron_btn = lv_obj_create(saver);
	lv_obj_set_size(chevron_btn, 80, 52);
	lv_obj_align(chevron_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
	lv_obj_set_style_bg_opa(chevron_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chevron_btn, 0, 0);
	lv_obj_set_style_shadow_width(chevron_btn, 0, 0);
	lv_obj_set_style_pad_all(chevron_btn, 0, 0);
	lv_obj_set_clickable(chevron_btn, false);
	lv_obj_set_scrollable(chevron_btn, false);

	lv_obj_t *chevron = lv_image_create(chevron_btn);
	lv_image_set_src(chevron, &icon_chevron_up);
	lv_obj_set_style_image_recolor(chevron, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(chevron, LV_OPA_70, 0);
	lv_obj_center(chevron);

	// The top of the iPod touch lock screen: a dark translucent band with the
	// date under the time. The clock moves in here while the slider is the way
	// out (see screensaver_apply_unlock_style).
	clock_bar = lv_obj_create(saver);
	lv_obj_set_size(clock_bar, screen_w, LV_SIZE_CONTENT);
	lv_obj_align(clock_bar, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(clock_bar, lv_color_make(17, 19, 23), 0);
	lv_obj_set_style_bg_opa(clock_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(clock_bar, 0, 0);
	lv_obj_set_style_radius(clock_bar, 0, 0);
	lv_obj_set_style_pad_hor(clock_bar, 0, 0);
	lv_obj_set_style_pad_top(clock_bar, 22, 0);
	lv_obj_set_style_pad_bottom(clock_bar, 20, 0);
	lv_obj_set_style_pad_gap(clock_bar, 0, 0);
	lv_obj_set_scrollable(clock_bar, false);
	lv_obj_set_clickable(clock_bar, false);
	lv_obj_set_flex_flow(clock_bar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clock_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(clock_bar, true);

	date_label = lv_label_create(clock_bar);
	lv_obj_set_style_text_font(date_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(date_label, lv_color_make(229, 231, 235), 0);
	lv_obj_set_clickable(date_label, false);

	// iPod Touch classic slide-to-unlock bar
	slider_bar = lv_obj_create(saver);
	lv_obj_set_size(slider_bar, 400, SLIDER_H);
	lv_obj_align(slider_bar, LV_ALIGN_BOTTOM_MID, 0, -16);
	lv_obj_set_style_bg_color(slider_bar, lv_color_make(42, 45, 51), 0);
	lv_obj_set_style_bg_opa(slider_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(slider_bar, 0, 0);
	lv_obj_set_style_radius(slider_bar, SLIDER_H / 2, 0);
	lv_obj_set_style_pad_all(slider_bar, 0, 0);
	lv_obj_set_style_clip_corner(slider_bar, true, 0);
	lv_obj_set_scrollable(slider_bar, false);
	lv_obj_set_clickable(slider_bar, true);
	lv_obj_add_event_cb(slider_bar, slider_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(slider_bar, slider_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(slider_bar, slider_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(slider_bar, slider_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	slider_label = lv_label_create(slider_bar);
	lv_label_set_text(slider_label, tr("screensaver_slide_to_unlock"));
	lv_obj_set_style_text_font(slider_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(slider_label, lv_color_make(156, 163, 175), 0);
	lv_obj_set_style_text_opa(slider_label, LV_OPA_70, 0);
	lv_obj_set_style_text_align(slider_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(slider_label, LV_ALIGN_CENTER, SLIDER_KNOB / 2, 0); // clear of the knob
	lv_obj_set_clickable(slider_label, false);

	// A band of light that sweeps across the words, as on the lock screen: the
	// same words in white, shown only inside a moving window.
	shine_box = lv_obj_create(slider_bar);
	lv_obj_set_size(shine_box, 80, SLIDER_H);
	lv_obj_set_pos(shine_box, 0, 0);
	lv_obj_set_style_bg_opa(shine_box, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(shine_box, 0, 0);
	lv_obj_set_style_radius(shine_box, 0, 0);
	lv_obj_set_style_pad_all(shine_box, 0, 0);
	lv_obj_set_scrollable(shine_box, false);
	lv_obj_set_clickable(shine_box, false);
	shine_label = lv_label_create(shine_box);
	lv_label_set_text(shine_label, tr("screensaver_slide_to_unlock"));
	lv_obj_set_style_text_font(shine_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(shine_label, lv_color_white(), 0);
	lv_obj_set_style_text_opa(shine_label, LV_OPA_70, 0);
	lv_obj_set_clickable(shine_label, false);

	slider_knob = lv_obj_create(slider_bar);
	lv_obj_set_size(slider_knob, SLIDER_KNOB, SLIDER_KNOB);
	lv_obj_set_pos(slider_knob, 4, 4);
	lv_obj_set_style_bg_color(slider_knob, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(slider_knob, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(slider_knob, 0, 0);
	lv_obj_set_style_radius(slider_knob, SLIDER_KNOB / 2, 0);
	lv_obj_set_style_pad_all(slider_knob, 0, 0);
	lv_obj_set_style_shadow_width(slider_knob, 0, 0);
	lv_obj_set_scrollable(slider_knob, false);
	lv_obj_set_clickable(slider_knob, false);

	slider_knob_icon = lv_image_create(slider_knob);
	lv_image_set_src(slider_knob_icon, &icon_chevron_right);
	lv_obj_set_style_image_recolor(slider_knob_icon, lv_color_make(80, 80, 90), 0);
	lv_obj_set_style_image_recolor_opa(slider_knob_icon, LV_OPA_COVER, 0);
	lv_obj_center(slider_knob_icon);
	lv_obj_set_clickable(slider_knob_icon, false);

	screensaver_apply_unlock_style();

	// One second, not ten: the clock only needs a minute's resolution, but the
	// same tick is what notices a track change, and a screensaver that shows
	// the previous song for ten seconds is a screensaver that looks broken.
	clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
	lv_timer_pause(clock_timer);
	// It is drawn into the framebuffer before the blank so the next wake opens
	// on it -- but with the panel off nothing is scanned out, so ticking it
	// once a second all night buys nothing.
	power_pause_in_standby(clock_timer);
}
