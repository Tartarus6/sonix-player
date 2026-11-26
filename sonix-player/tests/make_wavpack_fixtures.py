#!/usr/bin/env python3
"""Build a PCM .wv and a DSD .wv for the WavPack routing test.

The PCM file is a short 16-bit stereo WAV with a known ramp. The DSD file is
the .dsf from make_dsd_fixtures.py transcoded by the wavpack CLI, so it holds a
DSD bitstream inside the WavPack container. Both are written with the same tool
the player links against, so the test reads real .wv files rather than a mock.

Usage: make_wavpack_fixtures.py <output-directory>
"""

import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_dsd_fixtures  # noqa: E402

RATE = 44100
CHANNELS = 2
FRAMES = 4096


def write_pcm_wav(path):
    frames = bytearray()
    for i in range(FRAMES):
        for c in range(CHANNELS):
            value = (i * 7 + c * 1000 + 1234) & 0x7FFF
            frames += struct.pack("<H", value)
    data = bytes(frames)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, CHANNELS, RATE,
                                     RATE * CHANNELS * 2, CHANNELS * 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(header + data)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_wavpack_fixtures.py <output-directory>\n")
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    tool = os.environ.get("WAVPACK") or shutil.which("wavpack")
    if not tool:
        sys.stderr.write("wavpack CLI not found; set WAVPACK to its path\n")
        return 2

    wav = os.path.join(out, "pcm.wav")
    dsf = os.path.join(out, "dsd-source.dsf")
    write_pcm_wav(wav)
    make_dsd_fixtures.write_dsf(dsf)

    for src, dst in ((wav, os.path.join(out, "pcm.wv")),
                     (dsf, os.path.join(out, "dsd.wv"))):
        subprocess.run([tool, "-q", "-y", src, "-o", dst], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
