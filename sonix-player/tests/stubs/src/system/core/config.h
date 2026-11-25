#ifndef CONFIG_H_STUB
#define CONFIG_H_STUB
#include <stdbool.h>
/* Provided by pod_layout_test.c */
long config_get_int(const char *section, const char *key, long fallback);
void config_set_int(const char *section, const char *key, long value);
bool config_save(void);
#endif
