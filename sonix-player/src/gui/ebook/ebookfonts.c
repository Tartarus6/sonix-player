#include "ebookfonts.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/system/core/respath.h"

// The firmware's own copy, then the card's.
#define EBOOK_FONT_DIR SONIX_RESOURCE_DIR "/fonts/ebook"
// The interface's script faces, used as fallbacks and as the primary face for
// a Japanese book: Korean, Chinese, Japanese, Thai, Arabic and the six
// regular-weight-only subsets (Devanagari, Tamil, Malayalam, Gurmukhi,
// Ethiopic, Hebrew).
#define SCRIPT_FONT_DIR SONIX_RESOURCE_DIR "/fonts"

// The four files, in the order ebookfonts_face() falls back through.
typedef enum {
	FACE_REGULAR = 0,
	FACE_ITALIC,
	FACE_BOLD,
	FACE_BOLD_ITALIC,
	FACE_COUNT,
} face_t;

static const char *const FACE_FILES[FACE_COUNT] = {
	"Bookerly-Regular.ttf",
	"Bookerly-Italic.ttf",
	"Bookerly-Bold.ttf",
	"Bookerly-BoldItalic.ttf",
};

// Rodin has no italic: the slanted styles come back as missing and fall back
// to the upright ones.
static const char *const JAPANESE_FILES[FACE_COUNT] = {
	"japanese.otf",
	NULL,
	"japanese-bold.otf",
	NULL,
};

typedef struct {
	const char *name;
	const char *regular;
	const char *bold;
} script_face_t;

// Script fallback fonts chained behind the primary ebook face so glyphs the
// primary lacks (Hangul, CJK, Thai, Arabic, Indic, Ethiopic, Hebrew) still
// render. Korean comes first to satisfy #190 regardless of metadata hints;
// Chinese (MiSans, the interface face) comes before Japanese so Han in a
// Chinese book uses Chinese forms rather than Rodin's Japanese ones.
static const script_face_t SCRIPT_FACES[] = {
	{"korean", "korean.otf", "korean-bold.otf"},
	{"chinese", "default.otf", "bold.otf"},
	{"japanese", "japanese.otf", "japanese-bold.otf"},
	{"thai", "thai.otf", "thai-bold.otf"},
	{"arabic", "arabic.otf", "arabic-bold.otf"},
	{"devanagari", "devanagari.ttf", "devanagari.ttf"},
	{"tamil", "tamil.ttf", "tamil.ttf"},
	{"malayalam", "malayalam.ttf", "malayalam.ttf"},
	{"gurmukhi", "gurmukhi.ttf", "gurmukhi.ttf"},
	{"ethiopic", "ethiopic.ttf", "ethiopic.ttf"},
	{"hebrew", "hebrew.ttf", "hebrew.ttf"},
};

#define SCRIPT_FACE_COUNT (sizeof(SCRIPT_FACES) / sizeof(SCRIPT_FACES[0]))

static const char *const *face_files = FACE_FILES;

static char font_dir[512];
static lv_font_t *faces[FACE_COUNT];
// Which faces have already been looked for and not found, so a book full of
// italics does not try to open a missing file on every paragraph. It must be
// cleared whenever the faces are -- hence a file static rather than a static
// local inside ebookfonts_face(), which close_faces() could not reach.
static bool tried[FACE_COUNT];
static int current_size = 20;
static bool have_regular;

// Fallback fonts opened at the current size: one regular-preferred chain and
// one bold-preferred chain, shared by all primary faces.
static lv_font_t *script_regular[SCRIPT_FACE_COUNT];
static lv_font_t *script_bold[SCRIPT_FACE_COUNT];
static lv_font_t *fallback_regular_head;
static lv_font_t *fallback_bold_head;
static bool fallback_korean_open;

static lv_font_t *open_font_path(const char *path, int size) {
	if (!path || access(path, R_OK) != 0) {
		return NULL;
	}
	return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)size,
								   LV_FREETYPE_FONT_STYLE_NORMAL);
}

static lv_font_t *open_primary_face(face_t which, int size) {
	if (!face_files[which]) {
		return NULL;
	}
	char path[640];
	snprintf(path, sizeof(path), "%s/%s", font_dir, face_files[which]);
	lv_font_t *font = open_font_path(path, size);
	// Rodin's line is two ems tall -- its hhea line gap is a whole em -- which
	// sets a book double-spaced. A line of one and a half ems, the em box (0.12
	// below the baseline to 0.88 above) in the middle of it, is how Japanese
	// prose is usually leaded; the spacing setting adds to it as it does to
	// Bookerly's.
	if (font && face_files == JAPANESE_FILES) {
		font->line_height = size * 3 / 2;
		font->base_line = size * 37 / 100;
	}
	return font;
}

