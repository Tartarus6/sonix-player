/*
 * AVRCP 1.5 target (TG) browsing helpers.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avrcp-tg-browse.h"

static bool has_suffix(const char *s, const char *suffix)
{
	size_t a, b;

	if (s == NULL || suffix == NULL)
		return false;

	a = strlen(s);
	b = strlen(suffix);

	return a >= b && g_ascii_strcasecmp(s + a - b, suffix) == 0;
}

uint8_t tg_map_item_type(const char *uri, const char *mime, bool folder)
{
	if (folder)
		return TG_ITEM_FOLDER;

	if (mime != NULL) {
		if (strncmp(mime, "image/", 6) == 0)
			return TG_ITEM_IMAGE;
		if (strncmp(mime, "video/", 6) == 0)
			return TG_ITEM_VIDEO;
		if (strncmp(mime, "audio/", 6) == 0)
			return TG_ITEM_SONG;
	}

	if (uri != NULL) {
		if (has_suffix(uri, ".jpg") || has_suffix(uri, ".jpeg") ||
				has_suffix(uri, ".png") || has_suffix(uri, ".gif") ||
				has_suffix(uri, ".bmp"))
			return TG_ITEM_IMAGE;
		if (has_suffix(uri, ".mp4") || has_suffix(uri, ".mkv") ||
				has_suffix(uri, ".avi") || has_suffix(uri, ".m4v"))
			return TG_ITEM_VIDEO;
		if (has_suffix(uri, ".m4b") || has_suffix(uri, ".aac"))
			return TG_ITEM_AUDIO_BOOK;
		if (has_suffix(uri, ".mp3") || has_suffix(uri, ".wav") ||
				has_suffix(uri, ".flac") || has_suffix(uri, ".ogg") ||
				has_suffix(uri, ".opus") || has_suffix(uri, ".wma") ||
				has_suffix(uri, ".m4a") || has_suffix(uri, ".ape"))
			return TG_ITEM_SONG;
	}

	return TG_ITEM_SONG;
}

const char *tg_display_name(GHashTable *metadata, const char *fallback,
						char *out, size_t len)
{
	static const char *keys[] = { "xesam:title", "title", "xesam:url",
						"mpris:trackid", NULL };
	const char *value = NULL;
	const char *base;
	int i;

	if (out == NULL || len == 0)
		return fallback;

	for (i = 0; keys[i] != NULL; i++) {
		const char *v = g_hash_table_lookup(metadata, keys[i]);

		if (v != NULL && *v != '\0') {
			value = v;
			break;
		}
	}

	if (value == NULL)
		value = fallback != NULL ? fallback : "";

	/* MPRIS URLs are file:// URIs; show only the last path component. */
	base = strrchr(value, '/');
	if (base != NULL && base[1] != '\0')
		value = base + 1;

	g_strlcpy(out, value, len);

	return out;
}

uint32_t tg_change_count(uint32_t start, uint32_t end, uint32_t total)
{
	uint32_t wanted;

	if (end < start || total <= start)
		return 0;

	wanted = end - start + 1;
	if (wanted > total - start)
		wanted = total - start;

	return wanted;
}

bool tg_check_scope(uint8_t scope)
{
	return scope == TG_SCOPE_VFS || scope == TG_SCOPE_NOW_PLAYING;
}

void tg_put_be16(uint8_t *buf, uint16_t v)
{
	buf[0] = v >> 8;
	buf[1] = v & 0xff;
}

void tg_put_be32(uint8_t *buf, uint32_t v)
{
	buf[0] = v >> 24;
	buf[1] = (v >> 16) & 0xff;
	buf[2] = (v >> 8) & 0xff;
	buf[3] = v & 0xff;
}

uint16_t tg_get_be16(const uint8_t *buf)
{
	return (uint16_t) ((buf[0] << 8) | buf[1]);
}

uint32_t tg_get_be32(const uint8_t *buf)
{
	return ((uint32_t) buf[0] << 24) | ((uint32_t) buf[1] << 16) |
			((uint32_t) buf[2] << 8) | (uint32_t) buf[3];
}

size_t sim_encode_pdu(uint8_t *buf, size_t cap, uint8_t pdu_id,
					const uint8_t *params, size_t len)
{
	if (buf == NULL || cap < TG_BROWSING_HEADER_LEN)
		return 0;

	if (len > cap - TG_BROWSING_HEADER_LEN)
		return 0;

	buf[0] = pdu_id;
	tg_put_be16(buf + 1, (uint16_t) len);
	if (len != 0)
		memcpy(buf + TG_BROWSING_HEADER_LEN, params, len);

	return TG_BROWSING_HEADER_LEN + len;
}

bool sim_check_pdu(const uint8_t *buf, size_t len, uint8_t pdu_id,
					size_t params_len)
{
	if (buf == NULL || len < TG_BROWSING_HEADER_LEN)
		return false;

	return buf[0] == pdu_id && tg_get_be16(buf + 1) == params_len &&
			len == TG_BROWSING_HEADER_LEN + params_len;
}

