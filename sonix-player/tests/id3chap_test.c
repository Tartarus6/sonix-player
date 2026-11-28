#include "src/system/library/id3chap.h"

#include <stdio.h>
#include <string.h>

// An ID3v2.3 CHAP with a one-character element ID and a five-character title.
#define CHAPTER(id, start, end, title) "CHAP\0\0\0\x22\0\0" id "\0" start end "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFFTIT2\0\0\0\x06\0\0\0" title

static const char tag[] = "ID3\x03\0\0\0\0\x01\x04"		  // 132 bytes: three 44-byte frames
	CHAPTER("a", "\0\0\0\0", "\0\0\x03\xE8", "Intro")	  // 0 to 1 second
	CHAPTER("b", "\0\0\x03\xE8", "\0\0\x07\xD0", "Topic") // 1 to 2 seconds
	CHAPTER("c", "\0\0\x07\xD0", "\0\0\x0B\xB8", "Outro");

static bool write_fixture(const char *path) {
	FILE *fixture = fopen(path, "wb");
	if (!fixture) {
		perror(path);
		return false;
	}
	size_t written = fwrite(tag, 1, sizeof(tag) - 1, fixture);
	if (fclose(fixture) != 0 || written != sizeof(tag) - 1) {
		fprintf(stderr, "could not write fixture: %s\n", path);
		return false;
	}
	return true;
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s FIXTURE_PATH\n", argv[0]);
		return 1;
	}
	const char *path = argv[1];
	if (!write_fixture(path)) {
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
	return 0;
}
