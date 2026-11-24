#include "ios_home_anim.h"

// A spring: a fast slowdown with a slight overshoot (about 1.12 times) as the
// icon settles, the cubic-bezier(0.22, 1.12, 0.36, 1) of the classic.
static float eval_ios_spring(float t) {
	if (t <= 0.0f) {
		return 0.0f;
	}
	if (t >= 1.0f) {
		return 1.0f;
	}
	const float c1 = 1.15f;
	const float c3 = c1 + 1.0f;
	float tm1 = t - 1.0f;
	return 1.0f + c3 * (tm1 * tm1 * tm1) + c1 * (tm1 * tm1);
}

// A smooth curve for the opacity, with no overshoot so it never passes 255.
static float eval_ease_out(float t) {
	if (t <= 0.0f) {
		return 0.0f;
	}
	if (t >= 1.0f) {
		return 1.0f;
	}
	float inv = 1.0f - t;
	return 1.0f - (inv * inv * inv);
}

void ios_home_anim_init(ios_home_anim_controller_t *ctrl, int16_t screen_w, int16_t screen_h) {
	ctrl->duration_sec = 0.56f; // the whole flight, 560 ms

	// (dx, dy as a fraction of the screen, starting scale, delay). The grid's
	// nine places row by row, then the dock's three. The corners and edges come
	// from their own side, the middle icon zooms out from in front, and the
	// dock rises from below.
	static const float fly_vectors[TOTAL_ANIM_ITEMS][4] = {
		{-0.45f, -0.35f, 1.38f, 0.00f}, // [0] top left
		{0.00f, -0.42f, 1.38f, 0.02f},	// [1] top centre
		{0.45f, -0.35f, 1.38f, 0.00f},	// [2] top right
		{-0.52f, 0.00f, 1.35f, 0.03f},	// [3] middle left
		{0.00f, 0.00f, 1.75f, 0.05f},	// [4] centre: zooms out from the front
		{0.52f, 0.00f, 1.35f, 0.03f},	// [5] middle right
		{-0.45f, 0.30f, 1.38f, 0.04f},	// [6] bottom left
		{0.00f, 0.32f, 1.38f, 0.05f},	// [7] bottom centre
		{0.45f, 0.30f, 1.38f, 0.04f},	// [8] bottom right
		{0.00f, 0.28f, 1.00f, 0.03f},	// [9..11] the dock rises from below
		{0.00f, 0.28f, 1.00f, 0.03f},
		{0.00f, 0.28f, 1.00f, 0.03f},
	};

	for (int i = 0; i < TOTAL_ANIM_ITEMS; i++) {
		ios_icon_anim_t *item = &ctrl->items[i];
		item->start_offset_x = fly_vectors[i][0] * (float)screen_w;
		item->start_offset_y = fly_vectors[i][1] * (float)screen_h;
		item->start_scale = fly_vectors[i][2];
		item->delay_sec = fly_vectors[i][3];
	}
	ios_home_anim_reset_locked(ctrl);
}

void ios_home_anim_reset_locked(ios_home_anim_controller_t *ctrl) {
	ctrl->elapsed_time = 0.0f;
	ctrl->is_playing = false;
	ctrl->is_unlocked = false;

	for (int i = 0; i < TOTAL_ANIM_ITEMS; i++) {
		ios_icon_anim_t *item = &ctrl->items[i];
		item->current_x = (int16_t)item->start_offset_x;
		item->current_y = (int16_t)item->start_offset_y;
		item->current_scale = item->start_scale;
		item->current_opacity = 0;
		item->finished = false;
	}
}

void ios_home_anim_trigger_unlock(ios_home_anim_controller_t *ctrl) {
	ios_home_anim_reset_locked(ctrl);
	ctrl->is_playing = true;
	ctrl->is_unlocked = true;
}

bool ios_home_anim_update(ios_home_anim_controller_t *ctrl, float dt_sec) {
	if (!ctrl->is_playing) {
		return false;
	}

	ctrl->elapsed_time += dt_sec;
	bool any_active = false;

	for (int i = 0; i < TOTAL_ANIM_ITEMS; i++) {
		ios_icon_anim_t *item = &ctrl->items[i];
		if (item->finished) {
			continue;
		}

		float local_t = (ctrl->elapsed_time - item->delay_sec) / ctrl->duration_sec;
		if (local_t <= 0.0f) {
			any_active = true;
			continue;
		}
		if (local_t >= 1.0f) {
			local_t = 1.0f;
			item->finished = true;
		} else {
			any_active = true;
		}

		float spring_p = eval_ios_spring(local_t);
		float alpha_p = eval_ease_out(local_t * 1.4f); // appears a little before landing

		float inv_p = 1.0f - spring_p;
		item->current_x = (int16_t)(item->start_offset_x * inv_p);
		item->current_y = (int16_t)(item->start_offset_y * inv_p);
		item->current_scale = 1.0f + (item->start_scale - 1.0f) * inv_p;
		item->current_opacity = (uint8_t)(alpha_p * 255.0f);
	}

	if (!any_active) {
		ctrl->is_playing = false;
	}
	return ctrl->is_playing;
}
