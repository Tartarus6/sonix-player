/*
 * AVRCP 1.5 target (TG) browsing helpers.
 *
 * The CT side of BlueZ 5.54 knows how to *ask* for folder items.  This module
 * implements the pieces needed to *answer*: item type mapping, display name
 * selection, the 505 byte response clamp, and byte-level encoders/checkers for
 * the browsing PDUs.  All routines are pure (no GLib main loop, no D-Bus) so
 * they can be exercised byte-for-byte by the host harnesses.
 */

#ifndef BLUEZ_AVRCP_TG_BROWSE_H
#define BLUEZ_AVRCP_TG_BROWSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>

/* AVRCP browsing PDU identifiers used by the target. */
#define TG_PDU_SET_BROWSED_PLAYER	0x70
#define TG_PDU_GET_FOLDER_ITEMS		0x71
#define TG_PDU_CHANGE_PATH		0x72
#define TG_PDU_GET_ITEM_ATTRIBUTES	0x73
#define TG_PDU_PLAY_ITEM		0x74
#define TG_PDU_GET_TOTAL_ITEMS		0x75

/* Scopes. */
#define TG_SCOPE_PLAYER_LIST		0x00
#define TG_SCOPE_VFS			0x01
#define TG_SCOPE_SEARCH			0x02
#define TG_SCOPE_NOW_PLAYING		0x03

/* Item types (AVRCP 1.5 table 5.10). */
#define TG_ITEM_MEDIA_PLAYER		0x01
#define TG_ITEM_FOLDER			0x02
#define TG_ITEM_SONG			0x03
#define TG_ITEM_IMAGE			0x04
#define TG_ITEM_VIDEO			0x05
#define TG_ITEM_AUDIO_BOOK		0x06

/* Item handed from the media layer to the AVRCP target. */
struct tg_browse_item {
	uint64_t uid;
	uint8_t type;
	char *name;
};

/* AVRCP status codes. */
#define TG_STATUS_SUCCESS		0x00
#define TG_STATUS_INVALID_PARAM		0x03
#define TG_STATUS_OUT_OF_BOUNDS		0x04
#define TG_STATUS_INTERNAL_ERROR	0x11
#define TG_STATUS_PLAYER_NOT_BROWSABLE	0x12
#define TG_STATUS_NO_FOLDER_ITEMS	0x13

#define TG_BROWSING_HEADER_LEN		3
#define TG_MAX_ITEM_NAME_LEN		255
#define TG_MAX_BROWSE_PAYLOAD		505

/* Map a MPRIS URI / mime type to an AVRCP folder item type. */
uint8_t tg_map_item_type(const char *uri, const char *mime, bool folder);

/* Pick the best human readable name out of a MPRIS metadata dictionary. */
const char *tg_display_name(GHashTable *metadata, const char *fallback,
						char *out, size_t len);

/* Number of items in [start,end], clamped to the folder's true total. */
uint32_t tg_change_count(uint32_t start, uint32_t end, uint32_t total);

/* True when the scope is one the target can serve. */
bool tg_check_scope(uint8_t scope);

/* Big endian helpers used by the encoders. */
void tg_put_be16(uint8_t *buf, uint16_t v);
void tg_put_be32(uint8_t *buf, uint32_t v);
uint16_t tg_get_be16(const uint8_t *buf);
uint32_t tg_get_be32(const uint8_t *buf);

/* Generic browsing PDU encoding/checking (used by the simulators). */
size_t sim_encode_pdu(uint8_t *buf, size_t cap, uint8_t pdu_id,
					const uint8_t *params, size_t len);
bool sim_check_pdu(const uint8_t *buf, size_t len, uint8_t pdu_id,
					size_t params_len);
size_t sim_encode_folder_item(uint8_t *buf, size_t cap, uint8_t type,
					uint64_t uid, const char *name);
bool sim_check_folder_item(const uint8_t *buf, size_t len, uint8_t type,
					uint64_t uid);

/* Target response encoders.  All return bytes written or 0 on overflow. */
size_t tg_pdu_status(uint8_t *buf, size_t cap, uint8_t pdu_id, uint8_t status);
size_t tg_pdu_folder_items(uint8_t *buf, size_t cap, uint16_t uid_counter,
					const uint8_t *items, size_t items_len);
size_t tg_pdu_total_items(uint8_t *buf, size_t cap, uint16_t uid_counter,
					uint32_t total);
size_t tg_pdu_change_path(uint8_t *buf, size_t cap, uint32_t count);
size_t tg_pdu_item_attributes(uint8_t *buf, size_t cap, uint8_t count,
					const uint8_t *attrs, size_t attrs_len);
size_t tg_pdu_play_item(uint8_t *buf, size_t cap);

/* SetBrowsedPlayer response carries the folder depth as its first parameter. */
size_t tg_pdu_set_browsed_player(uint8_t *buf, size_t cap, uint16_t uid_counter,
					uint16_t depth, uint32_t items);

#endif /* BLUEZ_AVRCP_TG_BROWSE_H */
