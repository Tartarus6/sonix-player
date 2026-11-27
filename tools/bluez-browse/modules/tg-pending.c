/*
 * TG browsing pending transaction tracking.
 *
 * The CT numbers every browsing PDU with a one byte transaction identifier.
 * Only one reply may be outstanding for a given identifier; replies for older
 * transactions must be thrown away (otherwise a slow MPRIS answer can be
 * delivered against a request the CT has already abandoned).
 */

#include <stdlib.h>
#include <string.h>

#include "tg-pending.h"

struct tg_pending *tg_pending_new(uint8_t transaction, uint16_t player_id,
							uint8_t pdu_id)
{
	struct tg_pending *p;

	p = g_new0(struct tg_pending, 1);
	p->transaction = transaction;
	p->player_id = player_id;
	p->pdu_id = pdu_id;
	p->stamp = g_get_monotonic_time();

	return p;
}

void tg_pending_free(void *data)
{
	struct tg_pending *p = data;

	if (p == NULL)
		return;

	g_free(p);
}

bool tg_pending_is_current(GSList *pending, uint8_t transaction)
{
	GSList *l;

	for (l = pending; l != NULL; l = l->next) {
		struct tg_pending *p = l->data;

		if (p != NULL && p->transaction == transaction)
			return true;
	}

	return false;
}

void tg_pending_drop_stale(GSList **pending, uint8_t current)
{
	GSList *l;

	if (pending == NULL)
		return;

	for (l = *pending; l != NULL;) {
		GSList *next = l->next;
		struct tg_pending *p = l->data;

		if (p == NULL || p->transaction != current) {
			*pending = g_slist_remove_link(*pending, l);
			tg_pending_free(p);
			g_slist_free1(l);
		}

		l = next;
	}
}

void tg_pending_drop_player(GSList **pending, uint16_t player_id)
{
	GSList *l;

	if (pending == NULL)
		return;

	for (l = *pending; l != NULL;) {
		GSList *next = l->next;
		struct tg_pending *p = l->data;

		if (p != NULL && p->player_id == player_id) {
			*pending = g_slist_remove_link(*pending, l);
			tg_pending_free(p);
			g_slist_free1(l);
		}

		l = next;
	}
}

int tg_pending_stale_count(GSList *pending, uint8_t current)
{
	GSList *l;
	int n = 0;

	for (l = pending; l != NULL; l = l->next) {
		struct tg_pending *p = l->data;

		if (p != NULL && p->transaction != current)
			n++;
	}

	return n;
}

int tg_pending_count(GSList *pending)
{
	return g_slist_length(pending);
}
