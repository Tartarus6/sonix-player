// Test the public RSS API with the network boundary supplied by a local fixture.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/lang.h"
#include "src/system/net/http.h"
#include "src/system/streaming/podcastrss.h"

static const char *url = "https://example.test/podcast.xml";
static const char *fixture;
static const char *response;
static bool long_note;

bool http_get_ex(const char *address, const char *headers, bool error_body, char **out, size_t *len, size_t limit, int timeout, int *status) {
	(void)headers;
	(void)error_body;
	(void)timeout;
	assert(strcmp(url, address) == 0);
	if (response) {
		*out = strdup(response);
		assert(*out);
		*len = strlen(response);
		return true;
	}
	FILE *f = long_note ? tmpfile() : fixture ? fopen(fixture, "rb") : NULL;
	if (!f) {
		return false;
	}
	if (long_note) {
		fputs("<rss><channel><title>Fixture podcast</title><item><guid>second &amp; latest</guid>"
			  "<enclosure url='https://example.test/second.mp3'/><description><![CDATA[",
			  f);
		for (int i = 0; i < 12000; i++) {
			fputs("€", f);
		}
		fputs("END MARKER]]></description></item></channel></rss>", f);
	}
	assert(fseek(f, 0, SEEK_END) == 0);
	long bytes = ftell(f);
	assert(bytes >= 0 && (size_t)bytes <= limit);
	rewind(f);
	*out = malloc((size_t)bytes + 1);
	assert(*out);
	*len = fread(*out, 1, (size_t)bytes, f);
	assert(*len == (size_t)bytes);
	(*out)[*len] = '\0';
	if (status) {
		*status = 200;
	}
	fclose(f);
	return true;
}

const char *tr(const char *key) { return key; }
const char *http_last_error(void) { return ""; }
void podcast_set_last_error(const char *message) { (void)message; }

static long long by_title(const podcast_episode_t *episodes, int count, const char *title) {
	for (int i = 0; i < count; i++) {
		if (strcmp(episodes[i].title, title) == 0) {
			return episodes[i].id;
		}
	}
	abort();
}

static void expect_note(long long feed, long long episode, const char *expected) {
	char *text = NULL;
	assert(podcastrss_episode_notes(feed, episode, &text) == PODCAST_NOTES_OK);
	if (strcmp(text, expected) != 0) {
		fprintf(stderr, "Expected:\n%s\nGot:\n%s\n", expected, text);
		abort();
	}
	free(text);
}

int main(void) {
	fixture = "tests/fixtures/show-notes.xml";
	podcast_feed_t feed;
	podcast_episode_t episodes[4];
	assert(podcastrss_read_feed(url, &feed));
	assert(podcastrss_episodes(feed.id, episodes, 4, NULL) == 4);
	long long second = by_title(episodes, 4, "Second episode");
	expect_note(feed.id, second, "Hello listeners.\n\nIn this episode:\n\n• News\n• Guest interview");
	expect_note(feed.id, by_title(episodes, 4, "First episode"), "Literal <em>, A & B.\n01:23 https://example.test");
	expect_note(feed.id, by_title(episodes, 4, "Summary only"), "Keep <tag> & words.\nNext line.");
	char *text = NULL;
	assert(podcastrss_episode_notes(feed.id, by_title(episodes, 4, "No notes"), &text) == PODCAST_NOTES_EMPTY && !text);
	assert(podcastrss_episode_notes(feed.id, PODCASTRSS_ID_BASE, &text) == PODCAST_NOTES_UNAVAILABLE && !text);
	assert(podcastrss_episode_notes(feed.id, 12, &text) == PODCAST_NOTES_UNAVAILABLE && !text);
	response = "<rss><channel>";
	assert(podcastrss_episode_notes(feed.id, second, &text) == PODCAST_NOTES_ERROR && !text);
	response = "<rss><channel><item><guid>second &amp; latest</guid><description>Still here.</description></item>";
	expect_note(feed.id, second, "Still here.");
	response = "<rss><channel><item><guid>second &amp; latest</guid>"
			   "<enclosure url='https://example.test/second.mp3'/><description>Why 1 &lt;3 matters</description></item></channel></rss>";
	expect_note(feed.id, second, "Why 1 <3 matters");
	response = "<rss><channel><item><guid>second &amp; latest</guid><description><![CDATA["
			   "<h4>Links and Show Notes:</h4><h5><a href='https://example.test/first'>First link</a></h5> "
			   "<h5><a href='https://example.test/second'>Second link</a></h5>]]></description></item></channel></rss>";
	expect_note(feed.id, second, "Links and Show Notes:\n\nFirst link\n\nSecond link");
	response = NULL;
	fixture = NULL;
	assert(podcastrss_episode_notes(feed.id, second, &text) == PODCAST_NOTES_ERROR && !text);
	long_note = true;
	assert(podcastrss_episode_notes(feed.id, second, &text) == PODCAST_NOTES_OK);
	size_t bytes = strlen(text);
	assert(bytes > 30000 && bytes <= 32 * 1024 && bytes % 3 == 0);
	for (size_t i = 0; i < bytes; i += 3) {
		assert(memcmp(text + i, "€", 3) == 0);
	}
	free(text);
	puts("show notes: passed");
	return 0;
}
