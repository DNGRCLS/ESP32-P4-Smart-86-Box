/* Coded Lock app: keypad, relay 1 (strike) + relay 2 (toggle), admin panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/lockcore.h"

typedef enum { ACT_UNLOCK = 0, ACT_RELAY2, ACT_ADMIN } lock_act_t;

static lv_obj_t *s_root;
static lv_obj_t *s_r1_img, *s_r1_state, *s_r1_name, *s_r1_sub, *s_r1_btn;
static lv_obj_t *s_r2_state, *s_r2_name, *s_r2_btn_lbl;
static lv_obj_t *s_prompt, *s_dots, *s_msg, *s_cancel_act;
static lv_timer_t *s_timer;
static char s_code[8];
static lock_act_t s_act;
static bool s_admin_mode;
static uint32_t s_admin_since;

static void build_main(void);
static void build_admin(void);

/* ------------------------------------------------------------------ helpers */

static void clear_root(void)
{
    lv_obj_clean(s_root);
    s_r1_img = s_r1_state = s_r1_name = s_r1_sub = s_r1_btn = NULL;
    s_r2_state = s_r2_name = s_r2_btn_lbl = NULL;
    s_prompt = s_dots = s_msg = s_cancel_act = NULL;
}

static void set_act(lock_act_t a)
{
    s_act = a;
    s_code[0] = 0;
    if (!s_prompt) return;
    switch (a) {
    case ACT_UNLOCK: lv_label_set_text_fmt(s_prompt, "Enter code to unlock %s", g_lock.relay1_name); break;
    case ACT_RELAY2:
        lv_label_set_text_fmt(s_prompt, "Enter code to switch %s %s", g_lock.relay2_name, g_lock.relay2_on ? "off" : "on");
        break;
    case ACT_ADMIN: lv_label_set_text(s_prompt, "Enter the 6-digit admin code"); break;
    }
    if (a == ACT_UNLOCK) lv_obj_add_flag(s_cancel_act, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_cancel_act, LV_OBJ_FLAG_HIDDEN);
}

static void update_dots(void)
{
    if (!s_dots) return;
    char buf[32] = "";
    int n = strlen(s_code);
    for (int i = 0; i < 6; i++) strcat(buf, i < n ? (i ? "  *" : "*") : (i ? "  _" : "_"));
    lv_label_set_text(s_dots, buf);
}

static void refresh(void)
{
    if (!s_r1_state) return;
    bool un = g_lock.relay1_unlocked;
    lv_image_set_src(s_r1_img, un ? &img_padlock_open : &img_padlock_closed);
    lv_obj_set_style_image_recolor(s_r1_img, un ? UI_C_OK : UI_C_ERR, 0);
    lv_label_set_text(s_r1_state, un ? "UNLOCKED" : "LOCKED");
    lv_obj_set_style_text_color(s_r1_state, un ? UI_C_OK : UI_C_ERR, 0);
    lv_label_set_text(s_r1_name, g_lock.relay1_name);
    int left = lock_relock_seconds_left();
    if (un && left) lv_label_set_text_fmt(s_r1_sub, "Locks again in %d s", left);
    else if (un) lv_label_set_text(s_r1_sub, "Stays unlocked until locked");
    else lv_label_set_text(s_r1_sub, "");
    if (un) lv_obj_remove_flag(s_r1_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_r1_btn, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_r2_name, g_lock.relay2_name);
    lv_label_set_text(s_r2_state, g_lock.relay2_on ? "ON" : "OFF");
    lv_obj_set_style_text_color(s_r2_state, g_lock.relay2_on ? UI_C_WARN : UI_C_MUTED, 0);
    lv_label_set_text_fmt(s_r2_btn_lbl, "Switch %s", g_lock.relay2_on ? "off" : "on");

    uint32_t lo;
    if (lock_locked_out(&lo)) {
        lv_label_set_text_fmt(s_msg, "Too many wrong codes - try again in %u s", (unsigned)lo);
        lv_obj_set_style_text_color(s_msg, UI_C_ERR, 0);
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_admin_mode && lv_display_get_inactive_time(NULL) > 120000) {
        s_admin_mode = false;
        build_main();
        return;
    }
    refresh();
}