static void fallbacks_close(void) {
	for (size_t i = 0; i < SCRIPT_FACE_COUNT; i++) {
		if (script_regular[i]) {
			lv_freetype_font_delete(script_regular[i]);
			script_regular[i] = NULL;
		}
		if (script_bold[i]) {
			lv_freetype_font_delete(script_bold[i]);
			script_bold[i] = NULL;
		}
	}
	fallback_regular_head = NULL;
	fallback_bold_head = NULL;
	fallback_korean_open = false;
}

static void fallbacks_open(int size, bool japanese, bool korean) {
	(void)japanese;
	(void)korean;

	fallbacks_close();

	lv_font_t *reg_tail = NULL;
	lv_font_t *bold_tail = NULL;

	for (size_t i = 0; i < SCRIPT_FACE_COUNT; i++) {
		char reg_path[640];
		char bold_path[640];
		snprintf(reg_path, sizeof(reg_path), "%s/%s", SCRIPT_FONT_DIR, SCRIPT_FACES[i].regular);
		snprintf(bold_path, sizeof(bold_path), "%s/%s", SCRIPT_FONT_DIR, SCRIPT_FACES[i].bold);

		lv_font_t *reg = open_font_path(reg_path, size);
		lv_font_t *bld = open_font_path(bold_path, size);
		if (!reg && !bld) {
			continue;
		}
		script_regular[i] = reg;
		script_bold[i] = bld;
		if (i == 0 && reg) {
			fallback_korean_open = true;
		}

		// Regular-preferred chain: use the regular face when available, else
		// the bold one. Bold-preferred chain: the opposite. If the same face
		// would be used in both chains, open a separate instance so each chain
		// remains an independent linked list (lv_font_t::fallback is a single
		// pointer).
		lv_font_t *reg_choice = reg ? reg : bld;
		lv_font_t *bold_choice = bld ? bld : reg;
		lv_font_t *bold_dup = NULL;
		if (reg_choice == bold_choice) {
			bold_dup = open_font_path(reg ? reg_path : bold_path, size);
			if (bold_dup) {
				bold_choice = bold_dup;
			} else {
				// Cannot build an independent bold chain node; attach only to
				// the regular chain and keep bold chain as-is.
				bold_choice = NULL;
			}
		}

		if (reg_choice) {
			reg_choice->fallback = NULL;
			if (!fallback_regular_head) {
				fallback_regular_head = reg_choice;
			} else {
				reg_tail->fallback = reg_choice;
			}
			reg_tail = reg_choice;
		}
		if (bold_choice) {
			bold_choice->fallback = NULL;
			if (!fallback_bold_head) {
				fallback_bold_head = bold_choice;
			} else {
				bold_tail->fallback = bold_choice;
			}
			bold_tail = bold_choice;
			if (bold_dup) {
				// Track the duplicate for cleanup by hanging it off the unused
				// slot of this script when possible, else delete via the chain
				// walk at close time is not enough because we only stored reg/bld.
				// Store duplicate in script_bold when bld was NULL, else in
				// script_regular when reg was NULL (the opposite slot is free).
				if (!bld) {
					script_bold[i] = bold_dup;
				} else if (!reg) {
					script_regular[i] = bold_dup;
				} else {
					// Both original slots occupied (should not happen when
					// reg_choice == bold_choice), but keep it deletable: prefer
					// attaching as tail-owned only is unsafe. Fall back: put in
					// script_bold slot? no slot free; close will leak in this
					// impossible case, so delete duplicate and skip bold chain.
					lv_freetype_font_delete(bold_dup);
					if (bold_tail == bold_dup) {
						// unreachable given logic
					}
					bold_choice = NULL;
				}
			}
		}
	}

	// Ensure chain tails terminate.
	if (reg_tail) {
		reg_tail->fallback = NULL;
	}
	if (bold_tail) {
		bold_tail->fallback = NULL;
	}
}

static void attach_fallbacks(lv_font_t *face, bool bold_style) {
	if (!face) {
		return;
	}
	face->fallback = bold_style ? fallback_bold_head : fallback_regular_head;
}

static void attach_fallbacks_to_loaded(void) {
	attach_fallbacks(faces[FACE_REGULAR], false);
	attach_fallbacks(faces[FACE_ITALIC], false);
	attach_fallbacks(faces[FACE_BOLD], true);
	attach_fallbacks(faces[FACE_BOLD_ITALIC], true);
}

