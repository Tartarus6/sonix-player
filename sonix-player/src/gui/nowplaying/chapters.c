#include "chapters.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/library/id3chap.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"

lv_obj_t *chapters_screen;

// Books run to dozens of chapters, occasionally a couple of hundred; the list
// is plainly rebuilt each time it opens, like the audiobook index itself. The
// cap is there for a pathological file, not for any real book.
#define MAX_ROWS 400
#define ROW_HEIGHT 68
#define ROW_RADIUS 12

static lv_obj_t *chapter_list;
static lv_obj_t *empty_label;
static int listed_count;
static int highlighted_row;
static bool listing_podcast;
static char podcast_file[512];
static id3chap_t *podcast_marks;
static int podcast_count;
static bool podcast_resolved;
static struct stat podcast_source;
static bool podcast_growing;
static uint64_t podcast_attempt;
static unsigned podcast_generation;

typedef struct {
	char path[sizeof(podcast_file)];
	unsigned generation;
	id3chap_t *marks;
	int count;
	bool done;
} podcast_chapter_job_t;

// Only the UI owns the job pointer. A worker publishes its result under the lock.
static pthread_mutex_t podcast_job_lock = PTHREAD_MUTEX_INITIALIZER;
static podcast_chapter_job_t *podcast_job;

// A folder book lists its files here instead: each file is a chapter.
static bool listing_parts;

static void format_clock(double seconds, char *out, size_t out_size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;

	if (hours > 0) {
		snprintf(out, out_size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, out_size, "%ld:%02ld", minutes, secs);
	}
}

static int compare_chapter_starts(const void *a, const void *b) {
	double first = ((const id3chap_t *)a)->start;
	double second = ((const id3chap_t *)b)->start;
	return (first > second) - (first < second);
}

bool chapters_podcast_supports(const char *path) {
	const char *ext = path ? strrchr(path, '.') : NULL;
	return ext && strcasecmp(ext, ".mp3") == 0;
}

static bool read_podcast_source(const char *path, struct stat *source) { return chapters_podcast_supports(path) && stat(path, source) == 0 && S_ISREG(source->st_mode); }

// Negative means the tag is not ready. Zero means there are no chapter marks.
static int read_podcast_marks(const char *path, id3chap_t **out) {
	if (id3chap_tag_pending(path)) {
		return -1;
	}
	id3chap_t *marks = calloc(MAX_ROWS, sizeof(*marks));
	if (!marks) {
		return -1;
	}
	int count = id3chap_read(path, marks, MAX_ROWS);
	if (count <= 0) {
		free(marks);
		return 0;
	}
	count = count < MAX_ROWS ? count : MAX_ROWS;
	qsort(marks, (size_t)count, sizeof(*marks), compare_chapter_starts);
	*out = marks;
	return count;
}

static void use_podcast_marks(id3chap_t *marks, int count) {
	free(podcast_marks);
	podcast_marks = marks;
	podcast_count = count > 0 ? count : 0;
	podcast_resolved = count >= 0;
}

static void select_podcast_source(const char *path) {
	use_podcast_marks(NULL, -1);
	podcast_generation++;
	snprintf(podcast_file, sizeof(podcast_file), "%s", path);
}

static bool same_podcast_source(const char *path, const struct stat *source, uint64_t attempt, bool growing) {
	if (strcmp(path, podcast_file) != 0 || attempt != podcast_attempt || source->st_dev != podcast_source.st_dev || source->st_ino != podcast_source.st_ino || source->st_size < podcast_source.st_size) {
		return false;
	}
	// Audio added to an existing stream does not change its leading tag.
	if (source->st_size > podcast_source.st_size) {
		return growing || podcast_growing;
	}
	return source->st_mtim.tv_sec == podcast_source.st_mtim.tv_sec && source->st_mtim.tv_nsec == podcast_source.st_mtim.tv_nsec;
}

static void *read_podcast_chapters(void *arg) {
	podcast_chapter_job_t *job = arg;
	thread_be_background("podcast chapters");
	job->count = read_podcast_marks(job->path, &job->marks);
	pthread_mutex_lock(&podcast_job_lock);
	job->done = true;
	pthread_mutex_unlock(&podcast_job_lock);
	return NULL;
}

static void take_podcast_chapters(void) {
	pthread_mutex_lock(&podcast_job_lock);
	podcast_chapter_job_t *finished = NULL;
	if (podcast_job && podcast_job->done) {
		finished = podcast_job;
		podcast_job = NULL;
	}
	pthread_mutex_unlock(&podcast_job_lock);
	if (!finished) {
		return;
	}
	if (finished->generation == podcast_generation && finished->count >= 0) {
		use_podcast_marks(finished->marks, finished->count);
		finished->marks = NULL;
	}
	free(finished->marks);
	free(finished);
}

