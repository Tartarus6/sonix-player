/*
 * Deferred AVCTP browsing replies.
 *
 * The stock avctp.c writes the response in the same callback that handles the
 * incoming PDU.  TG browsing needs to answer after the MPRIS calls have
 * completed, so the frame is saved here and sent later with
 * avctp_send_browsing_rsp().  A handler returning 0 tells avctp.c that the
 * reply was deferred and that no immediate write should be attempted.
 */

#ifndef BLUEZ_AVCTP_DEFER_H
#define BLUEZ_AVCTP_DEFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AVCTP_DEFER_MAX_OPERANDS 512

struct avctp_defer {
	bool pending;
	uint8_t transaction;
	uint8_t *operands;
	size_t len;
	uint64_t stamp;
};

void avctp_defer_init(struct avctp_defer *d);

/* Save a copy of the operands for a later reply.  Returns 0 or -EINVAL/
 * -ENOMEM/-EOVERFLOW. */
int avctp_defer_save(struct avctp_defer *d, uint8_t transaction,
					const uint8_t *operands, size_t len);

/* Build the complete AVCTP frame (header + saved operands) into buf.
 * Returns the frame length or -EAGAIN when nothing is pending, -ENOSPC when
 * buf is too small. */
int avctp_defer_frame(struct avctp_defer *d, uint8_t cr,
					uint8_t *buf, size_t cap);

/* Drop the saved frame.  The structure can be reused afterwards. */
void avctp_defer_free(struct avctp_defer *d);

bool avctp_defer_is_pending(const struct avctp_defer *d);

#endif /* BLUEZ_AVCTP_DEFER_H */
