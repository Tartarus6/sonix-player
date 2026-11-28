#include "petcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"

// A small, self-contained state machine for the Pet page. The V1 fork keeps a
// much larger one under src/system/pet; this port deliberately imports only
// the GUI groups, so the page gets the same public API with the arithmetic it
// actually needs: audible playback feeds the meters, a day's music feeds the
// stage, and the wardrobe and the little games pay out of the same wallet.

static pet_state_t state;
static bool initialized;
static lv_timer_t *watch_timer;
static int pending_events;
static int pending_achievement;
static int ticks_since_save;

static long day_of(int64_t now) { return (long)(now / 86400); }

static void save(void) {
	config_set_int("pet", "happy", state.happy);
	config_set_int("pet", "energy", state.energy);
	config_set_int("pet", "xp_secs", (long)state.xp_secs);
	config_set_int("pet", "today_secs", state.today_secs);
	config_set_int("pet", "today_xp_secs", state.today_xp_secs);
	config_set_int("pet", "notes", state.notes);
	config_set_int("pet", "streak", state.streak);
	config_set_int("pet", "day", state.day);
	config_set_int("pet", "goal_day", state.goal_day);
	config_set_int("pet", "owned", state.owned);
	config_set_int("pet", "wearing", state.wearing);
	config_set_int("pet", "room", state.room);
	config_set_int("pet", "freezes", state.freezes);
	config_set_int("pet", "last_seen", (long)state.last_seen);
	config_set_int("pet", "last_music", (long)state.last_music);
	config_set_int("pet", "fortune_day", state.fortune_day);
	config_set_int("pet", "fortune_paid", state.fortune_paid);
	config_set_int("pet", "rhythm_day", state.rhythm_day);
	config_set_int("pet", "treat_hour", state.treat_hour);
	config_set_int("pet", "achievements", state.achievements);
	config_set_int("pet", "found", state.found[0]);
	config_set_int("pet", "drops", state.drops);
}

static void load(void) {
	int64_t now = (int64_t)time(NULL);
	long day = day_of(now);
	state.adopted = (int64_t)config_get_int("pet", "adopted", (long)now);
	state.last_seen = (int64_t)config_get_int("pet", "last_seen", (long)now);
	state.last_music = (int64_t)config_get_int("pet", "last_music", (long)now);
	state.last_pet = (int64_t)config_get_int("pet", "last_pet", 0);
	state.xp_secs = (int64_t)config_get_int("pet", "xp_secs", 0);
	state.day = config_get_int("pet", "day", day);
	state.happy = (int)config_get_int("pet", "happy", 65);
	state.energy = (int)config_get_int("pet", "energy", 70);
	state.streak = (int)config_get_int("pet", "streak", 0);
	state.goal_day = config_get_int("pet", "goal_day", 0);
	state.notes = (int)config_get_int("pet", "notes", 0);
	state.owned = (int)config_get_int("pet", "owned", 0);
	state.wearing = (int)config_get_int("pet", "wearing", (long)PET_ITEM_NONE);
	state.room = (int)config_get_int("pet", "room", 0);
	state.freezes = (int)config_get_int("pet", "freezes", 0);
	state.found[0] = (int)config_get_int("pet", "found", 0);
	state.drops = (int)config_get_int("pet", "drops", 0);
	state.fortune_day = config_get_int("pet", "fortune_day", 0);
	state.fortune_paid = config_get_int("pet", "fortune_paid", 0);
	state.rhythm_day = config_get_int("pet", "rhythm_day", 0);
	state.treat_hour = config_get_int("pet", "treat_hour", 0);
	state.achievements = (int)config_get_int("pet", "achievements", 0);

	if (state.day != day) {
		state.today_secs = 0;
		state.today_xp_secs = 0;
		state.day = day;
	} else {
		state.today_secs = (int)config_get_int("pet", "today_secs", 0);
		state.today_xp_secs = (int)config_get_int("pet", "today_xp_secs", 0);
	}
	if (state.happy < 0) state.happy = 0;
	if (state.happy > PET_HAPPY_MAX) state.happy = PET_HAPPY_MAX;
	if (state.energy < 0) state.energy = 0;
	if (state.energy > PET_ENERGY_MAX) state.energy = PET_ENERGY_MAX;
	state.fortune_id = (int)(day % PET_FORTUNE_COUNT);
	initialized = true;
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	int64_t now = (int64_t)time(NULL);
	long day = day_of(now);
	if (day != state.day) {
		state.today_secs = 0;
		state.today_xp_secs = 0;
		state.day = day;
		state.fortune_id = (int)(day % PET_FORTUNE_COUNT);
	}
	bool playing = audio_get_status() == AUDIO_STATUS_PLAYING;
	bool audible = playing && device_state_get_volume_percent() > PET_VOLUME_FLOOR;
	if (audible) {
		state.last_music = now;
		if (state.today_xp_secs < 4 * 3600) {
			state.xp_secs++;
			state.today_xp_secs++;
		}
		if (state.today_secs < PET_DAY_GOAL_SECS && state.today_secs + 1 >= PET_DAY_GOAL_SECS) {
			pending_events |= PET_EVENT_GOAL_MET;
			if (state.goal_day != day) {
				state.goal_day = day;
				state.streak++;
			}
		}
		state.today_secs++;
		if (state.today_secs % 20 == 0 && state.happy < PET_HAPPY_MAX) state.happy++;
		if (state.today_secs % 12 == 0 && state.energy < PET_ENERGY_MAX) state.energy++;
	} else {
		if (state.energy > 0 && (now - state.last_seen) >= 600) state.energy--;
	}
	state.last_seen = now;
	if (++ticks_since_save >= 120) {
		ticks_since_save = 0;
		save();
	}
}

