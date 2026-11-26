#include "addons.h"

#include <stddef.h>
#include <string.h>

#include "src/system/core/config.h"

// The list, and nothing else in this file knows what any of them do: the gate
// is asked by the page that would build the module, and by the menu that would
// offer it.
//
// Porting note: the fork's list also had the streaming, games, tools and DSP
// modules. Those feature groups are not part of this port, so an entry for
// one would be a switch that does nothing -- the gate is only honoured where
// the page asks. These are the V1 extras whose gates this build actually
// enforces (Music settings and the two V1-only settings pages).
static const addon_t ADDONS[] = {
	{"listenbrainz", "listenbrainz", "addon_listenbrainz_note", "plugins_section_features"},
	{"battery_profile", "battery_profile_title", "battery_profile_help", "plugins_section_features"},
	{"bootpic", "bootpic_title", "bootpic_empty", "plugins_section_features"},
};

int addons_count(void) { return (int)(sizeof(ADDONS) / sizeof(ADDONS[0])); }

const addon_t *addons_at(int index) {
	if (index < 0 || index >= addons_count()) {
		return NULL;
	}
	return &ADDONS[index];
}

const addon_t *addons_find(const char *id) {
	if (!id) {
		return NULL;
	}
	for (int i = 0; i < addons_count(); i++) {
		if (strcmp(ADDONS[i].id, id) == 0) {
			return &ADDONS[i];
		}
	}
	return NULL;
}

// Fresh install defaults. The two settings pages ship on; scrobbling waits
// for the user and a token, as it does in the fork.
static bool addon_is_default_on(const char *id) {
	if (!id) {
		return false;
	}
	if (strcmp(id, "battery_profile") == 0) return true;
	if (strcmp(id, "bootpic") == 0) return true;
	return false;
}

bool addons_enabled(const char *id) {
	if (!id) {
		return true;
	}
	return config_get_bool("addons", id, addon_is_default_on(id));
}

bool addons_set(const char *id, bool on) {
	if (!id || !addons_find(id)) {
		return false;
	}
	bool previous = addons_enabled(id);
	config_set_bool("addons", id, on);
	if (addons_enabled(id) != on || !config_save()) {
		config_set_bool("addons", id, previous);
		config_save();
		return false;
	}
	return true;
}

void addons_seed(bool fresh_install) {
	if (config_get_bool("addons", "seeded", false)) {
		return;
	}
	for (int i = 0; i < addons_count(); i++) {
		bool on = fresh_install ? addon_is_default_on(ADDONS[i].id) : true;
		config_set_bool("addons", ADDONS[i].id, on);
	}
	config_set_bool("addons", "seeded", true);
	// The first-run hint explains the lean defaults, so it is for a fresh
	// install only. An existing device has nothing new to be told.
	config_set_bool("system", "plugins_notice_shown", !fresh_install);
	config_save();
}
