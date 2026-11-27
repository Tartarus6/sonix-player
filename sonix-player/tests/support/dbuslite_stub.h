#ifndef DBUSLITE_STUB_H
#define DBUSLITE_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/bluetooth/dbuslite.h"

// The host harness needs the wire format dbuslite writes, but not a bus. These
// helpers keep the production writer/reader byte-for-byte and only replace the
// connection with a place to put the last reply.
typedef struct {
	char signature[64];
	uint8_t body[16 * 1024];
	size_t len;
	bool is_error;
	char error[256];
} dbus_test_result_t;

const dbus_test_result_t *dbus_test_result(void);
void dbus_test_reset(void);

// A scratch writer for building incoming method bodies (filters, paths).
void dbus_test_scratch_reset(void);
dbus_writer_t *dbus_test_scratch(void);
size_t dbus_test_scratch_copy(uint8_t *dst, size_t size);

#endif /* DBUSLITE_STUB_H */
