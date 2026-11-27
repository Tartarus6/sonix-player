#include <string.h>

#include "common.h"
#include "media-browse.h"

static void parent_is(const char *path, const char *want)
{
	char buf[256];
	const char *got = media_browse_parent(path, buf, sizeof(buf));

	CHECK(got != NULL && strcmp(got, want) == 0,
				"parent('%s') = '%s', want '%s'",
				path, got != NULL ? got : "(null)", want);
}

int main(void)
{
	parent_is("/Filesystem/Music/Album", "/Filesystem/Music");
	parent_is("/Filesystem/Music", "/Filesystem");
	parent_is("/Filesystem", "/Filesystem");
	parent_is("/NowPlaying", "/NowPlaying");
	parent_is("/Filesystem/A/B/C/D", "/Filesystem/A/B/C");
	parent_is("relative/path", "relative");

	TEST_REPORT("parent");
}
