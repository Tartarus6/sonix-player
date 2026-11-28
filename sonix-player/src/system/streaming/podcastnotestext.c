#include "podcastnotestext.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/ebook/epub_xml.h"

typedef struct {
	char *text;
	size_t used;
	unsigned breaks;
	bool space;
	bool full;
} note_t;

static void line_break(note_t *out, unsigned lines) {
	if (out->breaks < lines) {
		out->breaks = lines;
	}
	out->space = false;
}

static void append(note_t *out, const char *s, size_t n) {
	size_t gap = out->used ? (out->breaks ? out->breaks : out->space) : 0;
	if (out->full || out->used + gap + n > PODCAST_NOTES_MAX) {
		out->full = true;
		return;
	}
	memset(out->text + out->used, out->breaks ? '\n' : ' ', gap);
	out->used += gap;
	memcpy(out->text + out->used, s, n);
	out->used += n;
	out->breaks = 0;
	out->space = false;
}

// These lead bytes narrow the range of the first continuation byte.
static bool utf8_first_pair(unsigned lead, unsigned next) {
	switch (lead) {
	case 0xE0:
		return next >= 0xA0; // no overlong three-byte form
	case 0xED:
		return next < 0xA0; // no surrogates
	case 0xF0:
		return next >= 0x90; // no overlong four-byte form
	case 0xF4:
		return next < 0x90; // at most U+10FFFF
	default:
		return true;
	}
}

// Zero for an invalid or cut-off code point.
static size_t utf8_size(const unsigned char *p, size_t left) {
	unsigned c = p[0];
	if (c < 0x80) {
		return 1;
	}
	size_t n = c < 0xC2 ? 0 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c <= 0xF4 ? 4 : 0;
	if (n == 0 || n > left) {
		return 0;
	}
	for (size_t i = 1; i < n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			return 0;
		}
	}
	return utf8_first_pair(c, p[1]) ? n : 0;
}

// Bytes consumed from the source, or zero if it is not a recognised entity.
static size_t read_entity(const char *s, size_t len, char *decoded, size_t cap, size_t *decoded_len) {
	if (*s != '&') {
		return 0;
	}
	const char *semi = memchr(s, ';', len < cap ? len : cap);
	if (!semi) {
		return 0;
	}
	size_t count = (size_t)(semi - s) + 1;
	*decoded_len = epub_xml_unescape(decoded, cap, s, count);
	return *decoded_len > 0 && *decoded_len < count ? count : 0;
}

static size_t append_character(note_t *out, const char *p, size_t available) {
	unsigned c = (unsigned char)*p;
	size_t n = utf8_size((const unsigned char *)p, available);
	if (!n) {
		append(out, "\xEF\xBF\xBD", 3);
		return 1;
	}
	if (c == '\n' || c == '\r') {
		line_break(out, out->breaks == 0 ? 1 : 2);
		return c == '\r' && available > 1 && p[1] == '\n' ? 2 : 1;
	}
	if (isspace(c) || (n == 2 && c == 0xC2 && (unsigned char)p[1] == 0xA0)) {
		out->space = true;
	} else if (c >= 0x20 && c != 0x7F) {
		append(out, p, n);
	}
	return n;
}

static void append_text(note_t *out, const char *s, size_t len) {
	for (size_t i = 0; i < len && !out->full;) {
		char entity[12];
		size_t decoded;
		size_t consumed = read_entity(s + i, len - i, entity, sizeof(entity), &decoded);
		if (consumed) {
			append_character(out, entity, decoded);
			i += consumed;
		} else {
			i += append_character(out, s + i, len - i);
		}
	}
}

static bool next_note_token(epub_xml_t *token) {
	// The tokenizer accepts digits in tag names, but '<3' in notes is text.
	const char *p = token->cursor;
	if (*p == '<' && p[1] >= '0' && p[1] <= '9') {
		token->event = EPUB_XML_TEXT;
		token->text = p;
		token->text_len = 1;
		token->name = NULL;
		token->name_len = 0;
		token->cursor++;
		return true;
	}
	return epub_xml_next(token);
}

