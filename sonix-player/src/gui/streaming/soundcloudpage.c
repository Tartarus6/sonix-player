#include "soundcloudpage.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/keyboard.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/system.h"
#include "src/system/streaming/soundcloud.h"
#include "src/system/streaming/soundcloudcache.h"
#include "src/system/streaming/soundcloudsync.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/core/utils.h"

// SoundCloud, front end.
//
// Everything in soundcloud.h blocks, so none of it runs on the LVGL thread:
// one worker, one slot for the job, and results come back through gui_post()
// (lv_async_call is not thread-safe here, see gui.h). A second tap replaces
// the first request rather than queuing behind it.
//
// The API pages by cursor, not by offset, so a list holds one page and keeps
// the cursors of the pages already seen. The next and previous rows walk them.

#define SC_PAGE_LIMIT 50
#define SC_PAGE_HISTORY 8

static lv_obj_t *login_screen;
static lv_obj_t *list_screen;
static lv_obj_t *search_screen;
static lv_obj_t *settings_screen;
lv_obj_t *soundcloud_screen;

typedef enum {
	JOB_NONE = 0,
	JOB_LOGIN_BEGIN,
	JOB_LOGIN_FINISH,
	JOB_SEARCH_TRACKS,
	JOB_SEARCH_PLAYLISTS,
	JOB_LIKED,
	JOB_REPOSTS,
	JOB_PLAYLISTS,
	JOB_PLAYLIST_TRACKS,
	JOB_PLAY,
	JOB_PREPARE,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char text[256]; // query, or the title the list will carry
	char id[SOUNDCLOUD_ID_MAX];
	char code[128];
	char cursor[SOUNDCLOUD_URL_MAX];
	int index;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static job_t pending;
static bool have_pending;
static bool worker_started;

// The worker writes these, the GUI reads them inside the async, and the worker
// waits for that read before taking another job. The wait is bounded: if
// gui_post() dropped the message, nothing will ever take the result.
static soundcloud_track_t result_tracks[SC_PAGE_LIMIT];
static soundcloud_playlist_t result_playlists[SC_PAGE_LIMIT];
static int result_count;
static job_kind_t result_kind;
static char result_error[256];
static char result_title[160];
static char result_cursor[SOUNDCLOUD_URL_MAX];
static char result_path[512];
static bool result_pending;
static pthread_cond_t result_taken = PTHREAD_COND_INITIALIZER;
static char login_url[SOUNDCLOUD_URL_MAX];

// State shared by functions defined further down.
static int play_index;
static int prepare_slot;
static int menu_index = -1;
static char sync_message[192];
static bool login_active;
static lv_obj_t *login_url_label;
static lv_obj_t *login_spin;
static char cursor_at[SC_PAGE_HISTORY][SOUNDCLOUD_URL_MAX];
static int cursor_pos;
static bool have_next;

static void rebuild_home(void);
static void row_clicked_cb(lv_event_t *e);
static void row_menu_cb(lv_event_t *e);
static void page_row_cb(lv_event_t *e);
static void job_done_cb(void *user);
static void login_failed(const char *why);
static bool list_has_tracks(job_kind_t kind);
static bool soundcloudpage_prepare_track(const char *path);
static void *worker_main(void *arg);

static job_t current_job;
static bool have_current_job;

static bool job_is_list(job_kind_t kind) {
	return kind != JOB_NONE && kind != JOB_PLAY && kind != JOB_PREPARE && kind != JOB_LOGIN_BEGIN &&
		   kind != JOB_LOGIN_FINISH;
}

static void submit(const job_t *job) {
	pthread_mutex_lock(&job_lock);
	pending = *job;
	have_pending = true;
	pthread_cond_signal(&job_ready);
	pthread_mutex_unlock(&job_lock);
}

static void result_taken_now(void) {
	pthread_mutex_lock(&job_lock);
	result_pending = false;
	pthread_cond_broadcast(&result_taken);
	pthread_mutex_unlock(&job_lock);
}

static void run(const job_t *job) {
	if (job_is_list(job->kind) && !job->cursor[0]) {
		// A new list starts at its first page. The step rows carry a cursor and
		// leave the job and the page history alone.
		cursor_pos = 0;
		cursor_at[0][0] = '\0';
		have_next = false;
		current_job = *job;
		have_current_job = true;
	}
	if (!worker_started) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
	}
	submit(job);
}

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

static lv_obj_t *list_container;
static lv_obj_t *list_title;
static lv_obj_t *list_empty;
static lv_obj_t *next_row;
static lv_obj_t *prev_row;
static job_kind_t list_kind;

// "3:42"
static void duration_text(int seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
	} else {
		snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
	}
}