static void ensure_init(void) {
	if (!initialized) {
		load();
	}
	if (!watch_timer && lv_is_initialized()) {
		watch_timer = lv_timer_create(tick_cb, 1000, NULL);
	}
}

void petwatch_init(void) { ensure_init(); }

const pet_state_t *petwatch_state(void) {
	ensure_init();
	return &state;
}

bool petwatch_listening(void) {
	ensure_init();
	return audio_get_status() == AUDIO_STATUS_PLAYING &&
		   device_state_get_volume_percent() > PET_VOLUME_FLOOR;
}

int64_t petwatch_now(void) { return (int64_t)time(NULL); }

bool petwatch_pet_now(void) {
	ensure_init();
	int64_t now = petwatch_now();
	if (now - state.last_pet < 60) {
		return false;
	}
	state.last_pet = now;
	if (state.happy < PET_HAPPY_MAX) state.happy += 4;
	if (state.energy < PET_ENERGY_MAX) state.energy += 2;
	save();
	return true;
}

bool petwatch_hold(void) {
	ensure_init();
	if (state.happy < PET_HAPPY_MAX) state.happy += 6;
	save();
	return true;
}

int petwatch_take_events(void) {
	int events = pending_events;
	pending_events = 0;
	return events;
}

int petwatch_take_achievement(void) {
	int achievement = pending_achievement;
	pending_achievement = 0;
	return achievement;
}

pet_vibe_t petwatch_vibe(void) { return PET_VIBE_NEUTRAL; }
pet_genre_look_t petwatch_look(void) { return PET_LOOK_NONE; }

petwatch_box_t petwatch_box(void) {
	ensure_init();
	petwatch_box_t box;
	memset(&box, 0, sizeof(box));
	box.fortune_ready = state.fortune_paid != state.day;
	box.fortune_id = (int)(state.day % PET_FORTUNE_COUNT);
	box.drops = state.drops;
	memcpy(box.found, state.found, sizeof(box.found));
	box.treats_ready = state.treat_hour != petwatch_now() / 3600;
	return box;
}

int petwatch_drop_take(void) {
	ensure_init();
	if (state.drops <= 0) {
		return -1;
	}
	state.drops--;
	int kind = state.found[0] > 0 ? PET_DROP_NOTE : PET_DROP_CASSETTE;
	if (state.found[kind] > 0) state.found[kind]--;
	save();
	return kind;
}

bool petwatch_fortune_pay(void) {
	ensure_init();
	if (state.fortune_paid == state.day) {
		return false;
	}
	state.fortune_paid = state.day;
	state.notes += PET_FORTUNE_NOTES;
	save();
	return true;
}

int petwatch_treat_finish(int hits) {
	ensure_init();
	long hour = petwatch_now() / 3600;
	if (state.treat_hour == hour) {
		return 0;
	}
	state.treat_hour = hour;
	int pay = 5 + (hits > 0 ? hits : 0) * 2;
	if (pay > 10) pay = 10;
	state.notes += pay;
	save();
	return pay;
}

bool petwatch_focus(void) { return false; }

bool petwatch_set_name(const char *name) {
	if (!name) {
		return false;
	}
	config_set("pet", "name", name);
	config_save();
	return true;
}

pet_temperament_t petwatch_temperament(void) {
	ensure_init();
	return pet_temperament(&state);
}

bool petwatch_has_soundscape(void) { return petwatch_temperament() != PET_TEMPERAMENT_NONE; }

bool petwatch_rhythm_ready(void) {
	ensure_init();
	return state.rhythm_day != state.day;
}

int petwatch_rhythm_finish(int score) {
	ensure_init();
	if (!petwatch_rhythm_ready()) {
		return 0;
	}
	state.rhythm_day = state.day;
	int pay = score / 10;
	if (pay > 12) pay = 12;
	state.notes += pay;
	save();
	return pay;
}

