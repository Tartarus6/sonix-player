#include "podmenu.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/pod_home.h"
#include "src/gui/shell/pod_layout.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *podmenu_screen;

// ---------------------------------------------------------------------------
// Visual constants  (mirror musictiles.c to keep the two editors consistent)
// ---------------------------------------------------------------------------

#define ROW_HEIGHT   72
#define ROW_GAP       8
#define ROW_PITCH    (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS   12
#define ROW_PAD      20
#define ICON_BOX     48
#define ICON_SCALE   (LV_SCALE_NONE / 2)
#define HEADING_H    40
#define GRIP_SIZE    48
#define GRIP_SPACE   64
#define SWITCH_W     68

#define DROP_MIN_TRAVEL 8
#define EDGE_PX   70
#define EDGE_STEP 18

// ---------------------------------------------------------------------------
// Row model
//
// The editor shows two sections on one scrollable canvas:
//
//   "Home screen" — one fixed row per slot: Grid 1–9 then Dock 1–3. Empty
//                   slots are shown too, so every slot is a drop target
//                   (Requirements 3.1, 3.2, 3.5).
//
//   "Not on home screen" — every shortcut that is not in any slot, whether
//                   hidden or visible-without-a-free-slot. Together with the
//                   slot rows this lists all POD_APP_COUNT shortcuts
//                   (Requirement 5.3 / Property 10).
//
// Dragging a row's grip onto a slot row moves the shortcut there; whatever
// was in the target slot goes to the source slot (swap), or to the
// "Not on home screen" section when the source was not a slot.
// ---------------------------------------------------------------------------

#define SLOT_ROWS   (POD_GRID_SLOTS + POD_DOCK_SLOTS)
#define ENTRY_COUNT (SLOT_ROWS + POD_APP_COUNT)

typedef struct {
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *slot_label;
    lv_obj_t *name;
    lv_obj_t *toggle;
    lv_obj_t *grip;
} entry_t;

static lv_obj_t *page;
static lv_obj_t *body;
static lv_obj_t *slots_heading;
static lv_obj_t *other_heading;
static entry_t   entries[ENTRY_COUNT];

// App shown by each row. For slot rows (index < SLOT_ROWS) POD_APP_NONE means
// the slot is empty.
static pod_app_t row_app[ENTRY_COUNT];
static int       row_count;

static int        drag_row   = -1;
static int        hover_slot = -1;
static bool       drag_moved;
static lv_point_t drag_from;

// ---------------------------------------------------------------------------
// Slot helpers  (slot 0–8 = grid, 9–11 = dock)
// ---------------------------------------------------------------------------

static pod_app_t slot_get(int s) {
    if (s < POD_GRID_SLOTS) {
        return pod_layout_grid_at(s);
    }
    return pod_layout_dock_at(s - POD_GRID_SLOTS);
}

static void slot_put(int s, pod_app_t app) {
    if (s < POD_GRID_SLOTS) {
        pod_layout_grid_set(app, s);
    } else {
        pod_layout_dock_set(app, s - POD_GRID_SLOTS);
    }
}

static bool app_placed(pod_app_t app) {
    for (int s = 0; s < SLOT_ROWS; s++) {
        if (slot_get(s) == app) {
            return true;
        }
    }
    return false;
}

