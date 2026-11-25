#ifndef IOS_HOME_ANIM_H
#define IOS_HOME_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The classic iOS home screen's arrival after unlocking: every icon flies in
// from a different side, larger than it ends up and transparent, and lands with
// a small overshoot. Only the arithmetic lives here; pod_home.c applies the
// numbers to its tiles.

#define GRID_ICON_COUNT 9 // the Pod home's 3x3 grid, row by row
#define DOCK_ICON_COUNT 3
#define TOTAL_ANIM_ITEMS (GRID_ICON_COUNT + DOCK_ICON_COUNT)

typedef struct {
	float start_offset_x; // where it starts, relative to its place, in pixels
	float start_offset_y;
	float start_scale; // 1.0 is its own size
	float delay_sec;   // staggers the start

	// The state for this frame. The offsets are how far it still is from its
	// place, so (0, 0) means landed.
	int16_t current_x;
	int16_t current_y;
	float current_scale;
	uint8_t current_opacity; // 0 to 255
	bool finished;
} ios_icon_anim_t;

typedef struct {
	ios_icon_anim_t items[TOTAL_ANIM_ITEMS];
	float elapsed_time;
	float duration_sec;
	bool is_playing;
	bool is_unlocked;
} ios_home_anim_controller_t;

// Sets up the flight vectors for a screen of this size and leaves every icon at
// its starting point.
void ios_home_anim_init(ios_home_anim_controller_t *ctrl, int16_t screen_w, int16_t screen_h);

// Starts the arrival.
void ios_home_anim_trigger_unlock(ios_home_anim_controller_t *ctrl);

// Puts every icon back at its starting point, hidden.
void ios_home_anim_reset_locked(ios_home_anim_controller_t *ctrl);

// Moves the animation on by one frame. True while some icon is still on its way.
bool ios_home_anim_update(ios_home_anim_controller_t *ctrl, float dt_sec);

#ifdef __cplusplus
}
#endif

#endif // IOS_HOME_ANIM_H
