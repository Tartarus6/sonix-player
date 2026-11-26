/*
 * Header detection and duration for the WAV path, which plays uncompressed
 * PCM without a decoder. The fixtures come from make_wavparse_fixtures.py and
 * all carry the same known 16-bit stereo payload.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "system/audio/wavparse.h"

#define RATE 44100
#define CHANNELS 2
#define FRAMES 256

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void check(int condition, const char *what)
{
	if (!condition)
		fail(what);
}

static void test_container(const char *path, const char *label)
{
	wav_info_t info;
	FILE *f = NULL;
	char what[160];

	snprintf(what, sizeof(what), "%s: parse", label);
	check(wav_parse(path, &info, &f) == WAV_OK, what);
	if (!f)
		return;
	snprintf(what, sizeof(what), "%s: channels", label);
	check(info.channels == CHANNELS, what);
	snprintf(what, sizeof(what), "%s: rate", label);
	check(info.sample_rate == RATE, what);
	snprintf(what, sizeof(what), "%s: bits", label);
	check(info.bits_per_sample == 16, what);
	snprintf(what, sizeof(what), "%s: sample type", label);
	check(info.sample == WAV_PCM_S16, what);
	snprintf(what, sizeof(what), "%s: frame bytes", label);
	check(info.src_frame_bytes == 4, what);
	snprintf(what, sizeof(what), "%s: data size", label);
	check(info.data_size == FRAMES * 4, what);
	snprintf(what, sizeof(what), "%s: duration", label);
	check(info.data_size / info.src_frame_bytes == FRAMES, what);

	/* The first frames are read straight from the file, so the data offset
	 * has to name the payload, not the header. */
	short first[CHANNELS];
	snprintf(what, sizeof(what), "%s: data offset", label);
	check(fseeko(f, (off_t)info.data_offset, SEEK_SET) == 0, what);
	snprintf(what, sizeof(what), "%s: read first frame", label);
	check(fread(first, sizeof(first), 1, f) == 1, what);
	snprintf(what, sizeof(what), "%s: first frame value", label);
	check(first[0] == 7 && first[1] == 107, what);
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc != 6) {
		fprintf(stderr, "usage: %s <plain.wav> <rf64.wav> <wave64.wav> <truncated.wav> <unsupported.wav>\n",
				argv[0]);
		return 2;
	}

	test_container(argv[1], "wav");
	test_container(argv[2], "rf64");
	test_container(argv[3], "wave64");

	/* A ds64 size past EOF is clamped to what the file holds. */
	wav_info_t info;
	FILE *f = NULL;
	check(wav_parse(argv[4], &info, &f) == WAV_OK, "truncated: parse");
	if (f) {
		check(info.data_size == FRAMES * 4, "truncated: size clamped to EOF");
		fclose(f);
	}

	/* An A-law WAV is sound but this path does not read it. */
	f = NULL;
	check(wav_parse(argv[5], &info, &f) == WAV_UNSUPPORTED, "unsupported: reported as unsupported");
	if (f)
		fclose(f);

	if (failures > 0) {
		printf("wavparse_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("wavparse_test: ok\n");
	return 0;
}
