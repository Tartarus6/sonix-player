#include "common.h"
#include "avrcp-tg-browse.h"

int main(void)
{
	CHECK(tg_change_count(0, 9, 100) == 10,
						"window smaller than total");
	CHECK(tg_change_count(95, 200, 100) == 5,
						"window clamped to true total");
	CHECK(tg_change_count(20, 10, 100) == 0,
						"inverted window rejected");

	TEST_REPORT("change-count");
}