/* ------------------------------------------------------------------ keypad */

static void submit(void)
{
    uint32_t lo;
    if (lock_locked_out(&lo)) { refresh(); s_code[0] = 0; update_dots(); return; }
    if (strlen(s_code) < 4) {
        lv_label_set_text(s_msg, "Codes have 4 to 6 digits");
        lv_obj_set_style_text_color(s_msg, UI_C_WARN, 0);
        return;
    }
    int who = lock_check(s_code, true);
    s_code[0] = 0;
    update_dots();
    if (who < 0) {
        if (!lock_locked_out(NULL)) {
            lv_label_set_text(s_msg, "Wrong code");
            lv_obj_set_style_text_color(s_msg, UI_C_ERR, 0);
        }
        refresh();
        return;
    }
    const char *name = who == LOCK_ADMIN ? "admin" : g_lock.users[who].label;
    lv_obj_set_style_text_color(s_msg, UI_C_OK, 0);
    switch (s_act) {
    case ACT_UNLOCK:
        lock_unlock(name);
        lv_label_set_text_fmt(s_msg, "Welcome, %s", name);
        break;
    case ACT_RELAY2:
        lock_relay2_set(!g_lock.relay2_on, name);
        lv_label_set_text_fmt(s_msg, "%s switched %s", g_lock.relay2_name, g_lock.relay2_on ? "on" : "off");
        set_act(ACT_UNLOCK);
        break;
    case ACT_ADMIN:
        if (who != LOCK_ADMIN) {
            lv_label_set_text(s_msg, "That is not the admin code");
            lv_obj_set_style_text_color(s_msg, UI_C_ERR, 0);
            return;
        }
        lock_log(LEV_ADMIN, "panel");
        s_admin_mode = true;
        s_admin_since = lv_tick_get();
        build_admin();
        return;
    }
    refresh();
}

static void key_cb(char k, void *u)
{
    (void)u;
    int n = strlen(s_code);
    if (k == '<') { if (n) s_code[n - 1] = 0; }
    else if (k == '#') { submit(); return; }
    else if (n < 6) { s_code[n] = k; s_code[n + 1] = 0; }
    if (s_msg && !lock_locked_out(NULL)) lv_label_set_text(s_msg, "");
    update_dots();
}

static void lock_now_cb(lv_event_t *e) { (void)e; lock_lock("panel", false); refresh(); }

static void r2_cb(lv_event_t *e)
{
    (void)e;
    if (!g_lock.relay2_needs_pin) {
        lock_relay2_set(!g_lock.relay2_on, "panel");
        refresh();
        return;
    }
    set_act(ACT_RELAY2);
    update_dots();
}

static void admin_cb(lv_event_t *e) { (void)e; set_act(ACT_ADMIN); update_dots(); }
static void cancel_act_cb(lv_event_t *e) { (void)e; set_act(ACT_UNLOCK); update_dots(); }

/* ------------------------------------------------------------------ main screen */