static bool list_has_tracks(job_kind_t kind) {
	return kind == JOB_SEARCH_TRACKS || kind == JOB_LIKED || kind == JOB_REPOSTS || kind == JOB_PLAYLIST_TRACKS;
}

static lv_obj_t *make_row(const char *name, const char *detail, bool dimmed, int index) {
	lv_obj_t *row = lv_btn_create(list_container);
	lv_obj_set_size(row, lv_pct(100), 88);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// A back swipe often starts on a row, and a button keeps the press to
	// itself: without this the gesture does not work anywhere on the list.
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 6, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *label = lv_label_create(texts);
	lv_label_set_text(label, name);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_22));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	if (dimmed) {
		lv_obj_set_style_text_color(label, theme()->text_secondary, 0);
	}
	if (detail && detail[0]) {
		lv_obj_t *sub = lv_label_create(texts);
		lv_label_set_text(sub, detail);
		lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
		lv_obj_set_width(sub, lv_pct(100));
		lv_obj_set_height(sub, lv_font_get_line_height(&font_ui_18));
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_18, 0);
	}

	// A track row can be liked on the account; the mirror says which way the
	// one menu item reads. Playlists have no write action in the public API.
	if (list_has_tracks(list_kind)) {
		lv_obj_t *menu_btn = lv_btn_create(row);
		lv_obj_set_size(menu_btn, 44, 44);
		lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(menu_btn, 0, 0);
		lv_obj_set_style_pad_all(menu_btn, 0, 0);
		lv_obj_add_event_cb(menu_btn, row_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
		lv_obj_t *dots = lv_image_create(menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
		lv_obj_center(dots);
	}
	return row;
}

static void fill_list(void) {
	lv_obj_clean(list_container);
	list_kind = result_kind;

	for (int i = 0; i < result_count; i++) {
		char detail[400];
		if (list_has_tracks(result_kind)) {
			const soundcloud_track_t *t = &result_tracks[i];
			char length[16];
			duration_text(t->duration, length, sizeof(length));
			if (!t->streamable) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", t->artist, tr("not_included"));
			} else if (t->artist[0] && length[0]) {
				snprintf(detail, sizeof(detail), "%s \xC2\xB7 %s", t->artist, length);
			} else {
				snprintf(detail, sizeof(detail), "%s", t->artist[0] ? t->artist : length);
			}
			make_row(t->title, detail, !t->streamable, i);
		} else {
			const soundcloud_playlist_t *p = &result_playlists[i];
			snprintf(detail, sizeof(detail), "%s \xC2\xB7 %d", p->owner, p->track_count);
			make_row(p->title, detail, false, i);
		}
	}

	// A cursor has no offset to scroll to, so the pages are stepped here
	// rather than appended to the rows already on screen.
	prev_row = settingsrow_action(list_container, "soundcloud_previous_page", page_row_cb, (void *)(intptr_t)0);
	next_row = settingsrow_action(list_container, "soundcloud_next_page", page_row_cb, (void *)(intptr_t)1);
	lv_obj_set_hidden(prev_row, cursor_pos == 0);
	lv_obj_set_hidden(next_row, !have_next || cursor_pos + 1 >= SC_PAGE_HISTORY);
	lv_obj_set_hidden(list_empty, result_count != 0);
	lv_label_set_text(list_title, result_title);
}

static void row_clicked_cb(lv_event_t *e) {
	// A gesture ending over a row is not a tap on it: click events bubble, so
	// the click that closes a drag arrives here too.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count) {
		return;
	}
	job_t job = {0};
	if (list_has_tracks(list_kind)) {
		if (!result_tracks[index].streamable) {
			gui_notify_popup(tr("not_included"));
			return;
		}
		play_index = index;
		job.kind = JOB_PLAY;
		run(&job);
		return;
	}
	job.kind = JOB_PLAYLIST_TRACKS;
	snprintf(job.id, sizeof(job.id), "%s", result_playlists[index].id);
	snprintf(job.text, sizeof(job.text), "%s", result_playlists[index].title);
	run(&job);
}

static void sync_result_async(void *user) {
	bool reload = (bool)(intptr_t)user;
	if (sync_message[0]) {
		gui_notify_popup(sync_message);
		return;
	}
	if (reload && have_current_job && (list_kind == JOB_LIKED || list_kind == JOB_PLAYLIST_TRACKS)) {
		job_t job = current_job;
		job.cursor[0] = '\0';
		run(&job);
	}
}

static void sync_done(bool ok, const char *error, void *user) {
	snprintf(sync_message, sizeof(sync_message), "%s",
			 ok ? "" : (error && error[0] ? error : tr("download_failed")));
	gui_post(sync_result_async, user);
}

