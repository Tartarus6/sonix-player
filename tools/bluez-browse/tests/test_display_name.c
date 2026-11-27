#include <string.h>

#include "common.h"
#include "avrcp-tg-browse.h"

int main(void)
{
	GHashTable *meta = g_hash_table_new_full(g_str_hash, g_str_equal,
							g_free, g_free);
	char buf[128];

	g_hash_table_insert(meta, g_strdup("xesam:title"),
						g_strdup("Golden Hour"));
	CHECK(strcmp(tg_display_name(meta, "fallback", buf, sizeof(buf)),
						"Golden Hour") == 0,
						"title must win over fallback");
	g_hash_table_remove(meta, "xesam:title");
	CHECK(strcmp(tg_display_name(meta, "track07.flac", buf, sizeof(buf)),
						"track07.flac") == 0,
						"fallback must be used with no metadata");

	g_hash_table_unref(meta);
	TEST_REPORT("display-name");
}