static lv_obj_t *panel(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, UI_C_SURFACE, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_radius(c, 20, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void build_main(void)
{
    clear_root();
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *left = lv_obj_create(s_root);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, 274, lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 12, 0);

    lv_obj_t *c1 = panel(left, 274, 352);
    lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c1, 4, 0);
    s_r1_name = lv_label_create(c1);
    lv_obj_set_style_text_font(s_r1_name, ui_font(UI_FW_MEDIUM, 24), 0);
    lv_obj_set_style_text_color(s_r1_name, UI_C_MUTED, 0);
    s_r1_img = lv_image_create(c1);
    lv_obj_set_style_image_recolor_opa(s_r1_img, LV_OPA_COVER, 0);
    s_r1_state = lv_label_create(c1);
    lv_obj_set_style_text_font(s_r1_state, ui_font(UI_FW_BOLD, 34), 0);
    s_r1_sub = lv_label_create(c1);
    lv_obj_set_style_text_font(s_r1_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_r1_sub, UI_C_MUTED, 0);
    s_r1_btn = ui_button(c1, LV_SYMBOL_CLOSE "  Lock now", UI_C_ERR);
    lv_obj_set_width(s_r1_btn, lv_pct(100));
    lv_obj_add_event_cb(s_r1_btn, lock_now_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *c2 = panel(left, 274, 154);
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c2, 6, 0);
    lv_obj_t *row = lv_obj_create(c2);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    s_r2_name = lv_label_create(row);
    lv_label_set_long_mode(s_r2_name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_r2_name, 170, 30);
    lv_obj_set_style_text_font(s_r2_name, ui_font(UI_FW_MEDIUM, 22), 0);
    lv_obj_set_style_text_color(s_r2_name, UI_C_MUTED, 0);
    s_r2_state = lv_label_create(row);
    lv_obj_set_style_text_font(s_r2_state, ui_font(UI_FW_BOLD, 24), 0);
    lv_obj_align(s_r2_state, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *b2 = ui_button(c2, "Switch", UI_C_SURFACE2);
    lv_obj_set_width(b2, lv_pct(100));
    s_r2_btn_lbl = lv_obj_get_child(b2, 0);
    lv_obj_add_event_cb(b2, r2_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ab = ui_button(left, LV_SYMBOL_SETTINGS "  Admin", UI_C_BG);
    lv_obj_set_width(ab, 274);
    lv_obj_set_height(ab, 52);
    lv_obj_set_style_border_width(ab, 1, 0);
    lv_obj_set_style_border_color(ab, UI_C_BORDER, 0);
    lv_obj_add_event_cb(ab, admin_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *right = panel(s_root, 396, 620);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(right, 8, 0);
    s_prompt = lv_label_create(right);
    lv_label_set_long_mode(s_prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_prompt, lv_pct(100));
    lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_prompt, ui_font(UI_FW_REGULAR, 22), 0);
    s_dots = lv_label_create(right);
    lv_obj_set_style_text_font(s_dots, ui_font(UI_FW_MEDIUM, 40), 0);
    lv_obj_set_style_text_color(s_dots, UI_C_ACCENT, 0);
    s_msg = lv_label_create(right);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg, lv_pct(100));
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_min_height(s_msg, 22, 0);
    lv_label_set_text(s_msg, "");
    ui_keypad_create(right, 360, 400, key_cb, NULL);
    s_cancel_act = ui_button(right, "Cancel", UI_C_SURFACE2);
    lv_obj_set_size(s_cancel_act, 360, 46);

    lv_obj_add_event_cb(s_cancel_act, cancel_act_cb, LV_EVENT_CLICKED, NULL);
    set_act(ACT_UNLOCK);
    update_dots();
    refresh();
}

/* ------------------------------------------------------------------ first-time setup */

static char s_new_code[8];
static int s_edit_user;            /* user index being edited, LOCK_ADMIN for the admin code */
static char s_new_label[24];

static void admin_setup_confirm(const char *code, void *u);

static void admin_setup_first(const char *code, void *u)
{
    (void)u;
    snprintf(s_new_code, sizeof(s_new_code), "%s", code);
    ui_pin_prompt("Confirm admin code", "Enter the same 6 digits again", 6, 6, admin_setup_confirm, NULL);
}

static void admin_setup_confirm(const char *code, void *u)
{
    (void)u;
    if (strcmp(code, s_new_code) != 0) {
        ui_toast("The codes did not match - try again");
        ui_pin_prompt("Create admin code", "Choose 6 digits. The admin code manages user codes and settings.", 6, 6,
                      admin_setup_first, NULL);
        return;
    }
    if (lock_code_in_use(code, LOCK_ADMIN)) {
        ui_toast("That code is already a user code - choose another");
        ui_pin_prompt("Create admin code", "Choose 6 digits", 6, 6, admin_setup_first, NULL);
        return;
    }
    lock_set_admin(code);
    ui_toast("Admin code saved. The user code is 1234 - change it in Admin.");
    if (s_root) build_main();
}

/* ------------------------------------------------------------------ admin panel */

static void admin_done_cb(lv_event_t *e) { (void)e; s_admin_mode = false; build_main(); }

static void code_second(const char *code, void *u);

static void code_first(const char *code, void *u)
{
    (void)u;
    int minl = s_edit_user == LOCK_ADMIN ? 6 : 4;
    if (lock_code_in_use(code, s_edit_user)) { ui_toast("That code is already in use"); return; }
    snprintf(s_new_code, sizeof(s_new_code), "%s", code);
    ui_pin_prompt("Confirm code", "Enter the same code again", minl, 6, code_second, NULL);
}

static void code_second(const char *code, void *u)
{
    (void)u;
    if (strcmp(code, s_new_code) != 0) { ui_toast("The codes did not match - nothing changed"); return; }
    if (s_edit_user == LOCK_ADMIN) lock_set_admin(code);
    else lock_set_user(s_edit_user, s_new_label, code);
    ui_toast("Code saved");
    build_admin();
}

static void ask_code(int user, const char *label)
{
    s_edit_user = user;
    snprintf(s_new_label, sizeof(s_new_label), "%s", label ? label : "");
    if (user == LOCK_ADMIN) ui_pin_prompt("New admin code", "Exactly 6 digits", 6, 6, code_first, NULL);
    else {
        char sub[64];
        snprintf(sub, sizeof(sub), "Code for %s: 4 to 6 digits", s_new_label[0] ? s_new_label : "this user");
        ui_pin_prompt("New user code", sub, 4, 6, code_first, NULL);
    }
}

static void user_change_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ask_code(i, g_lock.users[i].label);
}