static void track_like(void *user) {
	bool want = (bool)(intptr_t)user;
	if (menu_index < 0 || menu_index >= result_count || !list_has_tracks(list_kind)) {
		return;
	}
	// Removing from the liked list leaves a row that is no longer true; adding
	// from a search does not change the list being shown.
	soundcloudsync_favorite_toggle(result_tracks[menu_index].id, want, sync_done,
								   (void *)(intptr_t)(want ? 0 : 1));
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= result_count || !list_has_tracks(list_kind)) {
		return;
	}
	menu_index = index;
	bool liked = soundcloudsync_is_favorite(result_tracks[index].id);
	popover_item_t item;
	memset(&item, 0, sizeof(item));
	item.label = liked ? "remove_from_favourites" : "medialist_add_to_favourites";
	item.action = track_like;
	item.user = (void *)(intptr_t)(!liked);
	popover_show(lv_event_get_current_target(e), &item, 1);
}

// ---------------------------------------------------------------------------
// playback
//
// The worker resolves the stream and writes the prebuffer; the queue is built
// here and handed to device_state in one call, as on Qobuz. Every path comes
// from soundcloudcache_path(), so a track not downloaded yet has a stable
// name and prepare_track can ask for it when playback reaches it.
// ---------------------------------------------------------------------------

static soundcloud_track_t queued_tracks[SC_PAGE_LIMIT];
static char queued_paths[SC_PAGE_LIMIT][512];
static const char *queue_ptrs[SC_PAGE_LIMIT];
static int queued_count;
static bool prepare_pending;

static void play_started_async(void *user) {
	(void)user;
	player_refresh_now_playing();
	switch_screen(player_screen);
}

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup("downloading");
}

static void no_card_async(void *user) {
	(void)user;
	gui_notify_no_card();
}

static void restart_queue(int slot) {
	if (slot < 0 || slot >= queued_count) {
		return;
	}
	for (int i = 0; i < queued_count; i++) {
		queue_ptrs[i] = queued_paths[i];
	}
	device_state_play_list(queue_ptrs, queued_count, slot);
	player_refresh_now_playing();
}

static void restart_async(void *user) { restart_queue((int)(intptr_t)user); }

static void start_playback(int index) {
	if (index < 0 || index >= result_count) {
		return;
	}
	if (!soundcloudcache_ready()) {
		gui_post(no_card_async, NULL);
		return;
	}
	queued_count = 0;
	int start = 0;
	for (int i = 0; i < result_count && queued_count < SC_PAGE_LIMIT; i++) {
		if (!result_tracks[i].streamable) {
			continue;
		}
		queued_tracks[queued_count] = result_tracks[i];
		if (i == index) {
			start = queued_count;
			// The real name, returned by cache_start a moment ago.
			snprintf(queued_paths[queued_count], sizeof(queued_paths[0]), "%s", result_path);
		} else {
			soundcloudcache_path(result_tracks[i].id, queued_paths[queued_count], sizeof(queued_paths[0]));
		}
		queued_count++;
	}
	if (queued_count > 0) {
		restart_queue(start);
		gui_post(play_started_async, NULL);
	}
}

// device_state calls this from whatever thread starts a file, the GUI thread
// for a tap and this page's worker for an on-demand prepare. It must not block.
// Anything that draws goes through gui_post().
static bool soundcloudpage_prepare_track(const char *path) {
	if (!path || !path[0]) {
		return true;
	}
	int slot = -1;
	for (int i = 0; i < queued_count; i++) {
		if (strcmp(queued_paths[i], path) == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		return true; // not a path this page queued
	}
	char real[512];
	if (soundcloudcache_has(queued_tracks[slot].id, real, sizeof(real))) {
		return true;
	}
	if (prepare_pending) {
		return false; // the first request is already on its way
	}
	prepare_pending = true;
	prepare_slot = slot;
	device_state_stop(); // the previous track is reading a file about to be replaced
	gui_post(downloading_note_async, NULL);

	job_t job = {0};
	job.kind = JOB_PREPARE;
	snprintf(job.id, sizeof(job.id), "%s", queued_tracks[slot].id);
	run(&job);
	return false;
}

static void worker_error(const char *fallback) {
	const char *why = soundcloud_last_error();
	snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr(fallback));
}

static void cache_error(void) {
	const char *why = soundcloudcache_last_error();
	snprintf(result_error, sizeof(result_error), "%s", why && why[0] ? why : tr("download_failed"));
}

