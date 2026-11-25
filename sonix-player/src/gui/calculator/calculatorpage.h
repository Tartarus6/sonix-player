#ifndef CALCULATORPAGE_H
#define CALCULATORPAGE_H

#include "src/gui/shell/gui.h"
#include "lvgl/lvgl.h"

// The calculator, from the More page and the Pod home: the classic iPod
// touch calculator in portrait. A light LCD-style display over a black body,
// dark keys for memory and operators, light keys for digits, and a tall
// orange "=". The arithmetic lives in calc_engine.c.
//
// Its fixed colours do not follow the dark/light theme: the body is a picture
// of the original, not a page of rows. The state stays put while the page is
// away, as on the original.
extern lv_obj_t *calculatorpage_screen;
void calculatorpage_init(gui_config_t *cfg);

#endif /* CALCULATORPAGE_H */
