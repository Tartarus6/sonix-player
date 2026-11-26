// board_profile.h -- shared geometry for the three supported panels.
//
// One binary serves the HiBy R1 (480x800), HiBy R3 Pro II (480x720) and
// TempoTec Variations V1 (240x320). The two HiBy panels keep the long-standing
// padding (15) and status bar (44). The V1 uses the compact values developed in
// the TempoTec fork; they are genuine layout sizes, not a scale factor applied
// to the HiBy layout. The answer is derived from the device model at runtime,
// so the same binary can be packed for any of the three devices.

#ifndef SRC_GUI_BOARD_PROFILE_H_
#define SRC_GUI_BOARD_PROFILE_H_

#include <stdbool.h>

// True for the TempoTec V1's 240x320 panel. False for both HiBy models and
// when no model is known (the simulator default).
bool bp_is_compact(void);

// Geometry used by the shell before gui_config_t exists (fonts_init) and by
// pages that already have a config.
int bp_screen_w(void);
int bp_screen_h(void);
int bp_padding(void);
int bp_status_bar_h(void);
int bp_tile_radius(void);
int bp_screen_corner_radius(void);
int bp_header_button_size(void);
int bp_header_button_gap(void);

// Compact/regular pairs: the regular value is returned on HiBy panels, the
// compact one on the V1. This preserves existing HiBy layouts exactly.
int bp_pick(int compact, int regular);

// The compact flag from the model table; kept as a helper for callers that
// already include sysinfo.h but want a single question to ask.
bool bp_model_is_compact(void);

#endif // SRC_GUI_BOARD_PROFILE_H_
