#!/usr/bin/env python3
"""Build WAV, RF64 and Wave64 fixtures with the same known PCM payload.

Every file holds 256 frames of 16-bit stereo at 44100 Hz, with a ramp that the
test reads back through wav_parse. The headers carry the chunk layout each
container uses:

  WAV     "RIFF"/"WAVE", 4-byte chunk sizes, padded to 2 bytes.
  RF64    "RF64"/"WAVE", a ds64 chunk with the 64-bit data size, and a data
          chunk whose 32-bit size is 0xFFFFFFFF.
  Wave64  16-byte GUIDs, 8-byte chunk sizes that include the 24-byte header,
          padding to 8 bytes. The chunk IDs are the first four GUID bytes.

A fourth file declares a data size past EOF, to check the truncated-file
fallback. A fifth uses an unsupported format tag, to check WAV_UNSUPPORTED.

Usage: make_wavparse_fixtures.py <output-directory>
"""

import os
import struct
import sys

RATE = 44100
CHANNELS = 2
FRAMES = 256

RIFF_GUID = bytes.fromhex("72696666" "2e91cf11" "a5d628db" "04c10000")
CHUNK_SUFFIX = bytes.fromhex("f3acd311" "8cd100c0" "4f8edb8a")
WAVE_GUID = b"wave" + CHUNK_SUFFIX
FMT_GUID = b"fmt " + CHUNK_SUFFIX
DATA_GUID = b"data" + CHUNK_SUFFIX


def pcm_data():
    out = bytearray()
    for i in range(FRAMES):
        for c in range(CHANNELS):
            out += struct.pack("<h", (i * 11 + c * 100 + 7) & 0x7FFF)
    return bytes(out)


def fmt_body(tag=1, bits=16):
    block = CHANNELS * bits // 8
    return struct.pack("<HHIIHH", tag, CHANNELS, RATE, RATE * block, block, bits)


def riff_chunk(name, body):
    pad = b"\0" if len(body) & 1 else b""
    return name + struct.pack("<I", len(body)) + body + pad


def write_wav(path, data, tag=1, bits=16):
    body = b"WAVE" + riff_chunk(b"fmt ", fmt_body(tag, bits)) + riff_chunk(b"data", data)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)


def write_rf64(path, data, declared=None):
    ds64_body = struct.pack("<QQQ", 0, len(data) if declared is None else declared, FRAMES)
    chunks = b"WAVE"
    chunks += riff_chunk(b"ds64", ds64_body)
    chunks += riff_chunk(b"fmt ", fmt_body())
    chunks += b"data" + struct.pack("<I", 0xFFFFFFFF) + data + (b"\0" if len(data) & 1 else b"")
    with open(path, "wb") as f:
        f.write(b"RF64" + struct.pack("<I", 0xFFFFFFFF) + chunks)


def w64_chunk(guid, body):
    size = 24 + len(body)
    pad = b"\0" * ((8 - (size & 7)) & 7)
    return guid + struct.pack("<Q", size) + body + pad


def write_w64(path, data):
    chunks = w64_chunk(FMT_GUID, fmt_body()) + w64_chunk(DATA_GUID, data)
    wave = WAVE_GUID + struct.pack("<Q", 24 + len(chunks)) + chunks
    with open(path, "wb") as f:
        f.write(RIFF_GUID + struct.pack("<Q", 24 + len(wave)) + wave)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_wavparse_fixtures.py <output-directory>\n")
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    data = pcm_data()
    write_wav(os.path.join(out, "plain.wav"), data)
    write_rf64(os.path.join(out, "rf64.wav"), data)
    write_w64(os.path.join(out, "wave64.wav"), data)
    write_rf64(os.path.join(out, "truncated.wav"), data, declared=len(data) + 4096)
    write_wav(os.path.join(out, "unsupported.wav"), data, tag=6, bits=8)
    return 0


if __name__ == "__main__":
    sys.exit(main())