size_t sim_encode_folder_item(uint8_t *buf, size_t cap, uint8_t type,
					uint64_t uid, const char *name)
{
	size_t namelen, total;

	if (buf == NULL || cap < 3)
		return 0;

	namelen = name != NULL ? strlen(name) : 0;
	if (namelen > TG_MAX_ITEM_NAME_LEN)
		namelen = TG_MAX_ITEM_NAME_LEN;
	total = 3 + 8 + namelen;
	if (total > cap)
		return 0;

	buf[0] = type;
	tg_put_be16(buf + 1, (uint16_t) (8 + namelen));
	tg_put_be32(buf + 3, (uint32_t) (uid >> 32));
	tg_put_be32(buf + 7, (uint32_t) uid);
	if (namelen != 0)
		memcpy(buf + 11, name, namelen);

	return total;
}

bool sim_check_folder_item(const uint8_t *buf, size_t len, uint8_t type,
					uint64_t uid)
{
	uint64_t got;

	if (buf == NULL || len < 11 || buf[0] != type)
		return false;

	if (tg_get_be16(buf + 1) != len - 3)
		return false;

	got = ((uint64_t) tg_get_be32(buf + 3) << 32) | tg_get_be32(buf + 7);

	return got == uid;
}

size_t tg_pdu_status(uint8_t *buf, size_t cap, uint8_t pdu_id, uint8_t status)
{
	return sim_encode_pdu(buf, cap, pdu_id, &status, 1);
}

size_t tg_pdu_folder_items(uint8_t *buf, size_t cap, uint16_t uid_counter,
					const uint8_t *items, size_t items_len)
{
	uint8_t params[8 + TG_MAX_BROWSE_PAYLOAD];
	size_t used = 5, pos = 0, count = 0;

	if (buf == NULL || cap < TG_BROWSING_HEADER_LEN)
		return 0;

	memset(params, 0, sizeof(params));
	params[0] = TG_STATUS_SUCCESS;
	tg_put_be16(params + 1, uid_counter);

	while (pos + 3 <= items_len) {
		size_t item_len = 3 + tg_get_be16(items + pos + 1);

		if (item_len > items_len - pos ||
				used + item_len > TG_MAX_BROWSE_PAYLOAD + 5)
			break;

		memcpy(params + used, items + pos, item_len);
		used += item_len;
		pos += item_len;
		count++;
	}

	tg_put_be16(params + 3, (uint16_t) count);

	return sim_encode_pdu(buf, cap, TG_PDU_GET_FOLDER_ITEMS, params, used);
}

size_t tg_pdu_total_items(uint8_t *buf, size_t cap, uint16_t uid_counter,
					uint32_t total)
{
	uint8_t params[7];

	params[0] = TG_STATUS_SUCCESS;
	tg_put_be16(params + 1, uid_counter);
	tg_put_be32(params + 3, total);

	return sim_encode_pdu(buf, cap, TG_PDU_GET_TOTAL_ITEMS,
							params, sizeof(params));
}

size_t tg_pdu_change_path(uint8_t *buf, size_t cap, uint32_t count)
{
	uint8_t params[5];

	params[0] = TG_STATUS_SUCCESS;
	tg_put_be32(params + 1, count);

	return sim_encode_pdu(buf, cap, TG_PDU_CHANGE_PATH,
							params, sizeof(params));
}

size_t tg_pdu_item_attributes(uint8_t *buf, size_t cap, uint8_t count,
					const uint8_t *attrs, size_t attrs_len)
{
	uint8_t params[2 + TG_MAX_BROWSE_PAYLOAD];
	size_t used;

	if (attrs_len > TG_MAX_BROWSE_PAYLOAD)
		attrs_len = TG_MAX_BROWSE_PAYLOAD;

	params[0] = TG_STATUS_SUCCESS;
	params[1] = count;
	used = 2;
	if (attrs_len != 0) {
		memcpy(params + used, attrs, attrs_len);
		used += attrs_len;
	}

	return sim_encode_pdu(buf, cap, TG_PDU_GET_ITEM_ATTRIBUTES,
								params, used);
}

size_t tg_pdu_play_item(uint8_t *buf, size_t cap)
{
	return tg_pdu_status(buf, cap, TG_PDU_PLAY_ITEM, TG_STATUS_SUCCESS);
}

size_t tg_pdu_set_browsed_player(uint8_t *buf, size_t cap, uint16_t uid_counter,
					uint16_t depth, uint32_t items)
{
	uint8_t params[12];

	params[0] = TG_STATUS_SUCCESS;
	tg_put_be16(params + 1, uid_counter);
	tg_put_be32(params + 3, items);
	tg_put_be16(params + 7, depth);
	params[9] = 0x02;		/* Folder type: mixed */
	tg_put_be16(params + 10, 0);	/* No name */

	return sim_encode_pdu(buf, cap, TG_PDU_SET_BROWSED_PLAYER,
							params, sizeof(params));
}
