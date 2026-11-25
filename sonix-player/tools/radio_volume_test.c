// Host checks for the volume step added to the internet radio write path.
// The real swvolume engine is linked in; only the three audio-stack queries it
// makes are stubbed. This translation unit renames the two apply entry points
// so the dispatch in radio_volume.h can be counted without replacing the
// engine's arithmetic with a model.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define swvolume_apply_s16 radio_test_apply_s16
#define swvolume_apply_s32 radio_test_apply_s32
#include "src/system/streaming/radio_volume.h"
#undef swvolume_apply_s16
#undef swvolume_apply_s32

void swvolume_apply_s16(short *samples, int count);
void swvolume_apply_s32(int32_t *samples, int count);

static bool bluetooth;
static bool usb_active = true;
static bool usb_has_control;

bool audio_output_is_bluetooth(void) { return bluetooth; }
bool usbaudio_active(void) { return usb_active; }
bool usbaudio_has_volume_control(void) { return usb_has_control; }

static int calls_s16;
static int calls_s32;
static void *seen_ptr;
static int seen_count;

void radio_test_apply_s16(short *samples, int count)
{
	calls_s16++;
	seen_ptr = samples;
	seen_count = count;
	swvolume_apply_s16(samples, count);
}

void radio_test_apply_s32(int32_t *samples, int count)
{
	calls_s32++;
	seen_ptr = samples;
	seen_count = count;
	swvolume_apply_s32(samples, count);
}

static int failures;

#define CHECK(cond)                                                                  \
	do {                                                                             \
		if (!(cond)) {                                                               \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			failures++;                                                              \
		}                                                                            \
	} while (0)

static void reset(void)
{
	bluetooth = false;
	usb_active = true;
	usb_has_control = false;
	swvolume_clear_usb_gain();
	swvolume_set_index(100);
	calls_s16 = 0;
	calls_s32 = 0;
	seen_ptr = NULL;
	seen_count = 0;
}

