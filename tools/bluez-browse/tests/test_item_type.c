#include "common.h"
#include "avrcp-tg-browse.h"

int main(void)
{
	CHECK(tg_map_item_type(NULL, NULL, true) == TG_ITEM_FOLDER,
						"folder must map to 0x02");
	CHECK(tg_map_item_type("file:///mnt/sdcard/song.flac", NULL, false) ==
						TG_ITEM_SONG,
						"audio file must map to 0x03");
	CHECK(tg_map_item_type("file:///mnt/sdcard/cover.JPG", NULL, false) ==
						TG_ITEM_IMAGE,
						"image file must map to 0x04");

	TEST_REPORT("item-type");
}
