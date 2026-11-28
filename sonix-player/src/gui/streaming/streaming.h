#ifndef STREAMING_H
#define STREAMING_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The V1 network-folder page asks whether LAN actions are real or previews.
// This build runs on the device (and in the native host simulator), not in the
// web emulator, so its actions are always real. Kept as a macro so the ported
// page compiles without a device-specific branch.
#define STREAMING_LAN_PREVIEW_ONLY 0

// No-op on the native build: there is no browser-preview action to disable.
void streaming_disable_preview_action(lv_obj_t *button, lv_obj_t *label);

// The Streaming section: the same tiled page as Music and Wireless, with a
// tile each for Tidal, Qobuz, internet radio and podcasts.
extern lv_obj_t *streaming_screen;

void streaming_init(gui_config_t *cfg);

#endif /* STREAMING_H */