static void start_podcast_chapters(void) {
	podcast_chapter_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	snprintf(job->path, sizeof(job->path), "%s", podcast_file);
	job->generation = podcast_generation;
	pthread_t thread;
	int error = pthread_create(&thread, NULL, read_podcast_chapters, job);
	if (error) {
		fprintf(stderr, "podcast chapters: cannot start reader: %s\n", strerror(error));
		free(job);
		return;
	}
	podcast_job = job;
	pthread_detach(thread);
}

void chapters_podcast_update(const char *path) {
	if (listing_podcast && lv_screen_active() == chapters_screen) {
		return; // the visible rows still refer to these marks
	}
	struct stat source;
	if (read_podcast_source(path, &source)) {
		bool growing;
		uint64_t attempt = growfile_attempt(path, &growing);
		if (!same_podcast_source(path, &source, attempt, growing)) {
			select_podcast_source(path);
		}
		podcast_source = source;
		podcast_growing = growing;
		podcast_attempt = attempt;
	} else if (podcast_file[0]) {
		select_podcast_source("");
	}
	take_podcast_chapters();
	if (podcast_file[0] && !podcast_resolved && !podcast_job) {
		start_podcast_chapters();
	}
}

// Check on menu open even if the worker has not finished. A retry can
// replace a download at the same path after the title was cached.
bool chapters_podcast_available(const char *path) {
	if (!path || !path[0]) {
		return false;
	}
	if (listing_podcast && lv_screen_active() == chapters_screen) {
		return podcast_count > 0 && strcmp(path, podcast_file) == 0;
	}
	struct stat source;
	if (!read_podcast_source(path, &source)) {
		select_podcast_source("");
		return false;
	}
	select_podcast_source(path);
	podcast_source = source;
	podcast_attempt = growfile_attempt(path, &podcast_growing);
	id3chap_t *marks = NULL;
	int count = read_podcast_marks(path, &marks);
	use_podcast_marks(marks, count);
	return podcast_count > 0;
}

static int podcast_chapter_at(const char *path, double seconds) {
	if (strcmp(path, podcast_file) == 0) {
		for (int i = podcast_count - 1; i >= 0; i--) {
			if (podcast_marks[i].start <= seconds + 0.001) {
				return i;
			}
		}
	}
	return -1;
}

const char *chapters_podcast_title(const char *path, double seconds) {
	int index = podcast_chapter_at(path, seconds);
	return index < 0 ? "" : podcast_marks[index].title;
}

static void chapter_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (listing_podcast) {
		device_state_t state;
		device_state_get(&state);
		if (strcmp(state.current_file, podcast_file) != 0 || index < 0 || index >= podcast_count) {
			return;
		}
		device_state_seek_to_chapter(podcast_marks[index].start);
		// Same episode. A full refresh would reset the clocks while the seek waits.
		back_btn_cb(NULL);
		return;
	} else {
		double start = 0;
		if (!audiobook_chapter(index, NULL, 0, &start)) {
			return;
		}
		device_state_seek(start);
		// A book remembers the chosen position even if the player is switched off immediately.
		audiobook_note_position(start, 0, true);
	}

	player_refresh_now_playing();
	back_btn_cb(NULL); // back to the player, which is where the jump is heard
}

// Starts part `index` of the folder book from its beginning, with the book's
// parts as the queue again, whatever the queue had become.
static void part_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	int count = audiobook_part_count();
	if (index < 0 || index >= count) {
		return;
	}
	char **paths = calloc((size_t)count, sizeof(char *));
	if (!paths) {
		return;
	}
	int filled = 0;
	for (int i = 0; i < count; i++) {
		char path[512];
		if (!audiobook_part(i, NULL, 0, path, sizeof(path)) || !(paths[filled] = strdup(path))) {
			break;
		}
		filled++;
	}
	if (filled == count) {
		device_state_play_list_ordered((const char *const *)paths, count, index);
	}
	for (int i = 0; i < filled; i++) {
		free(paths[i]);
	}
	free(paths);

	player_refresh_now_playing();
	back_btn_cb(NULL);
}

static void highlight(lv_obj_t *row, bool playing) {
	for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
		lv_obj_t *label = lv_obj_get_child(row, (int32_t)i);
		if (playing) {
			lv_obj_set_style_text_color(label, theme()->accent, 0);
		} else {
			lv_obj_remove_local_style_prop(label, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
		}
	}
}

