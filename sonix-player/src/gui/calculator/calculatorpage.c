#include "calculatorpage.h"

#include <stdint.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/calculator/calc_engine.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *calculatorpage_screen;

// ---------------------------------------------------------------------------
// Look  (fixed colours taken from the original, not from the theme)
// ---------------------------------------------------------------------------

#define BODY_TOP       0x2B2B2B
#define BODY_BOTTOM    0x0E0E0E
#define LCD_TOP        0xE9EEE6
#define LCD_BOTTOM     0xC4CCC1
#define LCD_BORDER     0x8A9187
#define LCD_TEXT       0x1E1E1E

#define BODY_PAD    14
#define BODY_RADIUS 18
#define KEY_GAP     10
#define KEY_RADIUS  10
#define LCD_HEIGHT  130
#define LCD_PAD     16

typedef enum { KIND_DARK, KIND_LIGHT, KIND_EQUALS } key_kind_t;

typedef struct {
	uint32_t top, bottom;         // gradient at rest
	uint32_t top_down, bottom_down; // gradient while pressed
	uint32_t text;
} key_look_t;

static const key_look_t LOOKS[] = {
	[KIND_DARK]   = {0x6E6E6E, 0x3C3C3C, 0x3C3C3C, 0x262626, 0xFFFFFF},
	[KIND_LIGHT]  = {0xF2F2F2, 0xC9C9C9, 0xBDBDBD, 0xA0A0A0, 0x2A2A2A},
	[KIND_EQUALS] = {0xFFA640, 0xE8700A, 0xD86506, 0xB85000, 0xFFFFFF},
};

// ---------------------------------------------------------------------------
// Keys  (the iPhone OS / iPod touch portrait layout)
//
//   mc   m+   m-   mr
//   C    ±    ÷    ×
//   7    8    9    −
//   4    5    6    +
//   1    2    3    =
//   0 (two wide) . =   ("=" is two tall)
// ---------------------------------------------------------------------------

typedef enum {
	K_0 = 0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
	K_DOT, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_CLEAR, K_NEG,
	K_MC, K_MPLUS, K_MMINUS, K_MR,
} key_code_t;

typedef struct {
	const char *text;
	key_code_t code;
	key_kind_t kind;
	uint8_t col, row, cols, rows;
} key_def_t;

static const key_def_t KEYS[] = {
	{"mc", K_MC, KIND_DARK, 0, 0, 1, 1},
	{"m+", K_MPLUS, KIND_DARK, 1, 0, 1, 1},
	{"m-", K_MMINUS, KIND_DARK, 2, 0, 1, 1},
	{"mr", K_MR, KIND_DARK, 3, 0, 1, 1},
	{"C", K_CLEAR, KIND_DARK, 0, 1, 1, 1},
	{"±", K_NEG, KIND_DARK, 1, 1, 1, 1},
	{"÷", K_DIV, KIND_DARK, 2, 1, 1, 1},
	{"×", K_MUL, KIND_DARK, 3, 1, 1, 1},
	{"7", K_7, KIND_LIGHT, 0, 2, 1, 1},
	{"8", K_8, KIND_LIGHT, 1, 2, 1, 1},
	{"9", K_9, KIND_LIGHT, 2, 2, 1, 1},
	{"−", K_SUB, KIND_DARK, 3, 2, 1, 1},
	{"4", K_4, KIND_LIGHT, 0, 3, 1, 1},
	{"5", K_5, KIND_LIGHT, 1, 3, 1, 1},
	{"6", K_6, KIND_LIGHT, 2, 3, 1, 1},
	{"+", K_ADD, KIND_DARK, 3, 3, 1, 1},
	{"1", K_1, KIND_LIGHT, 0, 4, 1, 1},
	{"2", K_2, KIND_LIGHT, 1, 4, 1, 1},
	{"3", K_3, KIND_LIGHT, 2, 4, 1, 1},
	{"=", K_EQ, KIND_EQUALS, 3, 4, 1, 2},
	{"0", K_0, KIND_LIGHT, 0, 5, 2, 1},
	{".", K_DOT, KIND_LIGHT, 2, 5, 1, 1},
};

#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))
#define KEY_COLS 4
#define KEY_ROWS 6

