#ifndef SCROLLTEXT_H
#define SCROLLTEXT_H

#include "lvgl/lvgl.h"

#include <stdbool.h>

// A label that scrolls its text sideways when it does not fit -- but only
// after standing still long enough to be read first, and pausing again at the
// end of every pass.
//
// LVGL's LV_LABEL_LONG_SCROLL_CIRCULAR starts moving the instant the text is
// set, so a long track title slides away before it can be read. LVGL has no
// direct setting for the delay, but a label's scroll animation copies
// `act_time` and `repeat_delay` from the "animation template" style property,
// and a negative act_time is how lv_anim expresses a delay -- so the pause is
// expressed there.
//
// It also scrolls at a constant speed rather than in a constant time. LVGL
// takes a duration for the whole pass, so on its own a long text goes past
// faster than a short one. This sizes the text and sets the duration to match,
// which is why every text must be set through scrolltext_set() and not
// lv_label_set_text().
//
// A text that does not fit is faded out at the edge rather than cut, so what is
// there reads as "there is more" instead of as a half-drawn letter. The right
// end always fades; the left one only while the text is actually travelling,
// because standing still the first letter of the title is at that edge and has
// to be read. See the note in scrolltext.c: it is an alpha mask over the
// label's own layer, which is what lets it work over the player's blurred
// artwork as well as over a flat card.
//
// Used by every place a track name is shown in a box it may outgrow: the
// player, the control centre and the screensaver.
void scrolltext_apply(lv_obj_t *label);

// Sets a scrolling label's text only when it has actually changed. Line breaks
// and other control characters are shown as spaces: the label is one line.
//
// LVGL rebuilds the scroll animation from scratch on every
// lv_label_set_text(), delay included. A card that repaints itself on a timer
// -- the control centre polls what is playing every 700 ms -- would therefore
// push the start of the scroll back for ever and the text would never move.
// Setting it only on a real change also saves the layout pass.
void scrolltext_set(lv_obj_t *label, const char *text);

// Whether list rows scroll long names. Now Playing and the other always-on
// labels ignore this setting.
bool scrolltext_auto_enabled(void);

// The three speeds offered by the settings UI, in pixels per second. Kept
// here so the pills and the runtime value cannot drift apart.
#define SCROLL_SPEED_SLOW 35
#define SCROLL_SPEED_NORMAL 55
#define SCROLL_SPEED_FAST 80

// The current speed in pixels per second, clamped to 20..120.
int scrolltext_speed_px_s(void);

// Applies the marquee to a list row's label. The row obeys the switch, unlike
// the always-on surfaces; its enabled state survives park and unpark cycles.
void scrolltext_apply_list(lv_obj_t *label);

// Called after either setting changes. Re-modes list rows and re-measures
// durations for every scrolling label.
void scrolltext_settings_changed(void);

#endif /* SCROLLTEXT_H */
