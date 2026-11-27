#include "dbuslite_stub.h"

#include <stdio.h>
#include <string.h>

#define WRITE_BUF_MAX (16 * 1024)

struct dbus_writer {
	uint8_t buf[WRITE_BUF_MAX];
	size_t used;
	bool overflow;
};

static dbus_writer_t reply_writer;
static dbus_writer_t scratch_writer;
static dbus_test_result_t last;

const dbus_test_result_t *dbus_test_result(void) { return &last; }

void dbus_test_reset(void) {
	memset(&last, 0, sizeof(last));
	memset(&reply_writer, 0, sizeof(reply_writer));
}

void dbus_test_scratch_reset(void) { memset(&scratch_writer, 0, sizeof(scratch_writer)); }
dbus_writer_t *dbus_test_scratch(void) { return &scratch_writer; }

size_t dbus_test_scratch_copy(uint8_t *dst, size_t size) {
	size_t n = scratch_writer.used < size ? scratch_writer.used : size;
	memcpy(dst, scratch_writer.buf, n);
	return n;
}

// ---------------------------------------------------------------------------
// Writer, identical alignment rules to the production dbuslite.
// ---------------------------------------------------------------------------

static void w_pad(dbus_writer_t *w, size_t align) {
	while (w->used % align) {
		if (w->used >= sizeof(w->buf)) {
			w->overflow = true;
			return;
		}
		w->buf[w->used++] = 0;
	}
}

void dbus_w_pad(dbus_writer_t *w, size_t align) { w_pad(w, align ? align : 1); }

static void w_raw(dbus_writer_t *w, const void *data, size_t len) {
	if (w->used + len > sizeof(w->buf)) {
		w->overflow = true;
		return;
	}
	memcpy(w->buf + w->used, data, len);
	w->used += len;
}

void dbus_w_byte(dbus_writer_t *w, uint8_t v) { w_raw(w, &v, 1); }
void dbus_w_u16(dbus_writer_t *w, uint16_t v) {
	w_pad(w, 2);
	w_raw(w, &v, 2);
}
void dbus_w_u32(dbus_writer_t *w, uint32_t v) {
	w_pad(w, 4);
	w_raw(w, &v, 4);
}
void dbus_w_bool(dbus_writer_t *w, bool v) { dbus_w_u32(w, v ? 1u : 0u); }
void dbus_w_i64(dbus_writer_t *w, int64_t v) {
	w_pad(w, 8);
	w_raw(w, &v, 8);
}

static void w_string_body(dbus_writer_t *w, const char *v) {
	if (!v) {
		v = "";
	}
	size_t len = strlen(v);
	dbus_w_u32(w, (uint32_t)len);
	w_raw(w, v, len + 1);
}

void dbus_w_string(dbus_writer_t *w, const char *v) { w_string_body(w, v); }
void dbus_w_path(dbus_writer_t *w, const char *v) { w_string_body(w, v); }

static void w_signature(dbus_writer_t *w, const char *sig) {
	if (!sig) {
		sig = "";
	}
	size_t len = strlen(sig);
	dbus_w_byte(w, (uint8_t)len);
	w_raw(w, sig, len + 1);
}

static size_t align_of(const char *sig) {
	switch (sig ? sig[0] : 0) {
	case 'y':
	case 'g':
	case 'v':
		return 1;
	case 'n':
	case 'q':
		return 2;
	case 'x':
	case 't':
	case 'd':
	case '(':
	case '{':
		return 8;
	default:
		return 4;
	}
}

void dbus_w_array_begin(dbus_writer_t *w, const char *elem_sig, dbus_array_t *a) {
	dbus_w_u32(w, 0);
	a->len_at = w->used - 4;
	w_pad(w, align_of(elem_sig));
	a->body_at = w->used;
}

void dbus_w_array_end(dbus_writer_t *w, dbus_array_t *a) {
	if (w->overflow || a->len_at + 4 > w->used) {
		return;
	}
	uint32_t len = (uint32_t)(w->used - a->body_at);
	memcpy(w->buf + a->len_at, &len, 4);
}

static void w_dict_key(dbus_writer_t *w, const char *key, const char *value_sig) {
	w_pad(w, 8);
	dbus_w_string(w, key);
	w_signature(w, value_sig);
}