static lv_obj_t *display_label;
static lv_obj_t *memory_mark;
static lv_obj_t *op_keys[4]; // ADD, SUB, MUL, DIV, for the highlight
static int display_width;

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Next code point of a UTF-8 string; advances *p. The translated "Error" can
// be Cyrillic or Japanese, so bytes are not letters.
static uint32_t next_utf8(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t c = s[0];
	int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
	if (extra) {
		c &= 0x3F >> extra;
	}
	int i = 1;
	for (; i <= extra && (s[i] & 0xC0) == 0x80; i++) {
		c = (c << 6) | (s[i] & 0x3F);
	}
	*p += i;
	return c;
}

static int text_width(const char *text, const lv_font_t *font) {
	int w = 0;
	const char *p = text;
	uint32_t c = *p ? next_utf8(&p) : 0;
	while (c) {
		uint32_t next = *p ? next_utf8(&p) : 0;
		w += (int)lv_font_get_glyph_width(font, c, next);
		c = next;
	}
	return w;
}

// The largest face the number fits the display with, like the original's
// shrinking digits.
static const lv_font_t *display_font(const char *text) {
	const lv_font_t *const faces[] = {&font_ui_72, &font_ui_64_bold, &font_ui_36_bold};
	for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
		if (text_width(text, faces[i]) <= display_width) {
			return faces[i];
		}
	}
	return &font_ui_28;
}

static void refresh(void) {
	const char *text = calc_error() ? tr("calculator_error") : calc_display();
	lv_label_set_text(display_label, text);
	lv_obj_set_style_text_font(display_label, display_font(text), 0);

	lv_obj_set_hidden(memory_mark, !calc_has_memory());

	// The pending operator's key keeps a white outline, as on the original.
	calc_op_t lit = calc_highlighted_op();
	const calc_op_t ops[4] = {CALC_OP_ADD, CALC_OP_SUB, CALC_OP_MUL, CALC_OP_DIV};
	for (int i = 0; i < 4; i++) {
		bool on = lit == ops[i];
		lv_obj_set_style_border_color(op_keys[i], on ? lv_color_white() : lv_color_hex(0x111111), 0);
		lv_obj_set_style_border_width(op_keys[i], on ? 3 : 1, 0);
	}
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

static void key_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return; // a swipe back that started on a key, not a press
	}
	key_code_t code = (key_code_t)(intptr_t)lv_event_get_user_data(e);
	switch (code) {
	case K_DOT: calc_dot(); break;
	case K_ADD: calc_op(CALC_OP_ADD); break;
	case K_SUB: calc_op(CALC_OP_SUB); break;
	case K_MUL: calc_op(CALC_OP_MUL); break;
	case K_DIV: calc_op(CALC_OP_DIV); break;
	case K_EQ: calc_equals(); break;
	case K_CLEAR: calc_clear(); break;
	case K_NEG: calc_negate(); break;
	case K_MC: calc_mem_clear(); break;
	case K_MPLUS: calc_mem_add(); break;
	case K_MMINUS: calc_mem_sub(); break;
	case K_MR: calc_mem_recall(); break;
	default:
		if (code >= K_0 && code <= K_9) {
			calc_digit((int)code);
		}
		break;
	}
	refresh();
}

static lv_obj_t *make_key(lv_obj_t *parent, const key_def_t *def, int x, int y, int w, int h) {
	const key_look_t *look = &LOOKS[def->kind];

	lv_obj_t *key = lv_button_create(parent);
	lv_obj_remove_style_all(key);
	lv_obj_set_pos(key, x, y);
	lv_obj_set_size(key, w, h);
	lv_obj_set_style_radius(key, KEY_RADIUS, 0);
	lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(key, lv_color_hex(look->top), 0);
	lv_obj_set_style_bg_grad_color(key, lv_color_hex(look->bottom), 0);
	lv_obj_set_style_bg_grad_dir(key, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_color(key, lv_color_hex(look->top_down), LV_STATE_PRESSED);
	lv_obj_set_style_bg_grad_color(key, lv_color_hex(look->bottom_down), LV_STATE_PRESSED);
	lv_obj_set_style_border_width(key, 1, 0);
	lv_obj_set_style_border_color(key, lv_color_hex(0x111111), 0);
	lv_obj_set_scrollable(key, false);
	lv_obj_add_event_cb(key, key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)def->code);

	lv_obj_t *label = lv_label_create(key);
	lv_label_set_text(label, def->text);
	lv_obj_set_style_text_color(label, lv_color_hex(look->text), 0);
	// The memory row is lower-case words; the rest are single big glyphs.
	bool memory_row = def->row == 0;
	lv_obj_set_style_text_font(label, memory_row ? &font_ui_26 : &font_ui_36_bold, 0);
	lv_obj_center(label);
	return key;
}

