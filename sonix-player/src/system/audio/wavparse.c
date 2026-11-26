#include "wavparse.h"

#include <stdlib.h>
#include <string.h>

// The chunk layout of the three containers differs only in the size of the
// chunk header and in where the real data length is written. RIFF and RF64
// share the eight-byte "id + 32-bit size" header and even chunk padding;
// Wave64 uses a 16-byte GUID and an eight-byte size that counts the header
// itself, with eight-byte padding.

// Sony's Wave64 top-level and chunk GUIDs. The four ASCII bytes at the front
// of each identify the chunk; the rest is the same for every chunk except the
// outer RIFF one.
static const unsigned char W64_RIFF[16] = {0x72, 0x69, 0x66, 0x66, 0x2e, 0x91, 0xcf, 0x11,
										  0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00};
static const unsigned char W64_WAVE[16] = {'w', 'a', 'v', 'e', 0xf3, 0xac, 0xd3, 0x11,
										  0x8c, 0xd1, 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};
static const unsigned char W64_SUFFIX[12] = {0xf3, 0xac, 0xd3, 0x11, 0x8c, 0xd1,
											 0x00, 0xc0, 0x4f, 0x8e, 0xdb, 0x8a};

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32); }

typedef enum {
	CONTAINER_RIFF,
	CONTAINER_RF64,
	CONTAINER_W64,
} wav_container_t;

