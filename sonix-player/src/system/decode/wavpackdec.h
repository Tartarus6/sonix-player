#ifndef WAVPACK_DECODE_H
#define WAVPACK_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WavPack: .wv files.
//
// Like Opus, this library is not borrowed from the firmware -- the device does
// not ship it at all -- but statically compiled into the binary by the Makefile.
//
// What those hundred and fifty kilobytes buy:
//
//   * lossless up to 32-bit integer and floating point;
//   * hybrid mode, i.e. a lossy .wv at fixed bitrate plus a .wvc correction
//     file beside it that restores lossless -- when the .wvc is present it is
//     picked up automatically, with no involvement from the caller;
//   * DSD inside WavPack. The file is opened as a bitstream, and the raw bytes
//     are handed to the DSD output stage (dsd_open_wavpack) for DoP or PCM.
//     DSD is never decimated here, which is what the PCM reads below are for.
//
// PCM samples come out as int32 right-justified at the file's real depth; this
// wrapper shifts them left for the player's 32-bit path, as dr_flac and
// libsndfile already do. DSD bytes come out separately, one per WavPack
// sample, most significant bit first.

typedef struct wavpackdec wavpackdec_t;

// NULL when the file cannot be opened; the reason goes to the log.
wavpackdec_t *wavpackdec_open(const char *filepath);
void wavpackdec_close(wavpackdec_t *w);

int wavpackdec_channels(const wavpackdec_t *w);
int wavpackdec_sample_rate(const wavpackdec_t *w);
int wavpackdec_bits(const wavpackdec_t *w); // the file's depth, 24 for float
uint64_t wavpackdec_total_frames(const wavpackdec_t *w);

// Interleaved frames. The 32-bit form is left-justified, like every other
// decoder here, so a 24-bit .wv reaches the DAC with all twenty-four bits.
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t frames, short *out);
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t frames, int32_t *out);

bool wavpackdec_seek(wavpackdec_t *w, uint64_t frame);

// True when the file carries a DSD bitstream. Such a file must be routed to
// the DSD output stage; the PCM reads above do not describe it.
bool wavpackdec_is_dsd(const wavpackdec_t *w);

// The native DSD rate (2822400, 5644800 or 11289600), 0 for a PCM file.
uint32_t wavpackdec_dsd_rate(const wavpackdec_t *w);

// Raw DSD bytes from a DSD file. `frames` counts bytes per channel; `out`
// receives channels * frames interleaved bytes, most significant bit first.
// Returns the number of per-channel bytes read.
uint64_t wavpackdec_read_dsd(wavpackdec_t *w, uint64_t frames, unsigned char *out);

// Seek in per-channel DSD bytes: one WavPack sample is one DSD byte.
bool wavpackdec_seek_dsd(wavpackdec_t *w, uint64_t byte);

#endif /* WAVPACK_DECODE_H */
