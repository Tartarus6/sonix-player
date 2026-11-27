/* Shared assertion counters for the host harnesses. */

#ifndef BLUEZ_BROWSE_TEST_COMMON_H
#define BLUEZ_BROWSE_TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>

static int check_count;
static int check_failures;

#define CHECK(cond, ...)						\
	do {								\
		check_count++;						\
		if (!(cond)) {						\
			check_failures++;				\
			fprintf(stderr, "FAIL %s:%d: ", __FILE__,	\
							__LINE__);	\
			fprintf(stderr, __VA_ARGS__);			\
			fputc('\n', stderr);				\
		}							\
	} while (0)

#define CHECK_EQ(got, want, fmt, ...)					\
	CHECK((got) == (want), fmt " (got " fmt ", want " fmt ")",	\
			__VA_ARGS__, (got), (want))

#define TEST_REPORT(name)						\
	do {								\
		if (check_failures != 0) {				\
			fprintf(stderr, "FAIL %s: %d/%d checks failed\n",\
				name, check_failures, check_count);	\
			return 1;					\
		}							\
		printf("PASS %s: %d checks\n", name, check_count);	\
		return 0;						\
	} while (0)

#endif /* BLUEZ_BROWSE_TEST_COMMON_H */