static int count_occurrences(const char *src, const char *needle)
{
	int n = 0;
	const char *p = src;
	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

// The trimmed first argument of a call such as `radio_volume_apply(pcm, ...)`,
// so the apply and the write can be compared by the buffer they both receive.
static const char *first_arg(const char *call, char *out, size_t cap)
{
	const char *open = strchr(call, '(');
	const char *comma = open ? strchr(open, ',') : NULL;
	const char *end;
	size_t len;

	if (!comma || (size_t)(comma - open - 1) >= cap)
		return NULL;
	while (open + 1 < comma && (open[1] == ' ' || open[1] == '\t'))
		open++;
	end = comma;
	while (end > open + 1 && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	len = (size_t)(end - open - 1);
	memcpy(out, open + 1, len);
	out[len] = '\0';
	return out;
}

static void check_wiring(void)
{
	FILE *f = fopen("src/system/streaming/radio.c", "r");
	char *src;
	long size;
	const char *apply = "radio_volume_apply(";
	const char *write = "audio_external_write(";
	int applies[4];
	int n_apply = 0;
	const char *p;

	CHECK(f != NULL);
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	src = malloc((size_t)size + 1);
	CHECK(src != NULL);
	if (!src) {
		fclose(f);
		return;
	}
	CHECK(fread(src, 1, (size_t)size, f) == (size_t)size);
	src[size] = '\0';
	fclose(f);

	CHECK(count_occurrences(src, apply) == 4);
	p = src;
	while ((p = strstr(p, apply)) != NULL && n_apply < 4) {
		applies[n_apply++] = (int)(p - src);
		p += strlen(apply);
	}
	CHECK(n_apply == 4);
	if (n_apply != 4) {
		free(src);
		return;
	}
	for (int i = 0; i < 4; i++) {
		const char *at = src + applies[i];
		const char *next_write = strstr(at, write);
		const char *next_apply = strstr(at + strlen(apply), apply);
		char apply_buf[64];
		char write_buf[64];
		const char *applied;
		const char *written;

		CHECK(next_write != NULL);
		if (!next_write)
			continue;
		CHECK(next_apply == NULL || next_write < next_apply);
		applied = first_arg(at, apply_buf, sizeof apply_buf);
		written = first_arg(next_write, write_buf, sizeof write_buf);
		CHECK(applied != NULL && written != NULL);
		if (applied && written)
			CHECK(strcmp(applied, written) == 0);
	}
	free(src);
}

int main(void)
{
	short pcm16[8];
	int32_t pcm32[8];
	unsigned char pcm24[12];

	// The real curve, not a model of it: mute at 0, a real attenuation at 1,
	// unity at 100, and clamping at both ends.
	CHECK(swvolume_millibel(0) == -1500);
	CHECK(swvolume_coefficient(0) == 0);
	CHECK(swvolume_millibel(1) == -1200);
	CHECK(swvolume_coefficient(1) > 2000 && swvolume_coefficient(1) < 2200);
	CHECK(swvolume_millibel(100) == 0);
	CHECK(swvolume_coefficient(100) == 2147483648LL);
	CHECK(swvolume_coefficient(-5) == swvolume_coefficient(0));
	CHECK(swvolume_coefficient(105) == swvolume_coefficient(100));

	// Each guard in the real swvolume_active() keeps the stream untouched.
	reset();
	bluetooth = true;
	CHECK(!swvolume_active());
	radio_volume_apply(pcm16, 2, 16);
	CHECK(calls_s16 == 0 && calls_s32 == 0);

	reset();
	usb_active = false;
	CHECK(!swvolume_active());
	radio_volume_apply(pcm16, 2, 16);
	CHECK(calls_s16 == 0 && calls_s32 == 0);

	reset();
	usb_has_control = true;
	CHECK(!swvolume_active());
	radio_volume_apply(pcm16, 2, 16);
	CHECK(calls_s16 == 0 && calls_s32 == 0);

	// Dispatch and sample count, with the engine active but at unity.
	reset();
	CHECK(swvolume_active());
	radio_volume_apply(pcm16, 3, 16);
	CHECK(calls_s16 == 1);
	CHECK(calls_s32 == 0);
	CHECK(seen_ptr == pcm16);
	CHECK(seen_count == 6);

	reset();
	radio_volume_apply(pcm32, 3, 32);
	CHECK(calls_s32 == 1);
	CHECK(calls_s16 == 0);
	CHECK(seen_ptr == pcm32);
	CHECK(seen_count == 6);

	reset();
	radio_volume_apply(pcm24, 2, 24);
	CHECK(calls_s16 == 0);
	CHECK(calls_s32 == 0);

	// Index 0 really silences the buffer; index 100 really passes it through.
	reset();
	swvolume_set_index(0);
	for (int i = 0; i < 8; i++)
		pcm16[i] = 1000;
	radio_volume_apply(pcm16, 4, 16);
	CHECK(calls_s16 == 1);
	for (int i = 0; i < 8; i++)
		CHECK(pcm16[i] == 0);

	reset();
	for (int i = 0; i < 8; i++)
		pcm16[i] = 1000;
	radio_volume_apply(pcm16, 4, 16);
	CHECK(calls_s16 == 1);
	for (int i = 0; i < 8; i++)
		CHECK(pcm16[i] == 1000);

	// The USB-C gain path: -6 dB scales, a zero gain with mute silences.
	reset();
	swvolume_set_usb_gain(-600, false);
	CHECK(swvolume_active());
	for (int i = 0; i < 8; i++) {
		pcm16[i] = 1000;
		pcm32[i] = 1000;
	}
	radio_volume_apply(pcm16, 4, 16);
	radio_volume_apply(pcm32, 4, 32);
	CHECK(calls_s16 == 1 && calls_s32 == 1);
	for (int i = 0; i < 8; i++) {
		CHECK(pcm16[i] == 501);
		CHECK(pcm32[i] == 501);
	}

	reset();
	swvolume_set_usb_gain(0, true);
	CHECK(swvolume_active());
	for (int i = 0; i < 8; i++)
		pcm16[i] = 1000;
	radio_volume_apply(pcm16, 4, 16);
	CHECK(calls_s16 == 1);
	for (int i = 0; i < 8; i++)
		CHECK(pcm16[i] == 0);

	CHECK(count_occurrences("radio_volume_apply(a)radio_volume_apply(b)radio_volume_apply(c)"
							"radio_volume_apply(d)radio_volume_apply(e)",
							"radio_volume_apply(") == 5);

	check_wiring();

	if (failures) {
		fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	printf("radio volume: all checks passed\n");
	return 0;
}
