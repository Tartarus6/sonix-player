#ifndef V1_STREAMING_ICONS_H
#define V1_STREAMING_ICONS_H

// Icon aliases for the ported V1 streaming pages.
//
// The fork shipped a larger icon set, including service badges that are not in
// the three-device build. Those pages keep referring to the V1 names; the
// aliases point them at existing glyphs so no new artwork or theme mapping is
// needed. On the HiBy panels these render exactly like the ordinary icons the
// shell already uses.

#include "src/gui/shell/icons.h"

#define icon_jellyfin_saved      icon_menu_favourites
#define icon_keylock_quick       icon_caps_lock
#define icon_menu_qobuz_albums   icon_menu_album
#define icon_menu_qobuz_favourites icon_menu_favourites
#define icon_menu_qobuz_genres   icon_menu_genre
#define icon_menu_qobuz_playlists icon_menu_playlist
#define icon_menu_sc_account     icon_menu_settings
#define icon_menu_sc_likes       icon_heart
#define icon_menu_sc_playlists   icon_menu_playlist
#define icon_menu_sc_reposts     icon_music_note
#define icon_menu_sub_artists    icon_menu_artist
#define icon_menu_sub_folders    icon_folder
#define icon_menu_sub_server     icon_menu_settings
#define icon_menu_sub_test       icon_search
#define icon_src                 icon_sort_az
#define icon_size                icon_menu_settings
#define icon_style               icon_menu_settings
#define icon_sub_artists_glyph   icon_menu_artist
#define icon_sub_folders_glyph   icon_folder
#define icon_sub_server_glyph    icon_menu_settings
#define icon_sub_test_glyph      icon_search

#endif /* V1_STREAMING_ICONS_H */
