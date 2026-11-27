/*
 * Deferred AVCTP browsing replies.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "avctp-defer.h"

#ifndef AV_REMOTE_SVCLASS_ID
#define AV_REMOTE_SVCLASS_ID 0x1101
#endif

static void put_be16(uint8_t *buf, uint16_t v)
{
	buf[0] = v >> 8;
	buf[1] = v & 0xff;
}

void avctp_defer_init(struct avctp_defer *d)
{
	if (d == NULL)
		return;

	memset(d, 0, sizeof(*d));
}

int avctp_defer_save(struct avctp_defer *d, uint8_t transaction,
					const uint8_t *operands, size_t len)
{
	if (d == NULL || (operands == NULL && len != 0))
		return -EINVAL;

	if (len > AVCTP_DEFER_MAX_OPERANDS)
		return -EOVERFLOW;

	avctp_defer_free(d);

	if (len != 0) {
		d->operands = g_malloc(len);
		memcpy(d->operands, operands, len);
	}

	d->len = len;
	d->transaction = transaction;
	d->stamp = g_get_monotonic_time();
	d->pending = true;

	return 0;
}

int avctp_defer_frame(struct avctp_defer *d, uint8_t cr,
					uint8_t *buf, size_t cap)
{
	size_t needed;

	if (d == NULL || buf == NULL)
		return -EINVAL;

	if (!d->pending)
		return -EAGAIN;

	needed = 4 + d->len;
	if (cap < needed)
		return -ENOSPC;

	buf[0] = d->transaction;
	buf[1] = cr;
	put_be16(buf + 2, AV_REMOTE_SVCLASS_ID);

	if (d->len != 0)
		memcpy(buf + 4, d->operands, d->len);

	return (int) needed;
}

void avctp_defer_free(struct avctp_defer *d)
{
	if (d == NULL)
		return;

	g_free(d->operands);
	d->operands = NULL;
	d->len = 0;
	d->pending = false;
}

bool avctp_defer_is_pending(const struct avctp_defer *d)
{
	return d != NULL && d->pending;
}