static lv_obj_t *make_row(int i, const char *title, const char *clock, bool playing, lv_event_cb_t cb) {
	lv_obj_t *row = lv_btn_create(chapter_list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 14, 0);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_set_event_bubble(row, true);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *number = lv_label_create(row);
	lv_label_set_text_fmt(number, "%d", i + 1);
	lv_obj_set_width(number, 44);
	lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_add_style(number, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(number, &font_ui_24, 0);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, title[0] ? title : tr("chapter"));
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	// One line, then dots. Without a height the label wraps instead, and a
	// long chapter name then grows past the fixed row it lives in.
	lv_obj_set_height(label, 32);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	if (clock) {
		lv_obj_t *time_label = lv_label_create(row);
		lv_label_set_text(time_label, clock);
		lv_obj_add_style(time_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(time_label, &font_ui_18, 0);
	}

	// The chapter being listened to is marked the way the queue marks the
	// playing track: the accent on the words, not a second widget.
	if (playing) {
		highlight(row, true);
	}
	return row;
}

// The row to open the list on: the chapter, or the part, being listened to.
static int playing_row(void) {
	if (listing_parts) {
		return audiobook_part_current();
	}
	device_state_t state;
	device_state_get(&state);
	if (!listing_podcast) {
		return audiobook_chapter_at(state.progress_current_secs);
	}
	return podcast_chapter_at(state.current_file, state.progress_current_secs);
}

static void refresh_podcast_highlight(lv_timer_t *timer) {
	(void)timer;
	if (!listing_podcast || lv_screen_active() != chapters_screen) {
		return;
	}
	int playing = playing_row();
	if (playing == highlighted_row) {
		return;
	}
	if (highlighted_row >= 0 && highlighted_row < listed_count) {
		highlight(lv_obj_get_child(chapter_list, highlighted_row), false);
	}
	if (playing >= 0 && playing < listed_count) {
		highlight(lv_obj_get_child(chapter_list, playing), true);
	}
	highlighted_row = playing;
}

static void rebuild(void) {
	lv_obj_clean(chapter_list);
	empty_label = NULL;

	listing_parts = !listing_podcast && audiobook_part_count() > 1;
	if (listing_parts) {
		int count = audiobook_part_count();
		if (count > MAX_ROWS) {
			count = MAX_ROWS;
		}
		listed_count = count;
		int playing = playing_row();
		for (int i = 0; i < count; i++) {
			char title[192] = "";
			if (!audiobook_part(i, title, sizeof(title), NULL, 0)) {
				break;
			}
			make_row(i, title, NULL, i == playing, part_clicked_cb);
		}
		return;
	}

	int count = listing_podcast ? podcast_count : audiobook_chapter_count();
	if (count > MAX_ROWS) {
		count = MAX_ROWS;
	}
	listed_count = count;

	if (count == 0) {
		empty_label = lv_label_create(chapter_list);
		lv_label_set_text(empty_label, tr("chapters_empty"));
		lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty_label, lv_pct(100));
		lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
		lv_obj_set_style_pad_top(empty_label, 80, 0);
		return;
	}

	highlighted_row = playing_row();
	for (int i = 0; i < count; i++) {
		char title[192] = "";
		double start = 0;
		if (listing_podcast) {
			snprintf(title, sizeof(title), "%s", podcast_marks[i].title);
			start = podcast_marks[i].start;
		} else if (!audiobook_chapter(i, title, sizeof(title), &start)) {
			break;
		}

		char clock[24];
		format_clock(start, clock, sizeof(clock));
		make_row(i, title, clock, i == highlighted_row, chapter_clicked_cb);
	}
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	rebuild();

	// Open on the chapter being listened to rather than at the top: on a book
	// with sixty chapters, the one that matters is the one playing.
	int playing = playing_row();
	if (playing > 0 && playing < listed_count) {
		lv_obj_t *row = lv_obj_get_child(chapter_list, playing);
		if (row) {
			lv_obj_scroll_to_view(row, LV_ANIM_OFF);
		}
	}
}

static void open_list(void) {
	player_sheet_close(false);
	switch_screen(chapters_screen);
	switcher_set_player_return(chapters_screen);
}

void chapters_open(void) {
	listing_podcast = false;
	open_list();
}

void chapters_open_podcast(const char *menu_path) {
	device_state_t state;
	device_state_get(&state);
	if (menu_path && podcast_count > 0 && strcmp(menu_path, state.current_file) == 0 && strcmp(menu_path, podcast_file) == 0) {
		listing_podcast = true;
		open_list();
	}
}

void chapters_init(gui_config_t *cfg) {
	lv_obj_add_style(chapters_screen, &theme_style_screen, 0);
	settingsrow_title(chapters_screen, cfg, "chapters");

	int content_top = settingsrow_content_top(cfg);

	chapter_list = lv_obj_create(chapters_screen);
	lv_obj_set_size(chapter_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(chapter_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(chapter_list, 0, 0);
	lv_obj_set_style_border_width(chapter_list, 0, 0);
	lv_obj_set_style_radius(chapter_list, 0, 0);
	lv_obj_set_style_pad_hor(chapter_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(chapter_list, 0, 0);
	lv_obj_set_style_pad_gap(chapter_list, 8, 0);
	lv_obj_set_scroll_dir(chapter_list, LV_DIR_VER);
	lv_obj_set_flex_flow(chapter_list, LV_FLEX_FLOW_COLUMN);
	// Set here, not in the row builder: a book with no chapters creates no
	// rows, and the page still has to answer the swipe-back.
	lv_obj_set_event_bubble(chapter_list, true);

	// No player-sheet swipe here, for the queue page's reason: it would slide
	// the sheet over the list being read. The swipe-back still works.
	switcher_attach_back_gesture(chapter_list);
	lv_obj_add_event_cb(chapters_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_timer_create(refresh_podcast_highlight, 500, NULL);
}