void dbus_w_dict_string(dbus_writer_t *w, const char *key, const char *value) {
	w_dict_key(w, key, "s");
	dbus_w_string(w, value);
}
void dbus_w_dict_path(dbus_writer_t *w, const char *key, const char *value) {
	w_dict_key(w, key, "o");
	dbus_w_path(w, value);
}
void dbus_w_dict_bool(dbus_writer_t *w, const char *key, bool value) {
	w_dict_key(w, key, "b");
	dbus_w_bool(w, value);
}
void dbus_w_dict_i64(dbus_writer_t *w, const char *key, int64_t value) {
	w_dict_key(w, key, "x");
	dbus_w_i64(w, value);
}
void dbus_w_dict_strings(dbus_writer_t *w, const char *key, const char *const *values, int count) {
	w_dict_key(w, key, "as");
	dbus_array_t a;
	dbus_w_array_begin(w, "s", &a);
	for (int i = 0; i < count; i++) {
		dbus_w_string(w, values[i]);
	}
	dbus_w_array_end(w, &a);
}
void dbus_w_dict_variant_begin(dbus_writer_t *w, const char *key, const char *value_sig) {
	w_dict_key(w, key, value_sig);
}
void dbus_w_variant_string(dbus_writer_t *w, const char *value) {
	w_signature(w, "s");
	dbus_w_string(w, value);
}
void dbus_w_variant_bool(dbus_writer_t *w, bool value) {
	w_signature(w, "b");
	dbus_w_bool(w, value);
}
void dbus_w_variant_u16(dbus_writer_t *w, uint16_t value) {
	w_signature(w, "q");
	dbus_w_u16(w, value);
}
void dbus_w_variant_u32(dbus_writer_t *w, uint32_t value) {
	w_signature(w, "u");
	dbus_w_u32(w, value);
}

// ---------------------------------------------------------------------------
// Reader, again the same rules as the production file.
// ---------------------------------------------------------------------------

static uint32_t swap32(uint32_t v) {
	return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) |
		   ((v & 0xff000000u) >> 24);
}

static bool host_is_big_endian(void) {
	const uint16_t one = 1;
	return ((const uint8_t *)&one)[0] == 0;
}

static void r_pad(dbus_reader_t *r, size_t align) {
	while (r->at % align) {
		if (r->at >= r->len) {
			r->bad = true;
			return;
		}
		r->at++;
	}
}

static uint32_t r_u32(dbus_reader_t *r) {
	r_pad(r, 4);
	if (r->bad || r->at + 4 > r->len) {
		r->bad = true;
		return 0;
	}
	uint32_t v;
	memcpy(&v, r->data + r->at, 4);
	r->at += 4;
	return r->big_endian == host_is_big_endian() ? v : swap32(v);
}

void dbus_reader_init(dbus_reader_t *r, const dbus_msg_t *m) {
	memset(r, 0, sizeof(*r));
	if (m) {
		r->data = m->body;
		r->len = m->body_len;
		r->big_endian = m->big_endian;
	}
}

bool dbus_r_string(dbus_reader_t *r, char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	uint32_t len = r_u32(r);
	if (r->bad || r->at + (size_t)len + 1 > r->len) {
		r->bad = true;
		return false;
	}
	if (out && out_size) {
		size_t take = len < out_size - 1 ? len : out_size - 1;
		memcpy(out, r->data + r->at, take);
		out[take] = '\0';
	}
	r->at += (size_t)len + 1;
	return true;
}

bool dbus_r_signature(dbus_reader_t *r, char *out, size_t out_size) {
	if (out && out_size) {
		out[0] = '\0';
	}
	if (r->bad || r->at >= r->len) {
		r->bad = true;
		return false;
	}
	size_t len = r->data[r->at++];
	if (r->at + len + 1 > r->len) {
		r->bad = true;
		return false;
	}
	if (out && out_size) {
		size_t take = len < out_size - 1 ? len : out_size - 1;
		memcpy(out, r->data + r->at, take);
		out[take] = '\0';
	}
	r->at += len + 1;
	return true;
}