// ---------------------------------------------------------------------------
// calculatorpage_init
// ---------------------------------------------------------------------------

void calculatorpage_init(gui_config_t *cfg) {
	calc_reset();

	lv_obj_add_style(calculatorpage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(calculatorpage_screen, false);
	settingsrow_title(calculatorpage_screen, cfg, "calculator");

	int top = settingsrow_content_top(cfg);
	int body_w = (int)cfg->screen_width - 2 * cfg->padding;
	int body_h = (int)cfg->screen_height - top - cfg->padding;

	// The black body of the calculator.
	lv_obj_t *body = lv_obj_create(calculatorpage_screen);
	lv_obj_remove_style_all(body);
	lv_obj_set_pos(body, cfg->padding, top);
	lv_obj_set_size(body, body_w, body_h);
	lv_obj_set_style_radius(body, BODY_RADIUS, 0);
	lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(body, lv_color_hex(BODY_TOP), 0);
	lv_obj_set_style_bg_grad_color(body, lv_color_hex(BODY_BOTTOM), 0);
	lv_obj_set_style_bg_grad_dir(body, LV_GRAD_DIR_VER, 0);
	lv_obj_set_scrollable(body, false);
	switcher_attach_back_gesture(body);

	// The display: a light LCD with dark digits, right-aligned.
	int inner_w = body_w - 2 * BODY_PAD;
	lv_obj_t *lcd = lv_obj_create(body);
	lv_obj_remove_style_all(lcd);
	lv_obj_set_pos(lcd, BODY_PAD, BODY_PAD);
	lv_obj_set_size(lcd, inner_w, LCD_HEIGHT);
	lv_obj_set_style_radius(lcd, KEY_RADIUS, 0);
	lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(lcd, lv_color_hex(LCD_TOP), 0);
	lv_obj_set_style_bg_grad_color(lcd, lv_color_hex(LCD_BOTTOM), 0);
	lv_obj_set_style_bg_grad_dir(lcd, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_border_width(lcd, 1, 0);
	lv_obj_set_style_border_color(lcd, lv_color_hex(LCD_BORDER), 0);
	lv_obj_set_scrollable(lcd, false);
	lv_obj_set_clickable(lcd, false);
	display_width = inner_w - 2 * LCD_PAD;

	display_label = lv_label_create(lcd);
	lv_obj_set_style_text_color(display_label, lv_color_hex(LCD_TEXT), 0);
	lv_obj_align(display_label, LV_ALIGN_RIGHT_MID, -LCD_PAD, 0);

	memory_mark = lv_label_create(lcd);
	lv_label_set_text(memory_mark, "M");
	lv_obj_set_style_text_color(memory_mark, lv_color_hex(LCD_TEXT), 0);
	lv_obj_set_style_text_font(memory_mark, &font_ui_20_bold, 0);
	lv_obj_align(memory_mark, LV_ALIGN_TOP_LEFT, 12, 8);

	// The keys share what the display leaves, in a 4 x 6 grid.
	int keys_top = BODY_PAD + LCD_HEIGHT + BODY_PAD;
	int key_w = (inner_w - (KEY_COLS - 1) * KEY_GAP) / KEY_COLS;
	int key_h = (body_h - keys_top - BODY_PAD - (KEY_ROWS - 1) * KEY_GAP) / KEY_ROWS;
	for (int i = 0; i < KEY_COUNT; i++) {
		const key_def_t *def = &KEYS[i];
		int x = BODY_PAD + def->col * (key_w + KEY_GAP);
		int y = keys_top + def->row * (key_h + KEY_GAP);
		int w = def->cols * key_w + (def->cols - 1) * KEY_GAP;
		int h = def->rows * key_h + (def->rows - 1) * KEY_GAP;
		lv_obj_t *key = make_key(body, def, x, y, w, h);
		switch (def->code) {
		case K_ADD: op_keys[0] = key; break;
		case K_SUB: op_keys[1] = key; break;
		case K_MUL: op_keys[2] = key; break;
		case K_DIV: op_keys[3] = key; break;
		default: break;
		}
	}

	refresh();
}
