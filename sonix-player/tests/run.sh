#!/bin/sh
# Builds and runs the host tests. Invoked as `sh tests/run.sh` from the project
# root; it finds the root itself otherwise.
set -e
cd "$(dirname "$0")/.."

# The playback-start volume safety clamp, against the real alsa-controls.c.
cc -O2 -Wall -Wextra -I. -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 \
   -o /tmp/volume_safety_test tests/volume_safety_test.c \
   src/system/audio/alsa-controls.c -lasound -pthread
/tmp/volume_safety_test
