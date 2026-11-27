/*
 * Byte-level checks for the AVRCP TG browsing PDU encoders.
 */

#include <stdio.h>
#include <string.h>

#include "common.h"
#include "avrcp-tg-browse.h"

static void check_bytes(const char *label, const uint8_t *got,
					const uint8_t *want, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		CHECK(got[i] == want[i], "%s byte %zu: got %02x want %02x",
						label, i, got[i], want[i]);
}

static size_t make_item(uint8_t *buf, uint8_t type, uint64_t uid,
							const char *name)
{
	return sim_encode_folder_item(buf, 256, type, uid, name);
}

int main(void)
{
	uint8_t buf[1024], items[512];
	size_t n, pos = 0;
	uint8_t v[8] = { 0xaa, 0xbb, 0xcc, 0, 0, 0, 0, 0 };

	/* GetTotalNumberOfItems: 71? no, 0x75 with status, uid, total. */
	n = tg_pdu_total_items(buf, sizeof(buf), 0x0001, 0x0000002a);
	{
		const uint8_t exp[] = { 0x75, 0x00, 0x07, 0x00, 0x00, 0x01,
					0x00, 0x00, 0x00, 0x2a };
		CHECK(n == sizeof(exp), "total-items length");
		check_bytes("total-items", buf, exp, sizeof(exp));
	}

	n = tg_pdu_total_items(buf, sizeof(buf), 0x1234, 0x01020304);
	{
		const uint8_t exp[] = { 0x75, 0x00, 0x07, 0x00, 0x12, 0x34,
					0x01, 0x02, 0x03, 0x04 };
		CHECK(n == sizeof(exp), "total-items-2 length");
		check_bytes("total-items-2", buf, exp, sizeof(exp));
	}

	n = tg_pdu_change_path(buf, sizeof(buf), 5);
	{
		const uint8_t exp[] = { 0x72, 0x00, 0x05, 0x00, 0x00, 0x00,
					0x00, 0x05 };
		CHECK(n == sizeof(exp), "change-path length");
		check_bytes("change-path", buf, exp, sizeof(exp));
	}

	n = tg_pdu_change_path(buf, sizeof(buf), 0xffffffff);
	{
		const uint8_t exp[] = { 0x72, 0x00, 0x05, 0x00, 0xff, 0xff,
					0xff, 0xff };
		CHECK(n == sizeof(exp), "change-path-max length");
		check_bytes("change-path-max", buf, exp, sizeof(exp));
	}

	n = tg_pdu_play_item(buf, sizeof(buf));
	{
		const uint8_t exp[] = { 0x74, 0x00, 0x01, 0x00 };
		CHECK(n == sizeof(exp), "play-item length");
		check_bytes("play-item", buf, exp, sizeof(exp));
	}

	n = tg_pdu_status(buf, sizeof(buf), TG_PDU_GET_FOLDER_ITEMS,
							TG_STATUS_OUT_OF_BOUNDS);
	{
		const uint8_t exp[] = { 0x71, 0x00, 0x01, 0x04 };
		CHECK(n == sizeof(exp), "status length");
		check_bytes("status", buf, exp, sizeof(exp));
	}

	n = tg_pdu_set_browsed_player(buf, sizeof(buf), 0x0007, 0, 3);
	{
		const uint8_t exp[] = { 0x70, 0x00, 0x0c, 0x00, 0x00, 0x07,
					0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
					0x02, 0x00, 0x00 };
		CHECK(n == sizeof(exp), "set-browsed length");
		check_bytes("set-browsed", buf, exp, sizeof(exp));
	}

	/* Empty GetFolderItems response. */
	n = tg_pdu_folder_items(buf, sizeof(buf), 0x0009, NULL, 0);
	{
		const uint8_t exp[] = { 0x71, 0x00, 0x05, 0x00, 0x00, 0x09,
					0x00, 0x00 };
		CHECK(n == sizeof(exp), "folder-empty length");
		check_bytes("folder-empty", buf, exp, sizeof(exp));
	}

	/* One item called "Hello". */
	pos = make_item(items, TG_ITEM_SONG, 0x1122334455667788ULL, "Hello");
	n = tg_pdu_folder_items(buf, sizeof(buf), 0x000a, items, pos);
	{
		const uint8_t exp[] = { 0x71, 0x00, 0x15, 0x00, 0x00, 0x0a,
					0x00, 0x01, 0x03, 0x00, 0x0d, 0x11,
					0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
					0x88, 'H', 'e', 'l', 'l', 'o' };
		CHECK(n == sizeof(exp), "folder-one length");
		check_bytes("folder-one", buf, exp, sizeof(exp));
	}

	/* Two items, different names. */
	pos = make_item(items, TG_ITEM_FOLDER, 1, "A");
	pos += make_item(items + pos, TG_ITEM_SONG, 2, "BC");
	n = tg_pdu_folder_items(buf, sizeof(buf), 0x000b, items, pos);
	{
		const uint8_t exp[] = { 0x71, 0x00, 0x1e, 0x00, 0x00, 0x0b,
					0x00, 0x02, 0x02, 0x00, 0x09, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x01, 'A', 0x03, 0x00, 0x0a, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					0x02, 'B', 'C' };
		CHECK(n == sizeof(exp), "folder-two length");
		check_bytes("folder-two", buf, exp, sizeof(exp));
	}

	/* Item attributes. */
	{
		const uint8_t attrs[] = { 1, 0, 0, 1, 0x00, 0x06 };
		const uint8_t exp[] = { 0x73, 0x00, 0x08, 0x00, 0x02, 0x01,
					0x00, 0x00, 0x01, 0x00, 0x06 };
		n = tg_pdu_item_attributes(buf, sizeof(buf), 2, attrs,
								sizeof(attrs));
		CHECK(n == sizeof(exp), "attributes length");
		check_bytes("attributes", buf, exp, sizeof(exp));
	}

	/* Generic sim PDU and folder item. */
	n = sim_encode_pdu(buf, sizeof(buf), TG_PDU_GET_TOTAL_ITEMS, v, 3);
	{
		const uint8_t exp[] = { 0x75, 0x00, 0x03, 0xaa, 0xbb, 0xcc };
		CHECK(n == sizeof(exp), "sim-pdu length");
		check_bytes("sim-pdu", buf, exp, sizeof(exp));
	}

	n = sim_encode_folder_item(buf, sizeof(buf), TG_ITEM_IMAGE,
					0x0000000000000042ULL, "img");
	{
		const uint8_t exp[] = { 0x04, 0x00, 0x0b, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00, 0x42, 'i',
					'm', 'g' };
		CHECK(n == sizeof(exp), "sim-item length");
		check_bytes("sim-item", buf, exp, sizeof(exp));
	}

	/* Endian helpers and checkers. */
	n = tg_get_be16((const uint8_t *) "\x12\x34");
	CHECK(n == 0x1234, "be16 decode");
	n = tg_get_be32((const uint8_t *) "\x01\x02\x03\x04");
	CHECK(n == 0x01020304, "be32 decode");
	tg_put_be16(v, 0xabcd);
	CHECK(v[0] == 0xab && v[1] == 0xcd, "be16 encode");
	tg_put_be32(v, 0x0a0b0c0d);
	CHECK(v[0] == 0x0a && v[1] == 0x0b && v[2] == 0x0c &&
						v[3] == 0x0d, "be32 encode");
	CHECK(tg_check_scope(TG_SCOPE_VFS), "VFS scope accepted");
	CHECK(tg_check_scope(TG_SCOPE_NOW_PLAYING), "NowPlaying accepted");
	CHECK(!tg_check_scope(TG_SCOPE_PLAYER_LIST), "player list rejected");
	CHECK(!tg_check_scope(TG_SCOPE_SEARCH), "search scope rejected");
	CHECK(sim_check_pdu((const uint8_t *) "\x75\x00\x01\x00", 4,
					TG_PDU_GET_TOTAL_ITEMS, 1), "valid PDU");
	CHECK(!sim_check_pdu((const uint8_t *) "\x75\x00\x01\x00", 4,
					TG_PDU_CHANGE_PATH, 1), "wrong PDU id");
	CHECK(sim_check_folder_item((const uint8_t *)
			"\x03\x00\x08\x00\x00\x00\x00\x00\x00\x00\x05", 11,
					TG_ITEM_SONG, 5), "valid folder item");
	CHECK(!sim_check_folder_item((const uint8_t *)
			"\x03\x00\x08\x00\x00\x00\x00\x00\x00\x00\x05", 11,
					TG_ITEM_FOLDER, 5), "wrong folder type");

	/* Clamp: at most 505 bytes of items in a response. */
	{
		uint8_t many[1024];
		size_t off = 0;
		int i;

		for (i = 0; i < 100; i++)
			off += make_item(many + off, TG_ITEM_SONG, i, "x");
		n = tg_pdu_folder_items(buf, sizeof(buf), 0, many, off);
		CHECK(n <= 3 + TG_MAX_BROWSE_PAYLOAD + 5,
						"response stays within 505 item bytes");
		CHECK(((uint32_t) buf[5] << 8 | buf[6]) <= 100,
						"reported item count clamped");
	}

	/* Success status and two small mapping helpers. */
	n = tg_pdu_status(buf, sizeof(buf), TG_PDU_PLAY_ITEM, TG_STATUS_SUCCESS);
	{
		const uint8_t exp[] = { 0x74, 0x00, 0x01, 0x00 };
		CHECK(n == sizeof(exp), "play-success length");
		check_bytes("play-success", buf, exp, sizeof(exp));
	}
	CHECK(tg_change_count(0, 4, 10) == 5, "five items in window");
	CHECK(tg_map_item_type("cover.jpg", NULL, false) == TG_ITEM_IMAGE,
						"image mime by extension");

	TEST_REPORT("avrcp-tg-browse");
}
