/*
 * TG browsing pending transaction tracking.
 *
 * Browsing replies are asynchronous: a GetFolderItems PDU is turned into one
 * or more MPRIS calls and the response is only sent once those calls have
 * completed.  This module keeps the small amount of state needed to decide
 * which reply belongs to the transaction the CT is currently waiting for.
 */

#ifndef BLUEZ_TG_PENDING_H
#define BLUEZ_TG_PENDING_H

#include <stdbool.h>
#include <stdint.h>

#include <glib.h>

struct tg_pending {
	uint8_t transaction;
	uint16_t player_id;
	uint8_t pdu_id;
	uint64_t stamp;
};

struct tg_pending *tg_pending_new(uint8_t transaction, uint16_t player_id,
							uint8_t pdu_id);
void tg_pending_free(void *data);

/* True when the list contains an entry for this transaction. */
bool tg_pending_is_current(GSList *pending, uint8_t transaction);

/* Remove every entry whose transaction differs from current. */
void tg_pending_drop_stale(GSList **pending, uint8_t current);

/* Remove every entry that belongs to player_id. */
void tg_pending_drop_player(GSList **pending, uint16_t player_id);

/* Number of entries in the list that do not match current. */
int tg_pending_stale_count(GSList *pending, uint8_t current);

/* Number of entries in the list. */
int tg_pending_count(GSList *pending);

#endif /* BLUEZ_TG_PENDING_H */