static bool read_at(FILE *f, int64_t off, void *buf, size_t len) {
	return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

int wav_parse(const char *filepath, wav_info_t *info, FILE **file_out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		fprintf(stderr, "Audio: Failed to open file: %s\n", filepath);
		return WAV_BAD;
	}

	memset(info, 0, sizeof(*info));

	unsigned char head[24];
	if (fread(head, 1, sizeof(head), f) != sizeof(head)) {
		fclose(f);
		return WAV_BAD;
	}

	wav_container_t kind;
	if (memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
		kind = CONTAINER_RIFF;
	} else if (memcmp(head, "RF64", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
		kind = CONTAINER_RF64;
	} else if (memcmp(head, W64_RIFF, sizeof(W64_RIFF)) == 0) {
		// The wave GUID starts at byte 24, past the eight-byte file size;
		// checked below once the first chunk header has been read.
		kind = CONTAINER_W64;
	} else {
		fclose(f);
		return WAV_BAD;
	}

	if (fseeko(f, 0, SEEK_END) != 0) {
		fclose(f);
		return WAV_BAD;
	}
	int64_t file_end = (int64_t)ftello(f);
	if (file_end < 0) {
		fclose(f);
		return WAV_BAD;
	}

	// Wave64 puts the wave GUID and its size between the outer header and
	// the first chunk. Verify it, then start the walk after both.
	int64_t pos = kind == CONTAINER_W64 ? 24 : 12;
	if (kind == CONTAINER_W64) {
		unsigned char wave[24];
		if (!read_at(f, 24, wave, sizeof(wave)) || memcmp(wave, W64_WAVE, sizeof(W64_WAVE)) != 0) {
			fclose(f);
			return WAV_BAD;
		}
		pos = 48;
	}

	int tag = 0;
	int bits = 0;
	bool have_fmt = false;
	uint64_t ds64_data = 0;
	bool have_ds64 = false;

	for (;;) {
		unsigned char chunk[24];
		if (pos + (kind == CONTAINER_W64 ? 24 : 8) > file_end) {
			break;
		}
		if (!read_at(f, pos, chunk, kind == CONTAINER_W64 ? sizeof(chunk) : 8)) {
			fclose(f);
			return WAV_BAD;
		}

		int64_t body;
		int64_t declared; /* payload bytes */
		int64_t next;
		unsigned char id[4];
		memcpy(id, chunk, 4);

		if (kind == CONTAINER_W64) {
			uint64_t size = le64(chunk + 16); // includes GUID and size
			if (size < 24) {
				break;
			}
			body = pos + 24;
			declared = (int64_t)(size - 24);
			next = (int64_t)((uint64_t)(pos + size) + 7u) & ~(int64_t)7;
		} else {
			uint32_t size = le32(chunk + 4);
			body = pos + 8;
			declared = (int64_t)size;
			next = body + declared + (declared & 1);
		}

		if (kind == CONTAINER_W64 && memcmp(chunk + 4, W64_SUFFIX, sizeof(W64_SUFFIX)) != 0) {
			fclose(f);
			return WAV_BAD;
		}

		if (memcmp(id, "ds64", 4) == 0 && kind == CONTAINER_RF64) {
			unsigned char ds[16];
			if (declared < 16 || !read_at(f, body, ds, sizeof(ds))) {
				fclose(f);
				return WAV_BAD;
			}
			ds64_data = le64(ds + 8);
			have_ds64 = true;
		} else if (memcmp(id, "fmt ", 4) == 0) {
			// Up to the end of WAVEFORMATEXTENSIBLE: 16 bytes of WAVEFORMAT,
			// the extension size, valid bits, channel mask and the sub-format
			// GUID, whose first two bytes are the real format tag.
			unsigned char fmt[40];
			int64_t want = declared < (int64_t)sizeof(fmt) ? declared : (int64_t)sizeof(fmt);
			if (want < 16 || !read_at(f, body, fmt, (size_t)want)) {
				fclose(f);
				return WAV_BAD;
			}
			tag = le16(fmt);
			info->channels = le16(fmt + 2);
			info->sample_rate = (int)le32(fmt + 4);
			bits = le16(fmt + 14);
			if (tag == WAVE_FORMAT_EXTENSIBLE) {
				tag = want >= 26 ? le16(fmt + 24) : 0;
			}
			have_fmt = true;
		} else if (memcmp(id, "data", 4) == 0) {
			info->data_offset = body;
			// RF64 writes 0xFFFFFFFF here and the real length in ds64.
			if (kind == CONTAINER_RF64 && declared == 0xFFFFFFFF && have_ds64) {
				declared = (int64_t)ds64_data;
			}
			// 0 and 0xFFFFFFFF are what a writer leaves when it never went
			// back to fill the length in, and a length past the end of the
			// file is a truncated copy: in all three cases the audio runs to
			// the end of the file.
			if (declared <= 0 || declared == 0xFFFFFFFF || body + declared > file_end) {
				declared = file_end - body;
			}
			info->data_size = declared;
			break;
		}

		if (next <= pos) {
			break;
		}
		pos = next;
	}

	if (!have_fmt || !info->data_offset || info->channels <= 0 || info->channels > 8 || info->sample_rate <= 0) {
		fclose(f);
		return WAV_BAD;
	}

	info->bits_per_sample = bits;
	if (tag == WAVE_FORMAT_PCM && bits == 8) {
		info->sample = WAV_PCM_U8;
	} else if (tag == WAVE_FORMAT_PCM && bits == 16) {
		info->sample = WAV_PCM_S16;
	} else if (tag == WAVE_FORMAT_PCM && bits == 24) {
		info->sample = WAV_PCM_S24;
	} else if (tag == WAVE_FORMAT_PCM && bits == 32) {
		info->sample = WAV_PCM_S32;
	} else if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
		info->sample = WAV_FLOAT32;
	} else if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 64) {
		info->sample = WAV_FLOAT64;
	} else {
		fprintf(stderr, "audio: '%s' is WAV format 0x%04x at %d bits, not read by the WAV path\n", filepath, tag,
				bits);
		fclose(f);
		return WAV_UNSUPPORTED;
	}

	info->src_frame_bytes = info->channels * (bits / 8);
	info->out_bits = (info->sample == WAV_PCM_U8 || info->sample == WAV_PCM_S16) ? 16 : 32;

	// Whole frames only.
	info->data_size -= info->data_size % info->src_frame_bytes;

	fseeko(f, (off_t)info->data_offset, SEEK_SET);
	*file_out = f;
	return WAV_OK;
}