static void user_delete_ok(void *u)
{
    lock_delete_user((int)(intptr_t)u);
    build_admin();
}

static void user_delete_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (lock_user_count() <= 1) { ui_toast("Keep at least one user code"); return; }
    char msg[80];
    snprintf(msg, sizeof(msg), "Delete the code for %s?", g_lock.users[i].label);
    ui_confirm("Delete code", msg, "Delete", UI_C_ERR, user_delete_ok, (void *)(intptr_t)i);
}

static void add_label_ok(const char *text, void *u)
{
    (void)u;
    int slot = -1;
    for (int i = 0; i < LOCK_MAX_USERS; i++) if (!g_lock.users[i].used) { slot = i; break; }
    if (slot < 0) { ui_toast("All 8 code slots are in use"); return; }
    ask_code(slot, text[0] ? text : "User");
}

static void user_add_cb(lv_event_t *e)
{
    (void)e;
    ui_text_prompt("Name for this code (e.g. Family, Guest)", "", false, 23, add_label_ok, NULL);
}

static void admin_code_cb(lv_event_t *e) { (void)e; ask_code(LOCK_ADMIN, NULL); }

static void name_ok(const char *text, void *u)
{
    if (!text[0]) return;
    int r = (int)(intptr_t)u;
    if (r == 1) snprintf(g_lock.relay1_name, sizeof(g_lock.relay1_name), "%s", text);
    else snprintf(g_lock.relay2_name, sizeof(g_lock.relay2_name), "%s", text);
    plat_lock_save();
    build_admin();
}

static void rename_cb(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    ui_text_prompt(r == 1 ? "Name for relay 1" : "Name for relay 2", r == 1 ? g_lock.relay1_name : g_lock.relay2_name,
                   false, 31, name_ok, (void *)(intptr_t)r);
}

static const uint16_t s_pulse_vals[] = {2, 3, 5, 10, 15, 30, 60};
static const uint8_t s_attempt_vals[] = {3, 5, 10};
static const uint16_t s_lockout_vals[] = {30, 60, 300, 900};