static void do_play(int index) {
	const soundcloud_track_t *track = &result_tracks[index];
	streamturn_abandon_all(); // the bandwidth belongs to the track just tapped
	soundcloud_stream_t stream;
	if (!soundcloud_track_stream(track->id, &stream)) {
		worker_error("download_failed");
	} else if (!soundcloudcache_start(track->id, &stream, track->duration, result_path, sizeof(result_path))) {
		cache_error();
	}
}

static void do_prepare(const char *id) {
	int duration = 0;
	for (int i = 0; i < queued_count; i++) {
		if (strcmp(queued_tracks[i].id, id) == 0) {
			duration = queued_tracks[i].duration;
			break;
		}
	}
	streamturn_abandon_all();
	soundcloud_stream_t stream;
	if (!soundcloud_track_stream(id, &stream)) {
		worker_error("download_failed");
	} else if (!soundcloudcache_start(id, &stream, duration, result_path, sizeof(result_path))) {
		cache_error();
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("soundcloud worker");
	for (;;) {
		pthread_mutex_lock(&job_lock);
		while (!have_pending) {
			pthread_cond_wait(&job_ready, &job_lock);
		}
		if (result_pending) {
			// Two seconds of patience, see the comment on result_pending.
			struct timespec until;
			struct timeval now;
			gettimeofday(&now, NULL);
			until.tv_sec = now.tv_sec + 2;
			until.tv_nsec = now.tv_usec * 1000;
			while (result_pending) {
				if (pthread_cond_timedwait(&result_taken, &job_lock, &until) == ETIMEDOUT) {
					break;
				}
			}
		}
		job_t job = pending;
		have_pending = false;
		pthread_mutex_unlock(&job_lock);

		result_error[0] = '\0';
		result_kind = job.kind;
		result_count = 0;
		result_cursor[0] = '\0';
		result_path[0] = '\0';

		char cursor[SOUNDCLOUD_URL_MAX];
		snprintf(cursor, sizeof(cursor), "%s", job.cursor);
		int n = 0;
		switch (job.kind) {
		case JOB_LOGIN_BEGIN:
			if (!soundcloud_login_begin(login_url, sizeof(login_url))) {
				worker_error("soundcloud_not_configured");
			}
			break;
		case JOB_LOGIN_FINISH:
			if (!soundcloud_login_finish(job.code)) {
				worker_error("soundcloud_not_signed_in");
			}
			break;
		case JOB_SEARCH_TRACKS:
			n = soundcloud_search_tracks(job.text, cursor, sizeof(cursor), result_tracks, SC_PAGE_LIMIT);
			break;
		case JOB_SEARCH_PLAYLISTS:
			n = soundcloud_search_playlists(job.text, cursor, sizeof(cursor), result_playlists, SC_PAGE_LIMIT);
			break;
		case JOB_LIKED:
			n = soundcloud_liked_tracks(cursor, sizeof(cursor), result_tracks, SC_PAGE_LIMIT);
			break;
		case JOB_REPOSTS:
			n = soundcloud_reposts(cursor, sizeof(cursor), result_tracks, SC_PAGE_LIMIT);
			break;
		case JOB_PLAYLISTS:
			n = soundcloud_user_playlists(cursor, sizeof(cursor), result_playlists, SC_PAGE_LIMIT);
			break;
		case JOB_PLAYLIST_TRACKS:
			n = soundcloud_playlist_tracks(job.id, cursor, sizeof(cursor), result_tracks, SC_PAGE_LIMIT);
			break;
		case JOB_PLAY:
			do_play(job.index);
			break;
		case JOB_PREPARE:
			do_prepare(job.id);
			break;
		default:
			break;
		}
		if (n < 0) {
			worker_error("download_failed");
			n = 0;
		}
		if (job.kind != JOB_PLAY && job.kind != JOB_PREPARE) {
			result_count = n;
			snprintf(result_cursor, sizeof(result_cursor), "%s", cursor);
			// Home rows carry no title of their own.
			if (job.text[0]) {
				snprintf(result_title, sizeof(result_title), "%.150s", job.text);
			} else if (job.kind == JOB_LIKED) {
				snprintf(result_title, sizeof(result_title), "%s", tr("soundcloud_likes"));
			} else if (job.kind == JOB_REPOSTS) {
				snprintf(result_title, sizeof(result_title), "%s", tr("soundcloud_reposts"));
			} else if (job.kind == JOB_PLAYLISTS) {
				snprintf(result_title, sizeof(result_title), "%s", tr("soundcloud_playlists"));
			} else {
				result_title[0] = '\0';
			}
		}
		pthread_mutex_lock(&job_lock);
		result_pending = true;
		pthread_mutex_unlock(&job_lock);
		gui_post(job_done_cb, NULL);
	}
	return NULL;
}

static void page_row_cb(lv_event_t *e) {
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	bool next = (bool)(intptr_t)lv_event_get_user_data(e);
	if (next) {
		if (!have_next || cursor_pos + 1 >= SC_PAGE_HISTORY) {
			return;
		}
		snprintf(cursor_at[cursor_pos + 1], sizeof(cursor_at[0]), "%s", result_cursor);
		cursor_pos++;
	} else {
		if (cursor_pos == 0) {
			return;
		}
		cursor_pos--;
	}
	if (!have_current_job) {
		return;
	}
	job_t job = current_job;
	snprintf(job.cursor, sizeof(job.cursor), "%s", cursor_at[cursor_pos]);
	run(&job);
}

static void job_done_cb(void *user) {
	(void)user;
	if (result_error[0]) {
		if (result_kind == JOB_LOGIN_BEGIN || result_kind == JOB_LOGIN_FINISH) {
			login_failed(result_error);
		} else {
			if (result_kind == JOB_PREPARE) {
				prepare_pending = false;
			}
			gui_notify_popup(result_error);
		}
		result_taken_now();
		return;
	}
	switch (result_kind) {
	case JOB_LOGIN_BEGIN:
		if (login_active) {
			lv_label_set_text(login_url_label, login_url);
			lv_obj_set_hidden(login_url_label, false);
			lv_obj_set_hidden(login_spin, true);
		}
		break;
	case JOB_LOGIN_FINISH:
		login_active = false;
		rebuild_home();
		if (lv_screen_active() == login_screen) {
			switch_screen(soundcloud_screen);
		}
		toast_success("soundcloud_connected");
		break;
	case JOB_PLAY:
		start_playback(play_index);
		break;
	case JOB_PREPARE:
		prepare_pending = false;
		// A new queue may have been started while this was downloading.
		if (prepare_slot >= 0 && prepare_slot < queued_count &&
			strcmp(queued_paths[prepare_slot], result_path) == 0) {
			restart_async((void *)(intptr_t)prepare_slot);
		}
		break;
	default:
		have_next = result_cursor[0] != '\0'; // fill_list() draws the page rows
		fill_list();
		if (lv_screen_active() != list_screen && cursor_pos == 0) {
			switch_screen(list_screen);
		}
		break;
	}
	result_taken_now();
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

static lv_obj_t *search_field;
static lv_obj_t *search_clear_btn;
static keyboard_t *search_keyboard;
static int search_kind; // 0 tracks, 1 playlists
static lv_obj_t *search_pills[2];

static void paint_search_pills(void) {
	for (int i = 0; i < 2; i++) {
		bool on = i == search_kind;
		lv_obj_set_style_bg_color(search_pills[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(search_pills[i], 0), on ? lv_color_white() : theme()->text_primary,
									0);
	}
}

static void search_pill_cb(lv_event_t *e) {
	search_kind = (int)(intptr_t)lv_event_get_user_data(e);
	paint_search_pills();
}

static void refresh_search_clear(void) {
	if (search_clear_btn) {
		const char *text = lv_textarea_get_text(search_field);
		lv_obj_set_hidden(search_clear_btn, !(text && text[0]));
	}
}

static void search_field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_search_clear();
}

static void search_clear_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
}

static void search_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(search_field);
	if (!text || !*text) {
		return;
	}
	job_t job = {0};
	job.kind = search_kind == 0 ? JOB_SEARCH_TRACKS : JOB_SEARCH_PLAYLISTS;
	snprintf(job.text, sizeof(job.text), "%s", text);
	run(&job);
}

