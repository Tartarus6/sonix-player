#ifndef GUI_PET_PETCOMPAT_H
#define GUI_PET_PETCOMPAT_H

// The listening/state half of the Pet app, kept inside the app so the port
// imports no other V1 feature group. The page draws the state returned here;
// a one-second timer credits audible playback and the whole state lives in
// device_config.ini's [pet] section.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_HAPPY_MAX 100
#define PET_ENERGY_MAX 100
#define PET_DAY_GOAL_SECS (15 * 60)
#define PET_DEFAULT_NAME "Pikachu"
#define PET_FORTUNE_NOTES 50
#define PET_FORTUNE_COUNT 6
#define PET_DROP_KINDS 4
#define PET_RHYTHM_BEATS 16
#define PET_RHYTHM_PERFECT_MS 120
#define PET_RHYTHM_GOOD_MS 260
// 100 BPM. Longer than twice the GOOD window so neighbouring beats never
// share a judging moment, and long enough that sixteen beats are a real game.
#define PET_RHYTHM_BEAT_MS 600
#define PET_VOLUME_FLOOR 10

typedef enum {
	PET_DROP_NOTE = 0,
	PET_DROP_CASSETTE,
	PET_DROP_STICKER,
	PET_DROP_VINYL,
	PET_DROP_COUNT,
} pet_drop_t;

typedef enum {
	PET_STAGE_BABY = 0,
	PET_STAGE_CHILD,
	PET_STAGE_TEEN,
	PET_STAGE_ADULT,
	PET_STAGE_COUNT,
} pet_stage_t;

typedef enum {
	PET_MOOD_SLEEPING = 0,
	PET_MOOD_SAD,
	PET_MOOD_OKAY,
	PET_MOOD_HAPPY,
	PET_MOOD_BLISSFUL,
	PET_MOOD_DIZZY,
	PET_MOOD_COUNT,
} pet_mood_t;

typedef enum {
	PET_ITEM_PARTY_HAT = 0,
	PET_ITEM_HEADPHONES,
	PET_ITEM_SCARF,
	PET_ITEM_CROWN,
	PET_ITEM_ROOM_SUNSET,
	PET_ITEM_ROOM_NIGHT,
	PET_ITEM_STREAK_FREEZE,
	PET_ITEM_COUNT,
} pet_item_t;

#define PET_ITEM_NONE ((pet_item_t)-1)
#define PET_ITEM_LAST_OUTFIT PET_ITEM_CROWN
#define PET_ITEM_FIRST_ROOM PET_ITEM_ROOM_SUNSET

typedef enum {
	PET_VIBE_CALM = 0,
	PET_VIBE_NEUTRAL,
	PET_VIBE_ENERGETIC,
} pet_vibe_t;

typedef enum {
	PET_LOOK_NONE = 0,
	PET_LOOK_SHADES,
	PET_LOOK_CHAIN,
	PET_LOOK_COLLAR,
	PET_LOOK_CUP,
} pet_genre_look_t;

typedef enum {
	PET_TEMPERAMENT_NONE = 0,
	PET_TEMPERAMENT_UPBEAT,
	PET_TEMPERAMENT_CALM,
	PET_TEMPERAMENT_CURIOUS,
	PET_TEMPERAMENT_COUNT,
} pet_temperament_t;

typedef enum {
	PET_ACH_NIGHT_OWL = 1 << 0,
	PET_ACH_DEEP_DIVER = 1 << 1,
	PET_ACH_POLYMATH = 1 << 2,
	PET_ACH_FOCUS = 1 << 3,
} pet_achievement_t;

typedef enum {
	PET_EVENT_NONE = 0,
	PET_EVENT_GOAL_MET = 1 << 0,
	PET_EVENT_STAGE_UP = 1 << 1,
	PET_EVENT_POLYMATH = 1 << 2,
	PET_EVENT_DROP = 1 << 3,
	PET_EVENT_DIZZY = 1 << 4,
	PET_EVENT_FORAGE = 1 << 5,
	PET_EVENT_FIND = 1 << 6,
} pet_event_t;

typedef struct {
	int64_t adopted;
	int64_t last_seen;
	int64_t last_music;
	int64_t last_pet;
	int64_t xp_secs;
	long day;
	int today_secs;
	int today_xp_secs;
	int happy;
	int energy;
	int streak;
	long goal_day;
	int notes;
	int note_credit;
	int owned;
	int wearing;
	int room;
	int freezes;
	int found[PET_DROP_COUNT];
	int drops;
	long fortune_day;
	int fortune_id;
	long fortune_paid;
	long rhythm_day;
	long treat_hour;
	int achievements;
	int day_genre_count;
} pet_state_t;

typedef struct {
	bool fortune_ready;
	int fortune_id;
	int drops;
	int found[PET_DROP_COUNT];
	bool treats_ready;
} petwatch_box_t;

void petwatch_init(void);
const pet_state_t *petwatch_state(void);
bool petwatch_listening(void);
bool petwatch_pet_now(void);
bool petwatch_hold(void);
int petwatch_take_events(void);
int petwatch_take_achievement(void);
pet_vibe_t petwatch_vibe(void);
pet_genre_look_t petwatch_look(void);
petwatch_box_t petwatch_box(void);
int petwatch_drop_take(void);
bool petwatch_fortune_pay(void);
int petwatch_treat_finish(int hits);
bool petwatch_focus(void);
bool petwatch_set_name(const char *name);
pet_temperament_t petwatch_temperament(void);
bool petwatch_has_soundscape(void);
bool petwatch_rhythm_ready(void);
int petwatch_rhythm_finish(int score);
bool petwatch_buy(pet_item_t item);
bool petwatch_wear(pet_item_t item);
void petwatch_flush(void);
void petwatch_track_title(char *out, size_t out_size);
int64_t petwatch_now(void);

pet_stage_t pet_stage(const pet_state_t *st);
pet_mood_t pet_mood(const pet_state_t *st, int64_t now);
pet_temperament_t pet_temperament(const pet_state_t *st);
bool pet_has_soundscape(const pet_state_t *st);
int pet_wearing(const pet_state_t *st);
int pet_room(const pet_state_t *st);
int pet_freezes(const pet_state_t *st);
bool pet_owned(const pet_state_t *st, pet_item_t item);
int pet_item_price(pet_item_t item);
const char *pet_item_tag(pet_item_t item);
const char *pet_achievement_tag(int achievement);
const char *pet_stage_tag(pet_stage_t stage);
const char *pet_mood_tag(pet_mood_t mood);
const char *pet_temperament_tag(pet_temperament_t temperament);
void pet_name(char *out, size_t out_size);

#endif /* GUI_PET_PETCOMPAT_H */
