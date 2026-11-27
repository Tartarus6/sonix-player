#include "lastfmsettings.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/lastfm/lastfm.h"
#include "src/system/net/wifi.h"

#define TICK_MS 1000
#define QR_MAX_SIZE 200
#define QR_QUIET 10

static lv_obj_t *page_screen;
static lv_obj_t *enabled_switch;
static lv_obj_t *account_value;
static lv_obj_t *status_value;
static lv_obj_t *queued_value;
static lv_obj_t *note_label;

static lv_obj_t *login_screen;
static lv_obj_t *login_message;
static lv_obj_t *login_qr;
static lv_obj_t *login_url;
static lv_obj_t *login_countdown;
static lv_obj_t *login_actions;
static lv_obj_t *login_done;
static lv_obj_t *login_retry;

static unsigned seen_login_serial;
static unsigned seen_auth_serial;
static bool waiting_for_login;

lv_obj_t *lastfmsettings_screen(void) { return page_screen; }

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void refresh(void) {
	lastfm_status_t st;
	lastfm_get_status(&st);

	if (st.enabled) {
		lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	}
	if (st.state == LASTFM_STATE_UNAVAILABLE) {
		lv_obj_add_state(enabled_switch, LV_STATE_DISABLED);
	}

	lv_label_set_text(account_value, st.user[0] ? st.user : tr("lastfm_not_signed_in"));

	const char *status = "";
	switch (st.state) {
	case LASTFM_STATE_UNAVAILABLE:
		status = tr("lastfm_unavailable");
		break;
	case LASTFM_STATE_OFF:
		status = tr("lastfm_state_off");
		break;
	case LASTFM_STATE_SIGNED_OUT:
		status = tr("lastfm_not_signed_in");
		break;
	case LASTFM_STATE_SIGNING_IN:
		status = tr("lastfm_state_signing_in");
		break;
	case LASTFM_STATE_AWAITING_AUTH:
		status = tr("lastfm_auth_waiting");
		break;
	case LASTFM_STATE_CONNECTED:
		status = tr("connected");
		break;
	case LASTFM_STATE_SESSION_EXPIRED:
		status = tr("lastfm_state_session_expired");
		break;
	case LASTFM_STATE_REFUSED:
		status = st.detail;
		break;
	}
	lv_label_set_text(status_value, status);
	lv_label_set_text(note_label, tr(st.state == LASTFM_STATE_UNAVAILABLE ? "api_keys_unavailable" : "lastfm_note"));
	lv_label_set_text_fmt(queued_value, "%d", st.queued);
}

// ---------------------------------------------------------------------------
// signing in with a token
// ---------------------------------------------------------------------------

static void set_visible(lv_obj_t *obj, bool visible) { lv_obj_set_hidden(obj, !visible); }

// The QR is the URL; the written URL stays as a fallback when the encoder
// cannot fit it, so a login flow never ends as a dead end.
static void show_auth_url(const char *url) {
	bool qr_ok = url && url[0] && lv_qrcode_update(login_qr, url, (uint32_t)strlen(url)) == LV_RESULT_OK;
	set_visible(login_qr, qr_ok);
	set_visible(login_url, !qr_ok);
	if (qr_ok) {
		lv_label_set_text(login_url, "");
	} else {
		lv_label_set_text(login_url, url ? url : "");
	}
	lv_label_set_text(login_message, url && url[0] ? tr("lastfm_auth_note") : tr("lastfm_no_reply"));
	set_visible(login_done, url && url[0]);
}

static void update_countdown(int seconds) {
	if (seconds > 0) {
		lv_label_set_text_fmt(login_countdown, "%s (%d:%02d)", tr("lastfm_auth_waiting"), seconds / 60, seconds % 60);
	} else {
		lv_label_set_text(login_countdown, tr("lastfm_auth_expired"));
	}
}

static bool wifi_ok(void) {
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	return wifi.state == WIFI_STATE_CONNECTED && wifi.ip[0];
}

static void open_login(void) {
	if (!wifi_ok()) {
		toast_error("enable_wi_fi_first");
		return;
	}

	lastfm_status_t st;
	lastfm_get_status(&st);
	seen_auth_serial = st.auth_serial;
	show_auth_url(st.auth_url);
	if (st.state == LASTFM_STATE_AWAITING_AUTH) {
		update_countdown(st.auth_seconds_left);
		// This token can still expire while the screen is open.
		waiting_for_login = true;
	} else {
		lv_label_set_text(login_countdown, "");
	}

	if (st.state != LASTFM_STATE_AWAITING_AUTH || st.auth_seconds_left <= 0) {
		// Ignore a result published by the attempt we are replacing.
		seen_login_serial = st.login_serial;
		waiting_for_login = true;
		toast_busy("lastfm_state_signing_in");
		lastfm_begin_login();
	}
	switch_screen(login_screen);
}