static void open_search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	lv_obj_add_state(search_field, LV_STATE_FOCUSED);
	lv_obj_send_event(search_field, LV_EVENT_FOCUSED, NULL);
	refresh_search_clear();
	switch_screen(search_screen);
}

static void build_search_page(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "soundcloud_search_tracks");
	int top = settingsrow_content_top(cfg);

	search_field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(search_field, true);
	lv_textarea_set_placeholder_text(search_field, tr("soundcloud_search_tracks"));
	lv_obj_set_size(search_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(search_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(search_field, &theme_style_card, 0);
	lv_obj_set_style_radius(search_field, 12, 0);
	lv_obj_set_style_border_width(search_field, 0, 0);
	lv_obj_set_style_shadow_width(search_field, 0, 0);
	lv_obj_set_style_pad_all(search_field, 14, 0);
	lv_obj_set_style_pad_right(search_field, 60, 0);
	lv_obj_set_style_text_font(search_field, &font_ui_24, 0);
	keyboard_style_caret(search_field);
	lv_obj_add_event_cb(search_field, search_field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The x on the right edge of the field, hidden while it is empty.
	search_clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(search_clear_btn, 56, 56);
	lv_obj_align(search_clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 3);
	lv_obj_set_style_bg_opa(search_clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(search_clear_btn, 0, 0);
	lv_obj_set_style_border_width(search_clear_btn, 0, 0);
	lv_obj_set_style_pad_all(search_clear_btn, 0, 0);
	lv_obj_set_hidden(search_clear_btn, true);
	lv_obj_add_event_cb(search_clear_btn, search_clear_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *clear_icon = lv_image_create(search_clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(clear_icon, LV_OPA_70, 0);
	lv_obj_center(clear_icon);

	static const char *const KINDS[2] = {"soundcloud_search_tracks", "soundcloud_playlists"};
	for (int i = 0; i < 2; i++) {
		lv_obj_t *pill = lv_btn_create(search_screen);
		lv_obj_set_size(pill, 200, 52);
		lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_shadow_width(pill, 0, 0);
		lv_obj_set_style_border_width(pill, 0, 0);
		lv_obj_align(pill, LV_ALIGN_TOP_LEFT, cfg->padding + i * 210, top + 78);
		lv_obj_add_event_cb(pill, search_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_t *label = lv_label_create(pill);
		lv_label_set_text(label, tr(KINDS[i]));
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_center(label);
		search_pills[i] = pill;
	}
	paint_search_pills();
	search_keyboard = keyboard_create(search_screen, cfg->screen_width, 316, search_field, NULL, "ok",
									  search_accept_cb, NULL);
	switcher_attach_back_gesture(search_screen);
	theme_register_refresh(paint_search_pills);
}

// ---------------------------------------------------------------------------
// sign-in
//
// soundcloud_login_begin() makes a URL that has to be opened in a browser. The
// person approves there, copies the code and types it in; the worker exchanges
// it for tokens. LVGL has no clipboard, so a tap selects the whole URL on the
// label rather than pretending to copy it.
// ---------------------------------------------------------------------------

static lv_obj_t *login_code_field;
static keyboard_t *login_keyboard;
static lv_obj_t *login_message;
static lv_obj_t *login_retry;

static void select_url_cb(lv_event_t *e) {
	lv_obj_t *label = lv_event_get_target(e);
	static bool selected;
	if (selected) {
		lv_label_set_text_selection_start(label, LV_LABEL_TEXT_SELECTION_OFF);
		lv_label_set_text_selection_end(label, LV_LABEL_TEXT_SELECTION_OFF);
	} else {
		lv_label_set_text_selection_start(label, 0);
		lv_label_set_text_selection_end(label, (uint32_t)strlen(lv_label_get_text(label)));
	}
	selected = !selected;
}

static void paint_login(void) {
	if (!login_url_label) {
		return;
	}
	lv_obj_set_style_text_color(login_url_label, theme()->accent, 0);
	lv_obj_set_style_text_color(login_message, theme()->text_primary, 0);
	lv_obj_set_style_bg_color(login_retry, theme()->accent, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(login_retry, 0), lv_color_white(), 0);
}

static void login_start(void) {
	login_active = true;
	lv_label_set_text(login_url_label, "");
	lv_obj_set_hidden(login_url_label, true);
	lv_obj_set_hidden(login_spin, false);
	lv_label_set_text(login_message, "");
	lv_obj_set_hidden(login_message, true);
	lv_obj_set_hidden(login_retry, true);
	job_t job = {0};
	job.kind = JOB_LOGIN_BEGIN;
	run(&job);
}

static void login_failed(const char *why) {
	if (!login_active) {
		return;
	}
	login_active = false;
	lv_obj_set_hidden(login_spin, true);
	lv_label_set_text(login_message, why && why[0] ? why : tr("soundcloud_not_signed_in"));
	lv_obj_set_hidden(login_message, false);
	lv_obj_set_hidden(login_retry, false);
}

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *code = lv_textarea_get_text(login_code_field);
	if (!code || !*code) {
		gui_notify_popup(tr("soundcloud_code"));
		return;
	}
	job_t job = {0};
	job.kind = JOB_LOGIN_FINISH;
	snprintf(job.code, sizeof(job.code), "%s", code);
	lv_textarea_set_text(login_code_field, "");
	lv_obj_set_hidden(login_spin, false);
	lv_obj_set_hidden(login_message, true);
	run(&job);
}

static void login_retry_cb(lv_event_t *e) {
	(void)e;
	if (!switcher_back_drag_active()) {
		login_start();
	}
}

static void open_login_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(login_code_field, "");
	keyboard_reset(login_keyboard);
	lv_obj_add_state(login_code_field, LV_STATE_FOCUSED);
	lv_obj_send_event(login_code_field, LV_EVENT_FOCUSED, NULL);
	switch_screen(login_screen);
	login_start();
}

static void login_unloaded_cb(lv_event_t *e) {
	(void)e;
	login_active = false; // the tokens may arrive after the page has gone
}

static void build_login_page(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);
	settingsrow_title(login_screen, cfg, "soundcloud_sign_in");
	int top = settingsrow_content_top(cfg);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("soundcloud_not_signed_in"));

	lv_obj_t *heading = lv_label_create(login_screen);
	lv_label_set_text(heading, tr("soundcloud_sign_in_url"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_20, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, top + 50);

	lv_obj_t *card = lv_btn_create(login_screen);
	lv_obj_set_size(card, cfg->screen_width - 2 * cfg->padding, 120);
	lv_obj_align(card, LV_ALIGN_TOP_LEFT, cfg->padding, top + 84);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 14, 0);
	lv_obj_set_scrollable(card, false);

	login_url_label = lv_label_create(card);
	lv_obj_set_width(login_url_label, lv_pct(100));
	lv_label_set_long_mode(login_url_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(login_url_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_url_label, &font_ui_20, 0);
	lv_obj_set_clickable(login_url_label, true);
	lv_obj_add_event_cb(login_url_label, select_url_cb, LV_EVENT_CLICKED, NULL);
	lv_label_set_text(login_url_label, "");

	login_code_field = lv_textarea_create(login_screen);
	lv_textarea_set_one_line(login_code_field, true);
	lv_textarea_set_placeholder_text(login_code_field, tr("soundcloud_code"));
	lv_obj_set_size(login_code_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(login_code_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(login_code_field, LV_ALIGN_TOP_LEFT, cfg->padding, top + 222);
	lv_obj_add_style(login_code_field, &theme_style_card, 0);
	lv_obj_set_style_radius(login_code_field, 12, 0);
	lv_obj_set_style_border_width(login_code_field, 0, 0);
	lv_obj_set_style_shadow_width(login_code_field, 0, 0);
	lv_obj_set_style_pad_all(login_code_field, 14, 0);
	lv_obj_set_style_text_font(login_code_field, &font_ui_24, 0);
	keyboard_style_caret(login_code_field);

	login_spin = spinner_create(login_screen, &icon_loader_big);
	lv_obj_align(login_spin, LV_ALIGN_TOP_MID, 0, top + 300);
	lv_obj_set_hidden(login_spin, true);
	login_message = lv_label_create(login_screen);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(login_message, LV_ALIGN_TOP_MID, 0, top + 300);
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_obj_set_hidden(login_message, true);

	login_retry = lv_btn_create(login_screen);
	lv_obj_set_size(login_retry, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(login_retry, 22, 0);
	lv_obj_set_style_radius(login_retry, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(login_retry, 0, 0);
	lv_obj_set_style_border_width(login_retry, 0, 0);
	lv_obj_align(login_retry, LV_ALIGN_TOP_MID, 0, top + 300);
	lv_obj_add_event_cb(login_retry, login_retry_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *retry_label = lv_label_create(login_retry);
	lv_label_set_text(retry_label, tr("soundcloud_sign_in"));
	lv_obj_set_style_text_font(retry_label, &font_ui_20, 0);
	lv_obj_center(retry_label);
	lv_obj_set_hidden(login_retry, true);

	// The accept key is one QWERTY key wide. "ok" fits; a longer word clips.
	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, login_code_field, NULL,
									 "ok", login_accept_cb, NULL);
	lv_obj_add_event_cb(login_screen, login_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(login_screen);
	paint_login();
	theme_register_refresh(paint_login);
}

// ---------------------------------------------------------------------------
// account settings
// ---------------------------------------------------------------------------

static void do_logout(void *user) {
	(void)user;
	soundcloud_logout();
	// Tracks downloaded under this account are of no use to the next one. The
	// covers stay: that directory is shared with the other services.
	soundcloudcache_clear();
	soundcloudsync_forget();
	queued_count = 0;
	rebuild_home();
	toast_success("signed_out");
}

static void logout_cb(lv_event_t *e) {
	(void)e;
	char message[256];
	snprintf(message, sizeof(message), tr("sign_out_confirm_note"), soundcloud_display_name());
	confirm_show("soundcloud_logout", message, "leave", do_logout, NULL);
}

static void clear_cache_cb(lv_event_t *e) {
	(void)e;
	soundcloudcache_clear();
	toast_success("soundcloud_cache_cleared");
}

static void open_settings_cb(lv_event_t *e) {
	(void)e;
	switch_screen(settings_screen);
}

static void build_settings_page(gui_config_t *cfg) {
	settings_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(settings_screen, cfg, "soundcloud_account_settings");
	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("soundcloud_settings_note"));
	settingsrow_action(container, "soundcloud_clear_cache", clear_cache_cb, NULL);
	settingsrow_action(container, "soundcloud_logout", logout_cb, NULL);
	switcher_attach_back_gesture(settings_screen);
}

// ---------------------------------------------------------------------------
// the main page
// ---------------------------------------------------------------------------

static lv_obj_t *home_container;
static lv_obj_t *settings_btn;
static lv_obj_t *account_value;

static lv_obj_t *corner_button(gui_config_t *cfg, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(soundcloud_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, &icon_music_settings);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	return button;
}

static void simple_job_cb(lv_event_t *e) {
	job_t job = {0};
	job.kind = (job_kind_t)(intptr_t)lv_event_get_user_data(e);
	run(&job);
}

static void rebuild_home(void) {
	lv_obj_clean(home_container);
	account_value = NULL;
	lv_obj_set_hidden(settings_btn, !(soundcloud_configured() && soundcloud_logged_in()));

	if (!soundcloud_configured()) {
		lv_obj_t *note = lv_label_create(home_container);
		lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(note, lv_pct(100));
		lv_obj_add_style(note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(note, &font_ui_20, 0);
		lv_label_set_text(note, tr("soundcloud_not_configured"));
		return;
	}
	if (!soundcloud_logged_in()) {
		settingsrow_add(home_container, "soundcloud_sign_in", NULL, open_login_cb, NULL);
		return;
	}
	// The account row carries the name and leads to sign-out and cache care.
	settingsrow_add(home_container, "soundcloud_account_settings", &account_value, open_settings_cb, NULL);
	if (account_value) {
		lv_label_set_text(account_value, soundcloud_display_name());
	}
	settingsrow_add(home_container, "soundcloud_search_tracks", NULL, open_search_cb, NULL);
	settingsrow_add(home_container, "soundcloud_likes", NULL, simple_job_cb, (void *)(intptr_t)JOB_LIKED);
	settingsrow_add(home_container, "soundcloud_reposts", NULL, simple_job_cb, (void *)(intptr_t)JOB_REPOSTS);
	settingsrow_add(home_container, "soundcloud_playlists", NULL, simple_job_cb, (void *)(intptr_t)JOB_PLAYLISTS);
}

static void home_loaded_cb(lv_event_t *e) {
	(void)e;
	// The card may have been removed or replaced while the page was away.
	soundcloudcache_set_root(storage_sd_root());
	if (soundcloud_logged_in() && !soundcloudsync_favorites_known()) {
		soundcloudsync_favorites_refresh(NULL);
	}
	// Everything here can change elsewhere: the keys, the session, whose
	// access token may have expired and been refreshed, and the card.
	rebuild_home();
}

static void build_list_page(gui_config_t *cfg) {
	list_screen = lv_obj_create(NULL);
	list_container = settingsrow_page(list_screen, cfg, "soundcloud");
	list_title = settingsrow_page_title(list_screen);
	list_empty = lv_label_create(list_screen);
	lv_label_set_text(list_empty, tr("soundcloud_empty"));
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_24, 0);
	lv_obj_align(list_empty, LV_ALIGN_TOP_MID, 0, settingsrow_content_top(cfg) + 100);
	lv_obj_set_hidden(list_empty, true);
	switcher_attach_back_gesture(list_screen);
	player_sheet_attach_drag(list_screen, true);
	player_sheet_attach_drag(list_container, true);
}

void soundcloudpage_init(gui_config_t *cfg) {
	soundcloud_screen = lv_obj_create(NULL);
	home_container = settingsrow_page(soundcloud_screen, cfg, "soundcloud");
	settingsrow_title_corner_slots(settingsrow_page_title(soundcloud_screen), cfg, 1);
	settings_btn = corner_button(cfg, open_settings_cb);

	build_list_page(cfg);
	build_search_page(cfg);
	build_settings_page(cfg);
	build_login_page(cfg);

	// device_state calls this from the thread that starts a file, this page's
	// worker included. A path that is not a SoundCloud cache file returns true
	// at once, so the Qobuz callback still sees it.
	device_state_add_prepare_cb(soundcloudpage_prepare_track);
	soundcloudcache_set_root(storage_sd_root());
	rebuild_home();

	lv_obj_add_event_cb(soundcloud_screen, home_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(soundcloud_screen);
	player_sheet_attach_drag(soundcloud_screen, true);
	player_sheet_attach_drag(home_container, true);
}