static void close_faces(void) {
	for (int i = 0; i < FACE_COUNT; i++) {
		if (faces[i]) {
			lv_freetype_font_delete(faces[i]);
			faces[i] = NULL;
		}
		tried[i] = false;
	}
	have_regular = false;
}

bool ebookfonts_open(const char *sd_root, int size, bool japanese, bool korean) {
	ebookfonts_close();

	current_size = size > 0 ? size : 20;
	face_files = japanese ? JAPANESE_FILES : FACE_FILES;

	snprintf(font_dir, sizeof(font_dir), "%s", japanese ? SCRIPT_FONT_DIR : EBOOK_FONT_DIR);
	faces[FACE_REGULAR] = open_primary_face(FACE_REGULAR, current_size);
	if (!faces[FACE_REGULAR]) {
		snprintf(font_dir, sizeof(font_dir), "%s/.local/fonts", sd_root ? sd_root : "");
		faces[FACE_REGULAR] = open_primary_face(FACE_REGULAR, current_size);
	}
	have_regular = faces[FACE_REGULAR] != NULL;
	if (!have_regular) {
		fprintf(stderr, "ebook: no %s in %s or %s; reading with the interface font\n", face_files[FACE_REGULAR],
				japanese ? SCRIPT_FONT_DIR : EBOOK_FONT_DIR, font_dir);
	}

	// Script fallbacks (every SCRIPT_FACES[] entry) so glyphs the primary
	// face lacks still render. They are attached even when the primary is the
	// interface font fallback, but in that case ebookfonts_face() returns the
	// UI font object directly.
	fallbacks_open(current_size, japanese, korean);
	attach_fallbacks_to_loaded();
	if (korean && !fallback_korean_open) {
		fprintf(stderr, "ebook: korean.otf not found in %s; Hangul may not render\n", SCRIPT_FONT_DIR);
	}

	return have_regular;
}

void ebookfonts_close(void) {
	close_faces();
	fallbacks_close();
	font_dir[0] = '\0';
}

bool ebookfonts_set_size(int size) {
	if (size <= 0 || size == current_size) {
		return have_regular;
	}

	// Which faces were in use, so the same ones come back at the new size and
	// no more: a book with no bold does not acquire the bold face because the
	// reader changed size.
	bool wanted[FACE_COUNT];
	for (int i = 0; i < FACE_COUNT; i++) {
		wanted[i] = faces[i] != NULL;
	}

	close_faces();
	current_size = size;
	fallbacks_open(current_size, face_files == JAPANESE_FILES, false);
	for (int i = 0; i < FACE_COUNT; i++) {
		if (wanted[i]) {
			faces[i] = open_primary_face((face_t)i, current_size);
		}
	}
	attach_fallbacks_to_loaded();
	have_regular = faces[FACE_REGULAR] != NULL;
	return have_regular;
}

int ebookfonts_size(void) { return current_size; }
bool ebookfonts_present(void) { return have_regular; }

// Loads a face the first time the book asks for it. `tried` is what stops a
// missing file being opened again on every paragraph; close_faces() clears it.
static lv_font_t *face_or_load(face_t which) {
	if (faces[which]) {
		return faces[which];
	}
	if (tried[which]) {
		return NULL;
	}
	tried[which] = true;
	faces[which] = open_primary_face(which, current_size);
	if (faces[which]) {
		bool bold_style = (which == FACE_BOLD || which == FACE_BOLD_ITALIC);
		attach_fallbacks(faces[which], bold_style);
	}
	return faces[which];
}

const lv_font_t *ebookfonts_face(bool bold, bool italic) {
	if (!have_regular) {
		// No Bookerly at all: the interface font, which has no italic of its
		// own either. A book is still readable; it is simply not set the way
		// its publisher set it.
		return &font_ui_20;
	}

	if (bold && italic) {
		lv_font_t *f = face_or_load(FACE_BOLD_ITALIC);
		if (f) {
			return f;
		}
		// The order the markdown asks for: italic before bold, because losing
		// the slant of a title is more noticeable than losing its weight.
		f = face_or_load(FACE_ITALIC);
		if (f) {
			return f;
		}
		f = face_or_load(FACE_BOLD);
		if (f) {
			return f;
		}
		return faces[FACE_REGULAR];
	}
	if (italic) {
		lv_font_t *f = face_or_load(FACE_ITALIC);
		return f ? f : faces[FACE_REGULAR];
	}
	if (bold) {
		lv_font_t *f = face_or_load(FACE_BOLD);
		return f ? f : faces[FACE_REGULAR];
	}
	return faces[FACE_REGULAR];
}