static void mode_cb(lv_event_t *e)
{
    g_lock.relay1_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    if (g_lock.relay1_mode == RELAY1_MOMENTARY && g_lock.relay1_unlocked) lock_lock("settings", true);
    plat_lock_save();
}
static void pulse_cb(lv_event_t *e) { g_lock.relay1_pulse_s = s_pulse_vals[lv_dropdown_get_selected(lv_event_get_target(e))]; plat_lock_save(); }
static void attempts_cb(lv_event_t *e) { g_lock.max_attempts = s_attempt_vals[lv_dropdown_get_selected(lv_event_get_target(e))]; plat_lock_save(); }
static void lockout_cb(lv_event_t *e) { g_lock.lockout_s = s_lockout_vals[lv_dropdown_get_selected(lv_event_get_target(e))]; plat_lock_save(); }
static void r2pin_cb(lv_event_t *e) { g_lock.relay2_needs_pin = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); plat_lock_save(); }
static void web_cb(lv_event_t *e) { g_lock.web_control = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); plat_lock_save(); }

static lv_obj_t *dd(lv_obj_t *row, const char *opts, int sel, int w, lv_event_cb_t cb)
{
    lv_obj_t *d = lv_dropdown_create(row);
    lv_dropdown_set_options(d, opts);
    lv_dropdown_set_selected(d, sel);
    lv_obj_set_width(d, w);
    lv_obj_set_style_bg_color(d, UI_C_SURFACE2, 0);
    lv_obj_add_event_cb(d, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return d;
}

static lv_obj_t *sw(lv_obj_t *row, bool on, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_switch_create(row);
    lv_obj_set_size(s, 84, 44);
    lv_obj_set_state(s, LV_STATE_CHECKED, on);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static int idx_of_u16(const uint16_t *v, int n, uint16_t x) { for (int i = 0; i < n; i++) if (v[i] == x) return i; return 0; }

static void build_admin(void)
{
    if (!s_root) return;
    clear_root();
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);

    lv_obj_t *hdr = lv_obj_create(s_root);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 60);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, "Lock administration");
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 28), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *done = ui_button(hdr, LV_SYMBOL_OK "  Done", UI_C_ACCENT);
    lv_obj_align(done, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(done, admin_done_cb, LV_EVENT_CLICKED, NULL);

    ui_section_title(s_root, "USER CODES");
    lv_obj_t *c = ui_card(s_root);
    for (int i = 0; i < LOCK_MAX_USERS; i++) {
        if (!g_lock.users[i].used) continue;
        lv_obj_t *r = ui_row(c, g_lock.users[i].label);
        lv_obj_t *b = ui_button(r, "Change code", UI_C_SURFACE2);
        lv_obj_set_height(b, 50);
        lv_obj_add_event_cb(b, user_change_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *d = ui_button(r, LV_SYMBOL_TRASH, UI_C_SURFACE2);
        lv_obj_set_size(d, 60, 50);
        lv_obj_set_style_pad_hor(d, 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(d, 0), UI_C_ERR, 0);
        lv_obj_add_event_cb(d, user_delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *br = lv_obj_create(c);
    lv_obj_remove_style_all(br);
    lv_obj_set_size(br, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(br, 12, 0);
    lv_obj_t *ad = ui_button(br, LV_SYMBOL_PLUS "  Add code", UI_C_ACCENT);
    lv_obj_set_flex_grow(ad, 1);
    lv_obj_add_event_cb(ad, user_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ac = ui_button(br, "Change admin code", UI_C_SURFACE2);
    lv_obj_set_flex_grow(ac, 1);
    lv_obj_add_event_cb(ac, admin_code_cb, LV_EVENT_CLICKED, NULL);

    ui_section_title(s_root, "RELAY 1");
    c = ui_card(s_root);
    lv_obj_t *r = ui_row(c, g_lock.relay1_name);
    lv_obj_t *rn = ui_button(r, LV_SYMBOL_EDIT "  Rename", UI_C_SURFACE2);
    lv_obj_set_height(rn, 50);
    lv_obj_add_event_cb(rn, rename_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    r = ui_row(c, "After unlocking");
    dd(r, "Stay unlocked until locked\nLock again after a delay", g_lock.relay1_mode, 330, mode_cb);
    r = ui_row(c, "Delay before locking again");
    dd(r, "2 seconds\n3 seconds\n5 seconds\n10 seconds\n15 seconds\n30 seconds\n1 minute",
       idx_of_u16(s_pulse_vals, 7, g_lock.relay1_pulse_s), 220, pulse_cb);

    ui_section_title(s_root, "RELAY 2");
    c = ui_card(s_root);
    r = ui_row(c, g_lock.relay2_name);
    rn = ui_button(r, LV_SYMBOL_EDIT "  Rename", UI_C_SURFACE2);
    lv_obj_set_height(rn, 50);
    lv_obj_add_event_cb(rn, rename_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    r = ui_row(c, "Require a code to switch");
    sw(r, g_lock.relay2_needs_pin, r2pin_cb);

    ui_section_title(s_root, "SECURITY");
    c = ui_card(s_root);
    r = ui_row(c, "Wrong codes before lockout");
    int ai = g_lock.max_attempts == 3 ? 0 : g_lock.max_attempts == 10 ? 2 : 1;
    dd(r, "3\n5\n10", ai, 120, attempts_cb);
    r = ui_row(c, "First lockout length (doubles each time)");
    dd(r, "30 seconds\n1 minute\n5 minutes\n15 minutes", idx_of_u16(s_lockout_vals, 4, g_lock.lockout_s), 220, lockout_cb);
    r = ui_row(c, "Allow lock control from the web page");
    sw(r, g_lock.web_control, web_cb);

    ui_section_title(s_root, "RECENT ACTIVITY");
    c = ui_card(s_root);
    lv_obj_set_style_pad_row(c, 4, 0);
    if (g_lock_log_count == 0) {
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, "No activity yet");
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    }
    for (int i = g_lock_log_count - 1, shown = 0; i >= 0 && shown < 25; i--, shown++) {
        struct tm tm;
        localtime_r(&g_lock_log[i].t, &tm);
        char tb[32], line[96];
        strftime(tb, sizeof(tb), "%b %d", &tm);
        char hb[16];
        ui_fmt_time(hb, sizeof(hb), &tm, false, true);
        snprintf(line, sizeof(line), "%s %s   %s  -  %s", tb, hb, lock_ev_text(g_lock_log[i].type), g_lock_log[i].who);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, line);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, g_lock_log[i].type == LEV_BAD_CODE || g_lock_log[i].type == LEV_LOCKOUT ? UI_C_WARN : UI_C_MUTED, 0);
    }
}

/* ------------------------------------------------------------------ app */

static void lock_create(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 16, 0);
    lv_obj_set_style_pad_row(parent, 12, 0);
    s_root = parent;
    s_admin_mode = false;
    s_code[0] = 0;
    build_main();
    s_timer = lv_timer_create(tick_cb, 500, NULL);
    if (!g_lock.admin_set)
        ui_pin_prompt("Create admin code", "First-time setup: choose a 6-digit admin code. It manages the user codes "
                                           "and protects the web page.", 6, 6, admin_setup_first, NULL);
}

static void lock_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_admin_mode = false;
    s_code[0] = 0;
    s_root = NULL;
    s_r1_state = NULL;
    s_prompt = s_dots = s_msg = NULL;
}

static void lock_on_settings(void)
{
    if (!s_root) return;
    if (s_admin_mode) build_admin();
    else refresh();
}

/* simulator hooks */
void app_lock_debug_admin(void) { s_admin_mode = true; build_admin(); }
void app_lock_debug_code(const char *c) { for (const char *p = c; *p; p++) key_cb(*p, NULL); }

const ui_app_t app_lock = {
    .name = "Lock",
    .icon = &icon_lock,
    .create = lock_create,
    .destroy = lock_destroy,
    .on_settings = lock_on_settings,
};