bool petwatch_buy(pet_item_t item) {
	ensure_init();
	if (item < 0 || item >= PET_ITEM_COUNT) {
		return false;
	}
	int price = pet_item_price(item);
	if (state.notes < price) {
		return false;
	}
	state.notes -= price;
	if (item == PET_ITEM_STREAK_FREEZE) {
		state.freezes++;
	} else {
		state.owned |= 1 << item;
	}
	save();
	return true;
}

bool petwatch_wear(pet_item_t item) {
	ensure_init();
	if (item < 0 || item >= PET_ITEM_COUNT || !pet_owned(&state, item)) {
		return false;
	}
	if (item >= PET_ITEM_FIRST_ROOM) {
		state.room = 1 + (item - (int)PET_ITEM_FIRST_ROOM);
	} else {
		state.wearing = item;
	}
	save();
	return true;
}

void petwatch_flush(void) {
	if (initialized) {
		save();
	}
}

void petwatch_track_title(char *out, size_t out_size) {
	if (!out || !out_size) {
		return;
	}
	out[0] = 0;
}

pet_stage_t pet_stage(const pet_state_t *st) {
	if (!st) return PET_STAGE_BABY;
	if (st->xp_secs >= 12 * 3600) return PET_STAGE_ADULT;
	if (st->xp_secs >= 4 * 3600) return PET_STAGE_TEEN;
	if (st->xp_secs >= 3600) return PET_STAGE_CHILD;
	return PET_STAGE_BABY;
}

pet_mood_t pet_mood(const pet_state_t *st, int64_t now) {
	if (!st) return PET_MOOD_OKAY;
	if (now - st->last_music > 48 * 3600) return PET_MOOD_SLEEPING;
	if (st->happy < 30 || st->energy < 20) return PET_MOOD_SAD;
	if (st->happy >= 85 && st->energy >= 70) return PET_MOOD_BLISSFUL;
	if (st->happy >= 60) return PET_MOOD_HAPPY;
	return PET_MOOD_OKAY;
}

pet_temperament_t pet_temperament(const pet_state_t *st) {
	if (!st || st->day_genre_count < 3) return PET_TEMPERAMENT_NONE;
	return PET_TEMPERAMENT_CURIOUS;
}

bool pet_has_soundscape(const pet_state_t *st) { return pet_temperament(st) != PET_TEMPERAMENT_NONE; }
int pet_wearing(const pet_state_t *st) { return st ? st->wearing : (int)PET_ITEM_NONE; }
int pet_room(const pet_state_t *st) { return st ? st->room : 0; }
int pet_freezes(const pet_state_t *st) { return st ? st->freezes : 0; }
bool pet_owned(const pet_state_t *st, pet_item_t item) {
	return st && item >= 0 && item < PET_ITEM_COUNT && (st->owned & (1 << item));
}

int pet_item_price(pet_item_t item) {
	static const int prices[PET_ITEM_COUNT] = {100, 150, 200, 300, 250, 350, 50};
	return (item >= 0 && item < PET_ITEM_COUNT) ? prices[item] : 0;
}

const char *pet_item_tag(pet_item_t item) {
	static const char *const tags[PET_ITEM_COUNT] = {
		"Party hat", "Headphones", "Scarf", "Crown",
		"Sunset room", "Night room", "Streak freeze",
	};
	return (item >= 0 && item < PET_ITEM_COUNT) ? tags[item] : "";
}

const char *pet_achievement_tag(int achievement) {
	switch (achievement) {
	case PET_ACH_NIGHT_OWL: return "Night owl";
	case PET_ACH_DEEP_DIVER: return "Deep diver";
	case PET_ACH_POLYMATH: return "Polymath";
	case PET_ACH_FOCUS: return "Focus";
	default: return "";
	}
}

const char *pet_stage_tag(pet_stage_t stage) {
	static const char *const tags[PET_STAGE_COUNT] = {"Baby", "Child", "Teen", "Adult"};
	return (stage >= 0 && stage < PET_STAGE_COUNT) ? tags[stage] : "";
}

const char *pet_mood_tag(pet_mood_t mood) {
	static const char *const tags[PET_MOOD_COUNT] = {
		"Sleeping", "Sad", "Okay", "Happy", "Blissful", "Dizzy",
	};
	return (mood >= 0 && mood < PET_MOOD_COUNT) ? tags[mood] : "";
}

const char *pet_temperament_tag(pet_temperament_t temperament) {
	static const char *const tags[PET_TEMPERAMENT_COUNT] = {
		"Undecided", "Upbeat", "Calm", "Curious",
	};
	return (temperament >= 0 && temperament < PET_TEMPERAMENT_COUNT) ? tags[temperament] : "";
}

void pet_name(char *out, size_t out_size) {
	if (!out || !out_size) {
		return;
	}
	const char *name = config_get("pet", "name", PET_DEFAULT_NAME);
	if (!name || !name[0]) {
		name = PET_DEFAULT_NAME;
	}
	snprintf(out, out_size, "%s", name);
}
