#include "src/system/library/id3chap.h"

#include <stdio.h>
#include <string.h>

// An ID3v2.3 CHAP with a one-character element ID and a five-character title.
#define CHAPTER(id, start, end, title) "CHAP\0\0\0\x22\0\0" id "\0" start end "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFFTIT2\0\0\0\x06\0\0\0" title

static const char tag[] = "ID3\x03\0\0\0\0\x01\x04"		  // 132 bytes: three 44-byte frames
	CHAPTER("a", "\0\0\0\0", "\0\0\x03\xE8", "Intro")	  // 0 to 1 second
	CHAPTER("b", "\0\0\x03\xE8", "\0\0\x07\xD0", "Topic") // 1 to 2 seconds
	CHAPTER("c", "\0\0\x07\xD0", "\0\0\x0B\xB8", "Outro");

static bool write_fixture(const char *path, const char *mode, const char *bytes, size_t len) {
	FILE *fixture = fopen(path, mode);
	if (!fixture) {
		perror(path);
		return false;
	}
	size_t written = fwrite(bytes, 1, len, fixture);
	if (fclose(fixture) != 0 || written != len) {
		fprintf(stderr, "could not write fixture: %s\n", path);
		return false;
	}
	return true;
}

static bool expect_pending(const char *path, bool expected, const char *description) {
	if (id3chap_tag_pending(path) != expected) {
		fprintf(stderr, "%s: expected %s\n", description, expected ? "pending" : "not pending");
		return false;
	}
	return true;
}

static bool test_growing_tag(const char *path) {
	const size_t short_header = 9;
	const size_t first_chapter = 54; // header and first complete CHAP
	if (!expect_pending(path, false, "absent file") || !write_fixture(path, "wb", tag, short_header) || !expect_pending(path, true, "short header") || !write_fixture(path, "ab", tag + short_header, first_chapter - short_header) || !expect_pending(path, true, "tag with one readable chapter but more bytes declared")) {
		return false;
	}
	if (id3chap_read(path, NULL, 0) != 1) {
		fprintf(stderr, "expected the legacy reader to return the one readable chapter\n");
		return false;
	}
	return write_fixture(path, "ab", tag + first_chapter, sizeof(tag) - 1 - first_chapter) && expect_pending(path, false, "complete tag with no audio bytes");
}

static bool test_pending_header_validation(const char *path) {
	static const struct {
		const char *description;
		char header[11];
		bool pending;
	} cases[] = {
		{"non-ID3 data", "MP3\x03\0\0\0\0\0\1", false}, {"unsupported major version", "ID3\x02\0\0\0\0\0\1", false}, {"invalid syncsafe size", "ID3\x04\0\0\0\0\0\x81", false}, {"invalid revision", "ID3\x03\xFF\0\0\0\0\1", false}, {"reserved ID3v2.3 flag", "ID3\x03\0\x10\0\0\0\1", false}, {"reserved ID3v2.4 flag", "ID3\x04\0\x08\0\0\0\1", false}, {"over-budget tag (4 MiB plus one)", "ID3\x04\0\0\x02\0\0\1", false}, {"in-budget tag (exactly 4 MiB)", "ID3\x04\0\0\x02\0\0\0", true}, {"empty ID3v2.4 tag", "ID3\x04\0\0\0\0\0\0", false},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (!write_fixture(path, "wb", cases[i].header, 10) || !expect_pending(path, cases[i].pending, cases[i].description)) {
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s FIXTURE_PATH\n", argv[0]);
		return 1;
	}
	const char *path = argv[1];
	if (!test_growing_tag(path)) {
		return 1;
	}

	if (!id3chap_present(path)) {
		fprintf(stderr, "expected the file to have chapters\n");
		return 1;
	}
	int count = id3chap_read(path, NULL, 0);
	if (count != 3) {
		fprintf(stderr, "expected 3 chapters in count-only mode, got %d\n", count);
		return 1;
	}
	id3chap_t chapters[3] = {0};
	int read_count = id3chap_read(path, chapters, count);
	if (read_count != count) {
		fprintf(stderr, "expected to read %d chapters, got %d\n", count, read_count);
		return 1;
	}
	const id3chap_t expected[] = {{0.0, "Intro"}, {1.0, "Topic"}, {2.0, "Outro"}};
	for (int i = 0; i < count; i++) {
		if (chapters[i].start != expected[i].start || strcmp(chapters[i].title, expected[i].title) != 0) {
			fprintf(stderr, "expected chapter %d to be %s at %.0f seconds\n", i + 1, expected[i].title, expected[i].start);
			return 1;
		}
	}
	puts("id3chap: counted and read all 3 chapters");
	if (!test_pending_header_validation(path)) {
		return 1;
	}
	puts("id3chap: growing-tag checks passed");
	return 0;
}