static void enabled_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enabled_switch, LV_STATE_CHECKED);
	lastfm_set_enabled(on);
	lastfm_status_t st;
	lastfm_get_status(&st);
	// Switched on with no account behind it: the sign-in is the next step.
	if (on && !st.user[0] && st.state != LASTFM_STATE_SIGNING_IN && st.state != LASTFM_STATE_AWAITING_AUTH) {
		open_login();
	}
	refresh();
}

static void do_logout(void *user) {
	(void)user;
	lastfm_logout();
	toast_success("signed_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	lastfm_status_t st;
	lastfm_get_status(&st);
	if (st.state == LASTFM_STATE_UNAVAILABLE) {
		toast_error("api_keys_unavailable");
		return;
	}
	if (st.state == LASTFM_STATE_SIGNING_IN) {
		return;
	}
	if (st.user[0]) {
		char message[160];
		snprintf(message, sizeof(message), tr("sign_out_confirm_note"), "Last.fm");
		confirm_show("lastfm_sign_out", message, "leave", do_logout, NULL);
		return;
	}
	open_login();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *label, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 24, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *text = lv_label_create(btn);
	lv_label_set_text(text, tr(label));
	lv_obj_set_style_text_font(text, &font_ui_20, 0);
	lv_obj_center(text);
	return btn;
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page_screen, cfg, "lastfm");

	settingsrow_toggle(container, "lastfm_scrobble", &enabled_switch, enabled_cb);
	settingsrow_add(container, "lastfm_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "lastfm_status", &status_value, NULL, NULL);
	settingsrow_add(container, "lastfm_queued", &queued_value, NULL, NULL);

	// A user name or a message from Last.fm can be longer than the room beside
	// the row's name: one line, cut with an ellipsis.
	lv_obj_t *values[] = {account_value, status_value};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(values[i], lv_pct(60), 0);
		lv_obj_set_height(values[i], lv_font_get_line_height(&font_ui_24));
	}

	note_label = lv_label_create(container);
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, lv_pct(100));
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_label_set_text(note_label, tr("lastfm_note"));

	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

static void done_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	// A Done after an error is a new attempt: arm the result watcher again,
	// otherwise the successful sign-in is saved but the GUI never hears it.
	waiting_for_login = true;
	toast_busy("lastfm_state_signing_in");
	lastfm_complete_login();
}

static void retry_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || !wifi_ok()) {
		if (!wifi_ok()) {
			toast_error("enable_wi_fi_first");
		}
		return;
	}
	lv_label_set_text(login_message, tr("lastfm_auth_note"));
	lastfm_status_t st;
	lastfm_get_status(&st);
	seen_login_serial = st.login_serial;
	waiting_for_login = true;
	toast_busy("lastfm_state_signing_in");
	lastfm_begin_login();
}

