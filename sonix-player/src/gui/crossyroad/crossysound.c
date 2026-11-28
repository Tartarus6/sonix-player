#include "crossysound.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/audio/audio.h"
#include "src/system/streaming/radio.h"

#define RATE 44100
#define MIX_FRAMES 441 // 10 ms
#define BUFFER_MS 46

typedef enum { WAVE_SINE, WAVE_TRIANGLE, WAVE_SAW } wave_t;

typedef struct {
	wave_t wave;
	float f0, f1;	// Hz at the start and at the end
	bool f_expo;	// exponential ramp, else linear
	float g0;		// gain at the start; it falls to 0.01 exponentially
	float seconds;
} recipe_t;

static const recipe_t RECIPES[CROSSY_SFX_COUNT] = {
	[CROSSY_SFX_HOP] = {WAVE_SINE, 340.0f, 660.0f, true, 0.14f, 0.07f},
	[CROSSY_SFX_SPLASH] = {WAVE_TRIANGLE, 220.0f, 55.0f, true, 0.22f, 0.25f},
	[CROSSY_SFX_CRASH] = {WAVE_SAW, 150.0f, 40.0f, false, 0.28f, 0.28f},
	[CROSSY_SFX_COIN] = {WAVE_SINE, 900.0f, 1500.0f, true, 0.13f, 0.1f},
};

typedef struct {
	int16_t *pcm; // mono
	int frames;
	int pos; // next frame to play; `frames` when silent
} sound_t;

static sound_t sounds[CROSSY_SFX_COUNT];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static bool mixer_live;
static volatile bool stop_flag;

static float wave_at(wave_t wave, float phase) {
	// `phase` in turns, 0..1.
	switch (wave) {
	case WAVE_TRIANGLE:
		return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
	case WAVE_SAW:
		return 2.0f * phase - 1.0f;
	default:
		return sinf(phase * 2.0f * (float)M_PI);
	}
}

static bool sound_make(sound_t *s, const recipe_t *r) {
	int frames = (int)(r->seconds * RATE);
	int16_t *pcm = malloc((size_t)frames * sizeof(int16_t));
	if (!pcm) {
		return false;
	}
	float phase = 0.0f;
	for (int i = 0; i < frames; i++) {
		float p = (float)i / (float)frames;
		float f = r->f_expo ? r->f0 * powf(r->f1 / r->f0, p) : r->f0 + (r->f1 - r->f0) * p;
		float gain = r->g0 * powf(0.01f / r->g0, p);
		phase += f / RATE;
		phase -= floorf(phase);
		pcm[i] = (int16_t)(wave_at(r->wave, phase) * gain * 32767.0f);
	}
	s->pcm = pcm;
	s->frames = frames;
	s->pos = frames;
	return true;
}

static void sounds_free(void) {
	for (int i = 0; i < CROSSY_SFX_COUNT; i++) {
		free(sounds[i].pcm);
		memset(&sounds[i], 0, sizeof(sounds[i]));
	}
}

// Adds what is sounding into `mix` (stereo) and moves each sound on.
static void mix_into(int32_t *mix) {
	memset(mix, 0, sizeof(int32_t) * MIX_FRAMES * 2);
	pthread_mutex_lock(&lock);
	for (int i = 0; i < CROSSY_SFX_COUNT; i++) {
		sound_t *s = &sounds[i];
		int n = s->frames - s->pos;
		if (n <= 0) {
			continue;
		}
		if (n > MIX_FRAMES) {
			n = MIX_FRAMES;
		}
		const int16_t *src = s->pcm + s->pos;
		for (int f = 0; f < n; f++) {
			mix[f * 2] += src[f];
			mix[f * 2 + 1] += src[f];
		}
		s->pos += n;
	}
	pthread_mutex_unlock(&lock);
}

static void *mixer_main(void *unused) {
	(void)unused;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < CROSSY_SFX_COUNT; i++) {
		sound_make(&sounds[i], &RECIPES[i]);
	}
	pthread_mutex_unlock(&lock);

	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}
	if (stop_flag || !audio_external_begin_latency(RATE, 2, 16, BUFFER_MS)) {
		if (!stop_flag) {
			fprintf(stderr, "crossysound: no PCM; the game plays silently\n");
		}
		return NULL;
	}

	int32_t mix[MIX_FRAMES * 2];
	int16_t out[MIX_FRAMES * 2];
	while (!stop_flag) {
		mix_into(mix);
		for (int i = 0; i < MIX_FRAMES * 2; i++) {
			int32_t v = mix[i];
			out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
		audio_external_write(out, MIX_FRAMES);
	}
	audio_external_end();
	return NULL;
}

bool crossysound_start(void) {
	crossysound_stop();
	stop_flag = false;
	if (pthread_create(&mixer, NULL, mixer_main, NULL) != 0) {
		return false;
	}
	mixer_live = true;
	return true;
}

void crossysound_play(crossy_sfx_t sfx) {
	if ((int)sfx < 0 || sfx >= CROSSY_SFX_COUNT) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (sounds[sfx].pcm) {
		sounds[sfx].pos = 0;
	}
	pthread_mutex_unlock(&lock);
}

void crossysound_stop(void) {
	if (mixer_live) {
		stop_flag = true;
		pthread_join(mixer, NULL);
		mixer_live = false;
	}
	pthread_mutex_lock(&lock);
	sounds_free();
	pthread_mutex_unlock(&lock);
}