bool dbus_r_byte(dbus_reader_t *r, uint8_t *out) {
	if (r->bad || r->at >= r->len) {
		r->bad = true;
		return false;
	}
	uint8_t v = r->data[r->at++];
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_u16(dbus_reader_t *r, uint16_t *out) {
	r_pad(r, 2);
	if (r->bad || r->at + 2 > r->len) {
		r->bad = true;
		return false;
	}
	uint16_t v;
	memcpy(&v, r->data + r->at, 2);
	r->at += 2;
	if (r->big_endian != host_is_big_endian()) {
		v = (uint16_t)((v >> 8) | (v << 8));
	}
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_u32(dbus_reader_t *r, uint32_t *out) {
	uint32_t v = r_u32(r);
	if (r->bad) {
		return false;
	}
	if (out) {
		*out = v;
	}
	return true;
}

bool dbus_r_bool(dbus_reader_t *r, bool *out) {
	uint32_t v = 0;
	if (!dbus_r_u32(r, &v)) {
		return false;
	}
	if (out) {
		*out = v != 0;
	}
	return true;
}

bool dbus_r_array_begin(dbus_reader_t *r, const char *elem_sig, dbus_array_iter_t *it) {
	uint32_t len = r_u32(r);
	it->align = align_of(elem_sig);
	it->end = 0;
	r_pad(r, it->align);
	if (r->bad || r->at + len > r->len) {
		r->bad = true;
		return false;
	}
	it->end = r->at + len;
	return true;
}

bool dbus_r_array_more(dbus_reader_t *r, dbus_array_iter_t *it) {
	if (r->bad || r->at >= it->end) {
		return false;
	}
	r_pad(r, it->align);
	return !r->bad && r->at < it->end;
}

static size_t sig_first_len(const char *sig) {
	size_t n = 0;
	if (!sig) {
		return 0;
	}
	while (sig[n] == 'a') {
		n++;
	}
	if (!sig[n]) {
		return n;
	}
	if (sig[n] == '(' || sig[n] == '{') {
		int depth = 0;
		for (; sig[n]; n++) {
			if (sig[n] == '(' || sig[n] == '{') {
				depth++;
			} else if (sig[n] == ')' || sig[n] == '}') {
				if (--depth == 0) {
					return n + 1;
				}
			}
		}
		return n;
	}
	return n + 1;
}

bool dbus_r_skip(dbus_reader_t *r, const char *sig) {
	if (r->bad) {
		return false;
	}
	if (!sig || !sig[0]) {
		return true;
	}
	switch (sig[0]) {
	case 'y':
		return dbus_r_byte(r, NULL);
	case 'n':
	case 'q':
		return dbus_r_u16(r, NULL);
	case 'b':
	case 'i':
	case 'u':
	case 'h':
		return dbus_r_u32(r, NULL);
	case 'x':
	case 't':
	case 'd':
		r_pad(r, 8);
		if (r->bad || r->at + 8 > r->len) {
			r->bad = true;
			return false;
		}
		r->at += 8;
		return true;
	case 's':
	case 'o':
		return dbus_r_string(r, NULL, 0);
	case 'g':
		return dbus_r_signature(r, NULL, 0);
	case 'v': {
		char inner[64];
		return dbus_r_signature(r, inner, sizeof(inner)) && dbus_r_skip(r, inner);
	}
	case 'a': {
		dbus_array_iter_t it;
		if (!dbus_r_array_begin(r, sig + 1, &it)) {
			return false;
		}
		while (dbus_r_array_more(r, &it)) {
			if (!dbus_r_skip(r, sig + 1)) {
				return false;
			}
		}
		return !r->bad;
	}
	case '(':
	case '{': {
		r_pad(r, 8);
		const char *at = sig + 1;
		const char *close = sig + sig_first_len(sig) - 1;
		while (at < close) {
			size_t n = sig_first_len(at);
			if (n == 0 || at + n > close) {
				r->bad = true;
				return false;
			}
			char one[64];
			size_t take = n < sizeof(one) - 1 ? n : sizeof(one) - 1;
			memcpy(one, at, take);
			one[take] = '\0';
			if (!dbus_r_skip(r, one)) {
				return false;
			}
			at += n;
		}
		return !r->bad;
	}
	default:
		r->bad = true;
		return false;
	}
}

// ---------------------------------------------------------------------------
// Connection stand-in
// ---------------------------------------------------------------------------

dbus_writer_t *dbus_reply_begin(dbus_conn_t *c, const dbus_msg_t *call, const char *signature) {
	(void)c;
	(void)call;
	snprintf(last.signature, sizeof(last.signature), "%s", signature ? signature : "");
	memset(&reply_writer, 0, sizeof(reply_writer));
	return &reply_writer;
}

bool dbus_reply_send(dbus_conn_t *c) {
	(void)c;
	last.len = reply_writer.used < sizeof(last.body) ? reply_writer.used : sizeof(last.body);
	memcpy(last.body, reply_writer.buf, last.len);
	last.is_error = false;
	return true;
}

bool dbus_error(dbus_conn_t *c, const dbus_msg_t *call, const char *name, const char *message) {
	(void)c;
	(void)call;
	last.is_error = true;
	snprintf(last.error, sizeof(last.error), "%s: %s", name ? name : "", message ? message : "");
	return true;
}
