#include "common.h"
#include "tg-pending.h"

static struct tg_pending *find(GSList *list, uint8_t transaction)
{
	GSList *l;

	for (l = list; l != NULL; l = l->next) {
		struct tg_pending *p = l->data;

		if (p->transaction == transaction)
			return p;
	}

	return NULL;
}

int main(void)
{
	GSList *list = NULL;
	struct tg_pending *p;

	list = g_slist_append(list, tg_pending_new(0x05, 1, 0x71));
	list = g_slist_append(list, tg_pending_new(0x06, 1, 0x72));
	list = g_slist_append(list, tg_pending_new(0x07, 2, 0x75));

	p = find(list, 0x06);
	CHECK(p != NULL && p->player_id == 1, "entry keeps player id");
	CHECK(p != NULL && p->pdu_id == 0x72, "entry keeps pdu id");
	CHECK(tg_pending_is_current(list, 0x06), "current transaction found");
	CHECK(!tg_pending_is_current(list, 0x09), "unknown transaction absent");
	CHECK(tg_pending_count(list) == 3, "three pending entries");
	CHECK(tg_pending_stale_count(list, 0x06) == 2, "two stale entries");

	tg_pending_drop_stale(&list, 0x06);
	CHECK(tg_pending_count(list) == 1, "one entry after drop_stale");
	CHECK(find(list, 0x06) != NULL, "current entry kept");
	CHECK(find(list, 0x05) == NULL, "stale 0x05 dropped");
	CHECK(find(list, 0x07) == NULL, "stale 0x07 dropped");

	tg_pending_drop_player(&list, 1);
	CHECK(list == NULL, "player's entries removed");
	CHECK(!tg_pending_is_current(list, 0x06), "empty list has no current");

	TEST_REPORT("tg-pending");
}