static void build_login(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "lastfm_sign_in");
	int top = settingsrow_content_top(cfg);

	lv_obj_t *box = lv_obj_create(login_screen);
	lv_obj_set_size(box, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_align(box, LV_ALIGN_TOP_MID, 0, top + 20);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_row(box, 18, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	login_message = lv_label_create(box);
	lv_label_set_long_mode(login_message, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_message, lv_pct(100));
	lv_obj_set_style_text_align(login_message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(login_message, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_message, &font_ui_20, 0);
	lv_label_set_text(login_message, tr("lastfm_auth_note"));

	// Fixed black on white with a quiet zone: a QR is an optical target, not
	// a themed decoration. The size follows the smaller of the panel and 200.
	int qr_size = cfg->screen_width - 2 * cfg->padding;
	if (qr_size > QR_MAX_SIZE) {
		qr_size = QR_MAX_SIZE;
	}
	login_qr = lv_qrcode_create(box);
	lv_qrcode_set_size(login_qr, qr_size);
	lv_qrcode_set_dark_color(login_qr, lv_color_black());
	lv_qrcode_set_light_color(login_qr, lv_color_white());
	lv_obj_set_style_border_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_border_width(login_qr, QR_QUIET, 0);
	lv_obj_set_style_bg_color(login_qr, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(login_qr, LV_OPA_COVER, 0);

	login_url = lv_label_create(box);
	lv_label_set_long_mode(login_url, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(login_url, lv_pct(100));
	lv_obj_set_style_text_align(login_url, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(login_url, &font_ui_18, 0);
	lv_label_set_text(login_url, "");

	login_countdown = lv_label_create(box);
	lv_obj_add_style(login_countdown, &theme_style_text, 0);
	lv_obj_set_style_text_font(login_countdown, &font_ui_20, 0);
	lv_label_set_text(login_countdown, "");

	// One group for the actions: a single Done sits centred, and when Retry
	// appears the pair shares the space with one even gap between them.
	login_actions = lv_obj_create(login_screen);
	lv_obj_set_size(login_actions, cfg->screen_width - 2 * cfg->padding, LV_SIZE_CONTENT);
	lv_obj_align(login_actions, LV_ALIGN_BOTTOM_MID, 0, -24);
	lv_obj_set_style_bg_opa(login_actions, 0, 0);
	lv_obj_set_style_border_width(login_actions, 0, 0);
	lv_obj_set_style_pad_all(login_actions, 0, 0);
	lv_obj_set_style_pad_gap(login_actions, 16, 0);
	lv_obj_set_scrollable(login_actions, false);
	lv_obj_set_flex_flow(login_actions, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(login_actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(login_actions, 0, 0);
	lv_obj_set_style_pad_bottom(login_actions, 0, 0);

	login_done = make_button(login_actions, "lastfm_auth_done", done_cb);
	login_retry = make_button(login_actions, "lastfm_auth_retry", retry_cb);
	set_visible(login_qr, false);
	set_visible(login_url, false);
	set_visible(login_retry, false);

	switcher_attach_back_gesture(login_screen);
}

static void login_finished(const lastfm_status_t *st) {
	if (!waiting_for_login) {
		return;
	}
	waiting_for_login = false;
	toast_busy_end();

	switch (st->login_result) {
	case LASTFM_LOGIN_OK:
		if (lv_screen_active() == login_screen) {
			back_btn_cb(NULL);
		}
		toast_success("signed_in");
		break;
	case LASTFM_LOGIN_NO_REPLY:
		toast_error("lastfm_no_reply");
		break;
	case LASTFM_LOGIN_EXPIRED:
		lv_label_set_text(login_message, tr("lastfm_auth_expired"));
		set_visible(login_qr, false);
		set_visible(login_url, false);
		set_visible(login_retry, true);
		break;
	case LASTFM_LOGIN_NOT_AUTHORIZED:
		lv_label_set_text(login_message, tr("lastfm_auth_not_authorized"));
		set_visible(login_retry, true);
		break;
	case LASTFM_LOGIN_REFUSED:
		toast_error(st->detail);
		break;
	}
}

// ---------------------------------------------------------------------------
// the timer: sign-in result, token and page refresh. What is playing is
// followed by src/system/library/playcount.c, not here.
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	(void)t;
	lastfm_status_t st;
	lastfm_get_status(&st);

	if (st.auth_serial != seen_auth_serial) {
		seen_auth_serial = st.auth_serial;
		show_auth_url(st.auth_url);
		set_visible(login_retry, st.auth_url[0] == '\0');
		if (st.auth_url[0]) {
			// The token is ready; the user, not the worker, is now the one
			// being waited on. Drop the "signing in" overlay.
			toast_busy_end();
		}
	}
	if (st.state == LASTFM_STATE_AWAITING_AUTH) {
		update_countdown(st.auth_seconds_left);
		if (st.auth_seconds_left <= 0) {
			set_visible(login_retry, true);
		}
	} else {
		lv_label_set_text(login_countdown, "");
	}
	if (st.login_serial != seen_login_serial) {
		seen_login_serial = st.login_serial;
		login_finished(&st);
	}
	if (lv_screen_active() == page_screen) {
		refresh();
	}
}

void lastfmsettings_init(gui_config_t *cfg) {
	build_page(cfg);
	build_login(cfg);

	lastfm_status_t st;
	lastfm_get_status(&st);
	seen_login_serial = st.login_serial;
	seen_auth_serial = st.auth_serial;
	refresh();

	lv_timer_create(tick_cb, TICK_MS, NULL);
}
