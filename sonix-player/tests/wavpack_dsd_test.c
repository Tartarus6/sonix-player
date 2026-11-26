/*
 * A .wv can hold PCM or DSD. The decoder has to tell them apart from the
 * content: PCM stays on the 32-bit PCM path, DSD goes through the same DoP
 * stage as .dsf, .dff and SACD images.
 *
 * Fixtures come from tests/make_wavpack_fixtures.py. The DSD file is the
 * .dsf pattern transcoded by the wavpack CLI; WavPack hands DSD bytes back
 * MSB first, so the DoP data bytes are the bit-reverse of the .dsf pattern.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "system/decode/dsd.h"
#include "system/decode/wavpackdec.h"

#define CHANNELS 2
#define FRAMES 4096
#define DSD64_RATE 2822400u

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

/* The formula tests/make_dsd_fixtures.py fills the DSD audio with. */
static unsigned char pattern(int channel, uint64_t index)
{
	return (unsigned char)((channel * 31 + index * 7 + 17) & 0xff);
}

static unsigned char reverse_bits(unsigned char v)
{
	v = (unsigned char)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
	v = (unsigned char)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
	v = (unsigned char)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
	return v;
}

/* PCM .wv: still decoded as PCM, at the file's rate and depth. */
static void test_pcm(const char *path)
{
	wavpackdec_t *w = wavpackdec_open(path);
	check(w != NULL, "pcm: open");
	if (!w)
		return;
	check(!wavpackdec_is_dsd(w), "pcm: not marked DSD");
	check(wavpackdec_channels(w) == CHANNELS, "pcm: channels");
	check(wavpackdec_sample_rate(w) == 44100, "pcm: sample rate");
	check(wavpackdec_bits(w) == 16, "pcm: bits");
	check(wavpackdec_total_frames(w) == FRAMES, "pcm: total frames");

	int32_t buf[256 * CHANNELS];
	uint64_t got = wavpackdec_read_s32(w, 256, buf);
	check(got == 256, "pcm: read 256 frames");
	if (got == 256) {
		/* 16-bit samples arrive left-justified in 32 bits. */
		int bad = 0;
		for (int i = 0; i < 256 && !bad; i++) {
			for (int c = 0; c < CHANNELS; c++) {
				int want = (i * 7 + c * 1000 + 1234) & 0x7FFF;
				if (buf[i * CHANNELS + c] != (int32_t)(want << 16))
					bad = 1;
			}
		}
		check(!bad, "pcm: samples match the source ramp");
	}
	wavpackdec_close(w);
}

/* DSD .wv: reported and read as DSD, not decimated PCM. */
static void test_dsd(const char *path)
{
	wavpackdec_t *w = wavpackdec_open(path);
	check(w != NULL, "dsd: open");
	if (!w)
		return;
	check(wavpackdec_is_dsd(w), "dsd: marked DSD");
	check(wavpackdec_dsd_rate(w) == DSD64_RATE, "dsd: native rate");
	wavpackdec_close(w);

	dsd_file_t *d = dsd_open_wavpack(path);
	check(d != NULL, "dsd: open output stage");
	if (!d)
		return;
	check(dsd_channels(d) == CHANNELS, "dsd: channels");
	check(dsd_rate(d) == DSD64_RATE, "dsd: stage rate");
	check(dsd_multiple(d) == 64, "dsd: multiple");
	check(dsd_output_rate(d) == 176400, "dsd: DoP rate");
	check(dsd_total_frames(d) == FRAMES / 2, "dsd: DoP total frames");
	check(!dsd_is_pcm(d), "dsd: DoP, not PCM");

	int32_t buf[256 * CHANNELS];
	uint64_t got = dsd_read(d, 256, buf);
	check(got == 256, "dsd: read 256 DoP frames");
	if (got == 256) {
		int bad = 0;
		for (int i = 0; i < 256 && !bad; i++) {
			for (int c = 0; c < CHANNELS; c++) {
				uint32_t word = (uint32_t)buf[i * CHANNELS + c] >> 8;
				uint32_t marker = word >> 16;
				unsigned hi = (word >> 8) & 0xff;
				unsigned lo = word & 0xff;
				uint32_t want_marker = (i & 1) ? 0xFA : 0x05;
				unsigned want_hi = reverse_bits(pattern(c, (uint64_t)i * 2));
				unsigned want_lo = reverse_bits(pattern(c, (uint64_t)i * 2 + 1));
				if (marker != want_marker || hi != want_hi || lo != want_lo)
					bad = 1;
			}
		}
		check(!bad, "dsd: DoP words match the DSD bytes");
	}

	check(dsd_seek(d, 1000), "dsd: seek");
	got = dsd_read(d, 8, buf);
	check(got == 8, "dsd: read after seek");
	if (got == 8) {
		int bad = 0;
		for (int i = 0; i < 8 && !bad; i++) {
			for (int c = 0; c < CHANNELS; c++) {
				uint32_t word = (uint32_t)buf[i * CHANNELS + c] >> 8;
				unsigned hi = (word >> 8) & 0xff;
				unsigned lo = word & 0xff;
				uint64_t byte = 2000 + (uint64_t)i * 2;
				if (hi != reverse_bits(pattern(c, byte)) ||
					lo != reverse_bits(pattern(c, byte + 1)))
					bad = 1;
			}
		}
		check(!bad, "dsd: bytes after seek match the source");
	}
	dsd_close(d);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <pcm.wv> <dsd.wv>\n", argv[0]);
		return 2;
	}

	test_pcm(argv[1]);
	test_dsd(argv[2]);

	if (failures > 0) {
		printf("wavpack_dsd_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("wavpack_dsd_test: ok\n");
	return 0;
}
