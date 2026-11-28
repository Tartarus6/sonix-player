#include "podcastnotes.h"

#include <pthread.h>
#include <stdlib.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcastcache.h"

static lv_obj_t *screen, *page, *label;
static lv_timer_t *poll;
static char *shown_text;
static long long selected_feed, selected_episode;

// At most one feed fetch and one queued selection. A generation lets the
// worker drop late results without touching the view or keeping a file open.
static pthread_mutex_t notes_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
	unsigned generation;
	bool running, pending, ready;
	long long feed_id, episode_id;
	podcast_notes_result_t result;
	char *text;
} job;

static void *fetch_main(void *arg) {
	(void)arg;
	pthread_mutex_lock(&notes_lock);
	while (job.pending) {
		unsigned generation = job.generation;
		long long feed_id = job.feed_id, episode_id = job.episode_id;
		job.pending = false;
		pthread_mutex_unlock(&notes_lock);

		char *text = NULL;
		podcast_notes_result_t result = podcast_episode_notes(feed_id, episode_id, &text);

		pthread_mutex_lock(&notes_lock);
		if (generation == job.generation) {
			job.text = text;
			job.result = result;
			job.ready = true;
		} else {
			free(text);
		}
	}
	job.running = false;
	pthread_mutex_unlock(&notes_lock);
	return NULL;
}

static void show_result(podcast_notes_result_t result, char *text) {
	switch (result) {
	case PODCAST_NOTES_OK:
		shown_text = text;
		lv_label_set_text_static(label, shown_text);
		break;
	case PODCAST_NOTES_EMPTY:
		lv_label_set_text(label, tr("podcast_notes_empty"));
		break;
	case PODCAST_NOTES_UNAVAILABLE:
		lv_label_set_text(label, tr("podcast_notes_unavailable"));
		break;
	case PODCAST_NOTES_ERROR:
		lv_label_set_text(label, tr("podcast_notes_error"));
		break;
	}
}

static void poll_cb(lv_timer_t *timer) {
	pthread_mutex_lock(&notes_lock);
	if (!job.ready) {
		pthread_mutex_unlock(&notes_lock);
		return;
	}
	podcast_notes_result_t result = job.result;
	char *text = job.text;
	job.text = NULL;
	job.ready = false;
	pthread_mutex_unlock(&notes_lock);
	lv_timer_pause(timer);
	show_result(result, text);
}

static void clear_note(void) {
	if (poll) {
		lv_timer_pause(poll);
	}
	pthread_mutex_lock(&notes_lock);
	job.generation++;
	job.pending = job.ready = false;
	free(job.text);
	job.text = NULL;
	pthread_mutex_unlock(&notes_lock);
	lv_label_set_text_static(label, "");
	free(shown_text);
	shown_text = NULL;
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	clear_note();
}

static void load_note(void) {
	clear_note();
	lv_obj_scroll_to_y(page, 0, LV_ANIM_OFF);
	lv_label_set_text(label, tr("loading"));
	if (!poll) {
		show_result(PODCAST_NOTES_ERROR, NULL);
		return;
	}
	pthread_mutex_lock(&notes_lock);
	job.feed_id = selected_feed;
	job.episode_id = selected_episode;
	job.pending = true;
	if (!job.running) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, fetch_main, NULL) != 0) {
			job.pending = false;
			pthread_mutex_unlock(&notes_lock);
			show_result(PODCAST_NOTES_ERROR, NULL);
			return;
		}
		job.running = true;
		pthread_detach(thread);
	}
	pthread_mutex_unlock(&notes_lock);
	lv_timer_resume(poll);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	load_note();
}

void podcastnotes_open(long long feed_id, long long episode_id, bool from_player) {
	selected_feed = feed_id;
	selected_episode = episode_id;
	if (from_player) {
		player_sheet_close(false);
	}
	if (lv_screen_active() == screen) {
		load_note();
	} else {
		switch_screen(screen);
	}
	if (from_player) {
		switcher_set_player_return(screen);
	}
}

void podcastnotes_open_path(const char *path, bool from_player) {
	long long feed_id = 0;
	if (path && path[0] && podcastcache_feed_id(path, &feed_id)) {
		podcastnotes_open(feed_id, podcastcache_episode_id(path), from_player);
	} else {
		podcastnotes_open(0, 0, from_player);
	}
}

void podcastnotes_init(gui_config_t *cfg) {
	screen = lv_obj_create(NULL);
	lv_obj_add_style(screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_t *title = settingsrow_title(screen, cfg, "podcast_show_notes");
	settingsrow_title_corner_slots(title, cfg, 0);
	int top = settingsrow_content_top(cfg);

	page = lv_obj_create(screen);
	lv_obj_remove_style_all(page);
	lv_obj_set_size(page, lv_pct(100), cfg->screen_height - top);
	lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, top);
	lv_obj_set_style_pad_hor(page, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(page, 2 * cfg->padding, 0);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_event_bubble(page, true);

	label = lv_label_create(page);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_text_line_space(label, 4, 0);
	lv_label_set_recolor(label, false);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_label_set_text_static(label, "");

	poll = lv_timer_create(poll_cb, 100, NULL);
	if (poll) {
		lv_timer_pause(poll);
	}
	lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(screen);
}