static void slot_text(int s, char *buf, size_t len) {
    if (s < POD_GRID_SLOTS) {
        snprintf(buf, len, "%s %d", tr("podmenu_grid"), s + 1);
    } else {
        snprintf(buf, len, "%s %d", tr("podmenu_dock"), s - POD_GRID_SLOTS + 1);
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static int slots_top(void)      { return HEADING_H; }
static int other_heading_y(void) { return slots_top() + SLOT_ROWS * ROW_PITCH + 8; }
static int other_top(void)      { return other_heading_y() + HEADING_H; }

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

static void refresh(void);
static void refresh_async(void *unused) { (void)unused; refresh(); }

static void refresh(void) {
    row_count = 0;
    for (int s = 0; s < SLOT_ROWS; s++) {
        row_app[row_count++] = slot_get(s);
    }
    for (int a = 0; a < POD_APP_COUNT; a++) {
        if (!app_placed((pod_app_t)a)) {
            row_app[row_count++] = (pod_app_t)a;
        }
    }

    for (int i = 0; i < ENTRY_COUNT; i++) {
        entry_t *en = &entries[i];
        if (i >= row_count) {
            lv_obj_set_hidden(en->row, true);
            continue;
        }
        lv_obj_set_hidden(en->row, false);

        bool      is_slot = i < SLOT_ROWS;
        pod_app_t app     = row_app[i];
        bool      empty   = app == POD_APP_NONE;

        int y = is_slot ? slots_top() + i * ROW_PITCH
                        : other_top() + (i - SLOT_ROWS) * ROW_PITCH;
        lv_obj_set_y(en->row, y);

        char buf[32];
        if (is_slot) {
            slot_text(i, buf, sizeof(buf));
        } else if (pod_layout_visible(app)) {
            snprintf(buf, sizeof(buf), "%s", tr("podmenu_unassigned"));
        } else {
            buf[0] = '\0';
        }
        lv_label_set_text(en->slot_label, buf);

        lv_obj_set_hidden(en->icon, empty);
        if (!empty) {
            lv_image_set_src(en->icon, pod_app_icon(app));
        }
        lv_label_set_text(en->name, empty ? tr("podmenu_empty")
                                          : tr(pod_app_label(app)));
        if (empty) {
            lv_obj_add_style(en->name, &theme_style_text_dim, 0);
        } else {
            lv_obj_remove_style(en->name, &theme_style_text_dim, 0);
        }

        lv_obj_set_user_data(en->toggle, (void *)(intptr_t)app);
        lv_obj_set_hidden(en->toggle, empty);
        if (!empty && pod_layout_visible(app)) {
            lv_obj_add_state(en->toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(en->toggle, LV_STATE_CHECKED);
        }

        // Only visible shortcuts can be dragged; hidden ones must be switched
        // on first.
        lv_obj_set_hidden(en->grip, empty || !pod_layout_visible(app));
    }

    lv_obj_set_y(other_heading, other_heading_y() + 10);
    lv_obj_set_hidden(other_heading, row_count == SLOT_ROWS);

    int other_rows = row_count - SLOT_ROWS;
    lv_obj_set_height(body, other_top() + other_rows * ROW_PITCH + 8);
}

// ---------------------------------------------------------------------------
// Drag and drop
// ---------------------------------------------------------------------------

// Slot row under the finger, or -1.
static int slot_at(lv_point_t point) {
    lv_area_t area;
    lv_obj_get_coords(body, &area);
    int y = point.y - area.y1 - slots_top();
    if (y < 0) {
        return -1;
    }
    int s = y / ROW_PITCH;
    return s < SLOT_ROWS ? s : -1;
}

static void row_outline(int index, bool on) {
    if (index < 0 || index >= ENTRY_COUNT) {
        return;
    }
    lv_obj_t *row = entries[index].row;
    lv_obj_set_style_border_width(row, on ? 2 : 0, 0);
    if (on) {
        lv_obj_set_style_border_color(row, theme()->accent, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    }
}

static void set_hover(int s) {
    if (s == hover_slot) {
        return;
    }
    if (hover_slot >= 0 && hover_slot != drag_row) {
        row_outline(hover_slot, false);
    }
    hover_slot = s;
    if (hover_slot >= 0) {
        row_outline(hover_slot, true);
    }
}

// Moves the shortcut in row `from` to slot `to`.
static void do_drop(int from, int to) {
    if (to < 0 || to >= SLOT_ROWS || from == to) {
        return;
    }
    pod_app_t moving = row_app[from];
    if (moving == POD_APP_NONE) {
        return;
    }
    pod_app_t displaced = slot_get(to);

    if (from < SLOT_ROWS) {
        // Swap between two slots. Empty the source first so neither app is
        // ever in two slots at once.
        slot_put(from, POD_APP_NONE);
        slot_put(to, moving);
        if (displaced != POD_APP_NONE) {
            slot_put(from, displaced);
        }
    } else {
        // From "Not on home screen": the displaced app (if any) leaves the
        // home screen but stays visible.
        slot_put(to, moving);
    }

    pod_layout_save();
    pod_home_refresh();
}

static void edge_scroll(lv_point_t point) {
    lv_area_t view;
    lv_obj_get_coords(page, &view);
    int step = 0;
    if (point.y < view.y1 + EDGE_PX) {
        step = -EDGE_STEP;
    } else if (point.y > view.y2 - EDGE_PX) {
        step = EDGE_STEP;
    }
    if (step == 0) {
        return;
    }
    int scroll = lv_obj_get_scroll_y(page);
    int limit  = scroll + lv_obj_get_scroll_bottom(page);
    int wanted = scroll + step;
    if (wanted < 0) wanted = 0;
    if (wanted > limit) wanted = limit;
    if (wanted != scroll) {
        lv_obj_scroll_to_y(page, wanted, LV_ANIM_OFF);
    }
}

static void drag_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t       *grip = lv_event_get_current_target(e);

    lv_indev_t *indev = lv_indev_active();
    lv_point_t  point = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &point);
    }

    if (code == LV_EVENT_PRESSED) {
        drag_row = -1;
        for (int i = 0; i < row_count; i++) {
            if (entries[i].grip == grip) {
                drag_row = i;
                break;
            }
        }
        if (drag_row < 0) {
            return;
        }
        drag_moved = false;
        drag_from  = point;
        hover_slot = -1;
        row_outline(drag_row, true);
        return;
    }

    if (drag_row < 0) {
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        edge_scroll(point);
        if (LV_ABS(point.y - drag_from.y) > DROP_MIN_TRAVEL ||
            LV_ABS(point.x - drag_from.x) > DROP_MIN_TRAVEL) {
            drag_moved = true;
        }
        if (drag_moved) {
            set_hover(slot_at(point));
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int from = drag_row;
        drag_row = -1;
        set_hover(-1);
        row_outline(from, false);

        if (code == LV_EVENT_RELEASED && drag_moved) {
            do_drop(from, slot_at(point));
        }
        // Defer the repaint so the event handler unwinds first (same reasoning
        // as musictiles.c).
        lv_async_call(refresh_async, NULL);
    }
}

// ---------------------------------------------------------------------------
// Visibility toggle
// ---------------------------------------------------------------------------

static void toggle_cb(lv_event_t *e) {
    lv_obj_t *sw  = lv_event_get_target(e);
    pod_app_t app = (pod_app_t)(intptr_t)lv_obj_get_user_data(sw);
    if (app < 0 || app >= POD_APP_COUNT) {
        return;
    }
    pod_layout_set_visible(app, lv_obj_has_state(sw, LV_STATE_CHECKED));
    pod_layout_save();
    pod_home_refresh();
    lv_async_call(refresh_async, NULL);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

static lv_obj_t *make_heading(lv_obj_t *parent, const char *tag, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, tr(tag));
    lv_obj_add_style(label, &theme_style_text_dim, 0);
    lv_obj_set_style_text_font(label, &font_ui_22, 0);
    lv_obj_set_pos(label, 4, y + 10);
    return label;
}

// Row layout (left to right):
//   [icon] [slot label over name] [toggle] [grip]
static void make_row(entry_t *entry, int width) {
    lv_obj_t *row = lv_obj_create(body);
    lv_obj_set_size(row, width, ROW_HEIGHT);
    lv_obj_set_x(row, 0);
    lv_obj_add_style(row, &theme_style_card, 0);
    lv_obj_set_style_radius(row, ROW_RADIUS, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, ROW_PAD, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    entry->row = row;

    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, ICON_BOX, ROW_HEIGHT);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_clickable(box, false);

    // The app artwork keeps its colours, as in musictiles.c.
    entry->icon = lv_image_create(box);
    lv_image_set_scale(entry->icon, ICON_SCALE);
    lv_obj_center(entry->icon);

    int right_reserved = SWITCH_W + 8 + GRIP_SPACE;
    int text_x         = ICON_BOX + 16;
    int text_width     = width - 2 * ROW_PAD - ICON_BOX - 16 - right_reserved;

    lv_obj_t *text_col = lv_obj_create(row);
    lv_obj_remove_style_all(text_col);
    lv_obj_set_size(text_col, text_width, ROW_HEIGHT);
    lv_obj_align(text_col, LV_ALIGN_LEFT_MID, text_x, 0);
    lv_obj_set_clickable(text_col, false);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(text_col, 2, 0);

    entry->slot_label = lv_label_create(text_col);
    lv_obj_add_style(entry->slot_label, &theme_style_text_dim, 0);
    lv_obj_set_style_text_font(entry->slot_label, &font_ui_20, 0);
    lv_label_set_long_mode(entry->slot_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(entry->slot_label, text_width);

    entry->name = lv_label_create(text_col);
    lv_obj_add_style(entry->name, &theme_style_text, 0);
    lv_obj_set_style_text_font(entry->name, &font_ui_24, 0);
    lv_label_set_long_mode(entry->name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(entry->name, text_width);

    lv_obj_t *sw = settingsrow_switch(row, toggle_cb);
    lv_obj_set_user_data(sw, (void *)(intptr_t)POD_APP_NONE);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -(GRIP_SPACE + 8), 0);
    entry->toggle = sw;

    lv_obj_t *grip = lv_obj_create(row);
    lv_obj_set_size(grip, GRIP_SIZE, GRIP_SIZE);
    lv_obj_align(grip, LV_ALIGN_RIGHT_MID,
                 ROW_PAD - (GRIP_SPACE - GRIP_SIZE) / 2, 0);
    lv_obj_set_style_bg_opa(grip, 0, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_set_style_pad_all(grip, 0, 0);
    lv_obj_set_scrollable(grip, false);
    lv_obj_set_scroll_chain_ver(grip, false);
    lv_obj_set_scroll_chain_hor(grip, false);
    lv_obj_set_clickable(grip, true);

    lv_obj_t *grip_icon = lv_image_create(grip);
    lv_image_set_src(grip_icon, &icon_grip);
    lv_obj_add_style(grip_icon, &theme_style_icon, 0);
    lv_obj_set_style_image_recolor_opa(grip_icon, LV_OPA_COVER, 0);
    lv_obj_center(grip_icon);

    lv_obj_add_event_cb(grip, drag_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(grip, drag_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(grip, drag_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(grip, drag_cb, LV_EVENT_PRESS_LOST, NULL);
    entry->grip = grip;
}

static void loaded_cb(lv_event_t *e) {
    (void)e;
    refresh();
}

void podmenu_init(gui_config_t *cfg) {
    podmenu_screen = lv_obj_create(NULL);
    lv_obj_add_style(podmenu_screen, &theme_style_screen, 0);

    page = settingsrow_page(podmenu_screen, cfg, "appearance_home_layout");
    settingsrow_title_corner_slots(settingsrow_page_title(podmenu_screen), cfg, 0);

    int width = (int)cfg->screen_width - 2 * cfg->padding;

    body = lv_obj_create(page);
    lv_obj_set_width(body, width);
    lv_obj_set_height(body, ROW_PITCH);
    lv_obj_set_style_bg_opa(body, 0, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_scrollable(body, false);

    slots_heading = make_heading(body, "podmenu_slots", 0);
    other_heading = make_heading(body, "podmenu_other", 0);

    for (int i = 0; i < ENTRY_COUNT; i++) {
        make_row(&entries[i], width);
    }

    lv_obj_add_event_cb(podmenu_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    switcher_attach_back_gesture(podmenu_screen);
    refresh();
}
