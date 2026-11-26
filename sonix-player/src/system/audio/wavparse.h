#ifndef WAV_PARSE_H
#define WAV_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// The WAV family read by audio.c's uncompressed path: RIFF/WAVE, RF64 with a
// ds64 size chunk, and Sony Wave64. Only sample encodings that can be streamed
// straight to the card are named here; anything else is left to libsndfile.

typedef enum {
	WAV_PCM_U8,
	WAV_PCM_S16,
	WAV_PCM_S24, // packed, three bytes a sample
	WAV_PCM_S32,
	WAV_FLOAT32,
	WAV_FLOAT64,
} wav_sample_t;

typedef struct {
	int channels;
	int sample_rate;
	int bits_per_sample; // as stored in the file, for the format line
	wav_sample_t sample;
	int src_frame_bytes; // one frame as stored in the file
	int out_bits;		 // 16 or 32: what the device is opened with
	// 64-bit: long is 32 bits on the device, and a 24/96 stereo file passes
	// 2 GB after about an hour.
	int64_t data_offset;
	int64_t data_size;
} wav_info_t;

#define WAV_OK 0
#define WAV_BAD -1		   // not a WAV, or a broken one
#define WAV_UNSUPPORTED -2 // a sound WAV in an encoding this path does not read

// Opens `filepath`, reads its headers and leaves the stream at the first audio
// byte. On failure no file is returned. WAV_UNSUPPORTED means the container is
// sound but the encoding needs another decoder.
int wav_parse(const char *filepath, wav_info_t *info, FILE **file_out);

#endif /* WAV_PARSE_H */
