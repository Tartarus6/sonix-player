// Board geometry shared by all three devices.
//
// The compact values come from the TempoTec V1 fork (mr-f0xx). The regular
// values are the ones the HiBy builds have always used. Nothing here changes
// the HiBy layouts: bp_pick() returns the regular value unless the recognised
// model is the V1.

#include "src/gui/board_profile.h"

#include "src/system/device/sysinfo.h"

bool bp_model_is_compact(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return model && model->compact_panel;
}

bool bp_is_compact(void) { return bp_model_is_compact(); }

int bp_screen_w(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return model ? model->panel_width : 480;
}

int bp_screen_h(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return model ? model->panel_height : 720;
}

int bp_padding(void) { return bp_is_compact() ? 6 : 15; }

int bp_status_bar_h(void) { return bp_is_compact() ? 24 : 44; }

int bp_tile_radius(void) { return bp_is_compact() ? 10 : 16; }

int bp_screen_corner_radius(void) { return bp_is_compact() ? 16 : 0; }

int bp_header_button_size(void) { return bp_is_compact() ? 36 : 48; }

int bp_header_button_gap(void) { return bp_is_compact() ? 4 : 8; }

int bp_pick(int compact, int regular) { return bp_is_compact() ? compact : regular; }
