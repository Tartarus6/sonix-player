#include <errno.h>
#include <string.h>

#include "common.h"
#include "avctp-defer.h"

int main(void)
{
	struct avctp_defer d;
	uint8_t payload[8] = { 0x71, 0x00, 0x05, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	uint8_t big[600];
	uint8_t frame[16];
	int n;

	avctp_defer_init(&d);
	CHECK(!avctp_defer_is_pending(&d), "fresh defer is not pending");
	CHECK(avctp_defer_save(&d, 0x42, payload, sizeof(payload)) == 0,
						"save succeeds");
	CHECK(avctp_defer_is_pending(&d), "saved defer is pending");
	CHECK(d.transaction == 0x42, "transaction stored");
	CHECK(d.len == sizeof(payload), "length stored");

	memset(big, 0, sizeof(big));
	CHECK(avctp_defer_save(&d, 0x01, big, sizeof(big)) == -EOVERFLOW,
						"oversized operands rejected");

	CHECK(avctp_defer_frame(&d, 0x00, NULL, sizeof(frame)) == -EINVAL,
						"null frame buffer rejected");
	CHECK(avctp_defer_frame(&d, 0x00, frame, 5) == -ENOSPC,
						"short frame buffer rejected");

	n = avctp_defer_frame(&d, 0x00, frame, sizeof(frame));
	CHECK(n == (int) (4 + sizeof(payload)), "frame length is header + data");
	CHECK(frame[0] == 0x42, "frame transaction byte");
	CHECK(frame[1] == 0x00, "frame cr byte");
	CHECK(frame[2] == 0x11 && frame[3] == 0x01, "frame AVCTP pid");
	CHECK(memcmp(frame + 4, payload, sizeof(payload)) == 0,
						"frame carries saved operands");

	avctp_defer_free(&d);
	CHECK(!avctp_defer_is_pending(&d), "free clears pending flag");

	TEST_REPORT("avctp-defer");
}