// Script, style and iframe bodies are raw text, not HTML tokens. A '<' in
// JavaScript must not swallow the end tag and hide the note that follows it.
static const char *after_raw_text(const char *p, const char *name) {
	size_t n = strlen(name);
	while ((p = strchr(p, '<'))) {
		p++;
		if (*p == '/' && strncasecmp(p + 1, name, n) == 0 && (p[n + 1] == '>' || isspace((unsigned char)p[n + 1]))) {
			p = strchr(p + n + 1, '>');
			return p ? p + 1 : NULL;
		}
	}
	return NULL;
}

static void skip_element_body(epub_xml_t *token, const char *name) {
	unsigned depth = 1;
	while (next_note_token(token)) {
		if (!epub_xml_is(token, name)) {
			continue;
		}
		if (token->event == EPUB_XML_OPEN && !token->self_closing) {
			depth++;
		} else if (token->event == EPUB_XML_CLOSE && --depth == 0) {
			return;
		}
	}
}

static bool skip_embedded(epub_xml_t *token) {
	static const struct {
		const char *name;
		bool raw_text;
	} TAGS[] = {{"script", true}, {"style", true}, {"iframe", true}, {"object", false}, {"svg", false}, {"audio", false}, {"video", false}, {"template", false}};
	if (token->event != EPUB_XML_OPEN) {
		return false;
	}
	for (size_t i = 0; i < sizeof(TAGS) / sizeof(*TAGS); i++) {
		if (!epub_xml_is(token, TAGS[i].name)) {
			continue;
		}
		if (TAGS[i].raw_text) {
			const char *end = after_raw_text(token->cursor, TAGS[i].name);
			token->cursor = end ? end : "";
		} else if (!token->self_closing) {
			skip_element_body(token, TAGS[i].name);
		}
		return true;
	}
	return false;
}

static bool is_heading(const epub_xml_t *token) { return token->name_len == 2 && (token->name[0] == 'h' || token->name[0] == 'H') && token->name[1] >= '1' && token->name[1] <= '6'; }

static void append_markup(note_t *out, const epub_xml_t *token) {
	bool open = token->event == EPUB_XML_OPEN;
	if (epub_xml_is(token, "br") && open) {
		line_break(out, out->breaks == 0 ? 1 : 2);
	} else if (is_heading(token) || epub_xml_is(token, "p") || epub_xml_is(token, "blockquote") || epub_xml_is(token, "hr")) {
		line_break(out, 2);
	} else if (epub_xml_is(token, "li")) {
		line_break(out, 1);
		if (open) {
			append(out, "•", sizeof("•") - 1);
			out->space = true;
		}
	} else if (epub_xml_is(token, "div") || epub_xml_is(token, "ul") || epub_xml_is(token, "ol")) {
		line_break(out, 1);
	}
}

podcast_notes_result_t podcast_notes_to_text(const char *html, char **text) {
	*text = NULL;
	note_t out = {.text = malloc(PODCAST_NOTES_MAX + 1)};
	if (!out.text) {
		return PODCAST_NOTES_ERROR;
	}
	epub_xml_t token;
	epub_xml_init(&token, html);
	while (!out.full && next_note_token(&token)) {
		if (token.event == EPUB_XML_TEXT) {
			append_text(&out, token.text, token.text_len);
		} else if (!skip_embedded(&token)) {
			append_markup(&out, &token);
		}
	}
	if (!out.used) {
		free(out.text);
		return PODCAST_NOTES_EMPTY;
	}
	out.text[out.used] = '\0';
	char *trimmed = realloc(out.text, out.used + 1);
	*text = trimmed ? trimmed : out.text;
	return PODCAST_NOTES_OK;
}
