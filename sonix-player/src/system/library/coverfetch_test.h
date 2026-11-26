#ifndef COVERFETCH_TEST_H
#define COVERFETCH_TEST_H

#include <stdbool.h>
#include <stddef.h>

// Test-only hooks, compiled with COVERFETCH_TEST beside HOST_BUILD. No product
// build defines COVERFETCH_TEST.
//
// The stub controls are implemented in tools/test_coverfetch.c, beside the
// HTTP state they drive; the record counter and the save-folder override live
// in coverfetch.c under COVERFETCH_TEST, where the code they observe lives.

void coverfetch_test_reset(void);
void coverfetch_test_set_body(const char *body);
void coverfetch_test_set_status(int status);
int coverfetch_test_calls(void);
const char *coverfetch_test_url(void);

// Smallest gap, in ms, between two MusicBrainz search requests since reset;
// -1 when fewer than two were made. Used to check the 1 req/s pacing.
int coverfetch_test_min_request_gap_ms(void);

// A binary download body with an explicit length, for coverfetch_save().
void coverfetch_test_set_download(const void *data, size_t len);

// Send every save to this folder instead of deriving one from the track path.
// Implemented beside coverfetch_save() in coverfetch.c.
void coverfetch_test_set_dir(const char *dir);

// The temp path the last save chose, so a case can check that two saves to
// one folder do not share it.
const char *coverfetch_test_last_temp(void);

int coverfetch_test_outstanding_records(void);

// Make HTTP answers alternate: each search is OK with one release, then an
// empty "releases" array (a NOT_FOUND); every cover download is a valid JPEG.
// Used by the batch cancel test to get one found and one miss per pair.
void coverfetch_test_set_alternating(bool on);

#endif /* COVERFETCH_TEST_H */
