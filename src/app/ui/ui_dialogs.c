/* Modal dialogs: text prompt with on-screen keyboard, confirm box, toast. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"

typedef struct {
    lv_obj_t *backdrop;
    lv_obj_t *ta;
    ui_text_cb_t on_text;
    ui_confirm_cb_t on_confirm;
    void *user;
    bool closed;
} dlg_t;

static void backdrop_delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static lv_obj_t *make_backdrop(dlg_t *d)
{
    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, UI_W, UI_H);
    lv_obj_set_style_bg_color(bd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_70, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bd, backdrop_delete_cb, LV_EVENT_DELETE, d);
    d->backdrop = bd;
    return bd;
}

static void dlg_close(dlg_t *d)
{
    d->closed = true;
    lv_obj_delete_async(d->backdrop);   /* d is freed by the backdrop's DELETE handler */
}

/* ------------------------------------------------------------------ text prompt */

static void prompt_finish(dlg_t *d, bool ok)
{
    if (d->closed) return;
    if (ok && d->on_text) {
        char *copy = strdup(lv_textarea_get_text(d->ta));
        ui_text_cb_t cb = d->on_text;
        void *user = d->user;
        dlg_close(d);
        cb(copy, user);
        free(copy);
    } else {
        dlg_close(d);
    }
}

static void kb_event_cb(lv_event_t *e)
{
    dlg_t *d = (dlg_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) prompt_finish(d, true);
    else if (code == LV_EVENT_CANCEL) prompt_finish(d, false);
}

static void ok_cb(lv_event_t *e) { prompt_finish((dlg_t *)lv_event_get_user_data(e), true); }
static void cancel_cb(lv_event_t *e) { prompt_finish((dlg_t *)lv_event_get_user_data(e), false); }

static void eye_cb(lv_event_t *e)
{
    dlg_t *d = (dlg_t *)lv_event_get_user_data(e);
    if (d->closed) return;
    lv_obj_t *btn = lv_event_get_current_target(e);
    bool show = lv_obj_has_state(btn, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(d->ta, !show);
}

void ui_text_prompt(const char *title, const char *initial, bool password, int max_len,
                    ui_text_cb_t on_ok, void *user)
{
    dlg_t *d = calloc(1, sizeof(dlg_t));
    d->on_text = on_ok;
    d->user = user;
    lv_obj_t *bd = make_backdrop(d);

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_set_size(card, 680, 250);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(card, UI_C_SURFACE, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 28), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    d->ta = lv_textarea_create(card);
    lv_textarea_set_one_line(d->ta, true);
    lv_textarea_set_password_mode(d->ta, password);
    if (max_len > 0) lv_textarea_set_max_length(d->ta, max_len);
    lv_textarea_set_text(d->ta, initial ? initial : "");
    lv_obj_set_width(d->ta, password ? 540 : 640);
    lv_obj_set_style_text_font(d->ta, &lv_font_montserrat_28, 0);
    lv_obj_align(d->ta, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_add_state(d->ta, LV_STATE_FOCUSED);

    if (password) {
        lv_obj_t *eye = lv_button_create(card);
        lv_obj_set_size(eye, 88, 56);
        lv_obj_add_flag(eye, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_color(eye, UI_C_SURFACE2, 0);
        lv_obj_set_style_bg_color(eye, UI_C_ACCENT, LV_STATE_CHECKED);
        lv_obj_set_style_shadow_width(eye, 0, 0);
        lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, 0, 52);
        lv_obj_t *el = lv_label_create(eye);
        lv_label_set_text(el, LV_SYMBOL_EYE_OPEN);
        lv_obj_center(el);
        lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_VALUE_CHANGED, d);
    }

    lv_obj_t *cancel = ui_button(card, "Cancel", UI_C_SURFACE2);
    lv_obj_set_width(cancel, 180);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, d);
    lv_obj_t *ok = ui_button(card, "OK", UI_C_ACCENT);
    lv_obj_set_width(ok, 180);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok, ok_cb, LV_EVENT_CLICKED, d);

    lv_obj_t *kb = lv_keyboard_create(bd);
    lv_obj_set_size(kb, UI_W, 400);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(kb, UI_C_SURFACE, 0);
    lv_keyboard_set_textarea(kb, d->ta);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, d);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, d);
}

/* ------------------------------------------------------------------ confirm */

static void confirm_ok_cb(lv_event_t *e)
{
    dlg_t *d = (dlg_t *)lv_event_get_user_data(e);
    if (d->closed) return;
    ui_confirm_cb_t cb = d->on_confirm;
    void *user = d->user;
    dlg_close(d);
    if (cb) cb(user);
}

static void confirm_cancel_cb(lv_event_t *e)
{
    dlg_t *d = (dlg_t *)lv_event_get_user_data(e);
    if (!d->closed) dlg_close(d);
}

void ui_confirm(const char *title, const char *message, const char *ok_text, lv_color_t ok_color,
                ui_confirm_cb_t on_ok, void *user)
{
    dlg_t *d = calloc(1, sizeof(dlg_t));
    d->on_confirm = on_ok;
    d->user = user;
    lv_obj_t *bd = make_backdrop(d);

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UI_C_SURFACE, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 30), 0);

    lv_obj_t *m = lv_label_create(card);
    lv_label_set_text(m, message);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_color(m, UI_C_MUTED, 0);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_style_pad_top(row, 8, 0);

    lv_obj_t *c = ui_button(row, "Cancel", UI_C_SURFACE2);
    lv_obj_add_event_cb(c, confirm_cancel_cb, LV_EVENT_CLICKED, d);
    lv_obj_t *o = ui_button(row, ok_text, ok_color);
    lv_obj_add_event_cb(o, confirm_ok_cb, LV_EVENT_CLICKED, d);
}

/* ------------------------------------------------------------------ toast */

static void toast_del_cb(lv_timer_t *t)
{
    lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(t));
}

void ui_toast(const char *text)
{
    lv_obj_t *l = lv_label_create(lv_layer_sys());
    lv_label_set_text(l, text);
    lv_obj_set_style_bg_color(l, lv_color_hex(0x2B3442), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(l, UI_C_TEXT, 0);
    lv_obj_set_style_radius(l, 24, 0);
    lv_obj_set_style_pad_hor(l, 24, 0);
    lv_obj_set_style_pad_ver(l, 14, 0);
    lv_obj_set_style_max_width(l, 640, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_timer_t *t = lv_timer_create(toast_del_cb, 2600, l);
    lv_timer_set_repeat_count(t, 1);
}

/* ------------------------------------------------------------------ menu */

typedef struct {
    dlg_t *d;
    int idx;
} menu_item_t;

static ui_menu_cb_t s_menu_cb;

static void menu_item_cb(lv_event_t *e)
{
    menu_item_t *mi = (menu_item_t *)lv_event_get_user_data(e);
    dlg_t *d = mi->d;
    if (d->closed) return;
    int idx = mi->idx;
    ui_menu_cb_t cb = s_menu_cb;
    void *user = d->user;
    dlg_close(d);
    if (cb && idx >= 0) cb(idx, user);
}

static void menu_item_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

void ui_menu(const char *title, const char *const *items, int count, ui_menu_cb_t on_pick, void *user)
{
    dlg_t *d = calloc(1, sizeof(dlg_t));
    d->user = user;
    s_menu_cb = on_pick;
    lv_obj_t *bd = make_backdrop(d);

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, 640, 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UI_C_SURFACE, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 26), 0);
    lv_obj_set_style_pad_bottom(t, 6, 0);

    for (int i = 0; i < count; i++) {
        lv_obj_t *b = ui_button(card, items[i], UI_C_SURFACE2);
        lv_obj_set_width(b, lv_pct(100));
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        if (strstr(items[i], LV_SYMBOL_TRASH)) lv_obj_set_style_text_color(l, UI_C_ERR, 0);
        menu_item_t *mi = malloc(sizeof(*mi));
        mi->d = d;
        mi->idx = i;
        lv_obj_add_event_cb(b, menu_item_cb, LV_EVENT_CLICKED, mi);
        lv_obj_add_event_cb(b, menu_item_free_cb, LV_EVENT_DELETE, mi);
    }
    lv_obj_t *c = ui_button(card, "Cancel", UI_C_BG);
    lv_obj_set_width(c, lv_pct(100));
    menu_item_t *mi = malloc(sizeof(*mi));
    mi->d = d;
    mi->idx = -1;
    lv_obj_add_event_cb(c, menu_item_cb, LV_EVENT_CLICKED, mi);
    lv_obj_add_event_cb(c, menu_item_free_cb, LV_EVENT_DELETE, mi);
}

/* ------------------------------------------------------------------ keypad + PIN prompt */

typedef struct {
    ui_key_cb_t cb;
    void *user;
} keypad_t;

static void keypad_btn_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    keypad_t *k = (keypad_t *)lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *t = lv_buttonmatrix_get_button_text(bm, id);
    if (!t) return;
    char key = t[0];
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) key = '<';
    else if (!strcmp(t, LV_SYMBOL_OK)) key = '#';
    if (k->cb) k->cb(key, k->user);
}

static void keypad_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

lv_obj_t *ui_keypad_create(lv_obj_t *parent, int w, int h, ui_key_cb_t cb, void *user)
{
    static const char *map[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""};
    lv_obj_t *bm = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(bm, map);
    lv_obj_set_size(bm, w, h);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_pad_gap(bm, 12, 0);
    lv_obj_set_style_radius(bm, 18, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, UI_C_SURFACE2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, UI_C_BORDER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(bm, ui_font(UI_FW_MEDIUM, 36), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, UI_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    keypad_t *k = malloc(sizeof(*k));
    k->cb = cb;
    k->user = user;
    lv_obj_add_event_cb(bm, keypad_btn_cb, LV_EVENT_VALUE_CHANGED, k);
    lv_obj_add_event_cb(bm, keypad_free_cb, LV_EVENT_DELETE, k);
    return bm;
}

/* The OK and backspace keys use symbol glyphs: give them the Montserrat font via a style on CHECKED-free items.
   (Poppins has no symbol glyphs, but ui_font() falls back to Montserrat 28 automatically.) */

typedef struct {
    dlg_t *d;
    lv_obj_t *dots, *msg;
    char code[8];
    int min, max;
    ui_text_cb_t cb;
} pin_dlg_t;

static void pin_update(pin_dlg_t *p)
{
    char buf[32] = "";
    int n = strlen(p->code);
    for (int i = 0; i < n; i++) strcat(buf, i ? "  *" : "*");
    for (int i = n; i < p->max; i++) strcat(buf, i ? "  _" : "_");
    lv_label_set_text(p->dots, buf);
}

static void pin_key(char k, void *user)
{
    pin_dlg_t *p = user;
    if (p->d->closed) return;
    int n = strlen(p->code);
    if (k == '<') {
        if (n) p->code[n - 1] = 0;
    } else if (k == '#') {
        if (n < p->min) {
            lv_label_set_text_fmt(p->msg, "Use %d to %d digits", p->min, p->max);
            return;
        }
        char code[8];
        snprintf(code, sizeof(code), "%s", p->code);
        ui_text_cb_t cb = p->cb;
        void *u = p->d->user;
        dlg_close(p->d);
        if (cb) cb(code, u);
        return;
    } else if (k >= '0' && k <= '9' && n < p->max) {
        p->code[n] = k;
        p->code[n + 1] = 0;
    }
    lv_label_set_text(p->msg, "");
    pin_update(p);
}

static void pin_cancel_cb(lv_event_t *e)
{
    pin_dlg_t *p = lv_event_get_user_data(e);
    if (!p->d->closed) dlg_close(p->d);
}

static void pin_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

void ui_pin_prompt(const char *title, const char *subtitle, int min_len, int max_len, ui_text_cb_t on_ok, void *user)
{
    dlg_t *d = calloc(1, sizeof(dlg_t));
    d->user = user;
    pin_dlg_t *p = calloc(1, sizeof(pin_dlg_t));
    p->d = d;
    p->min = min_len;
    p->max = max_len > 7 ? 7 : max_len;
    p->cb = on_ok;
    lv_obj_t *bd = make_backdrop(d);
    lv_obj_add_event_cb(bd, pin_free_cb, LV_EVENT_DELETE, p);

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_set_size(card, 520, 680);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, UI_C_SURFACE, 0);
    lv_obj_set_style_border_color(card, UI_C_BORDER, 0);
    lv_obj_set_style_text_color(card, UI_C_TEXT, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 28), 0);
    lv_obj_t *s = lv_label_create(card);
    lv_label_set_text(s, subtitle ? subtitle : "");
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s, UI_C_MUTED, 0);
    lv_obj_set_style_text_font(s, &lv_font_montserrat_16, 0);

    p->dots = lv_label_create(card);
    lv_obj_set_style_text_font(p->dots, ui_font(UI_FW_MEDIUM, 40), 0);
    lv_obj_set_style_text_color(p->dots, UI_C_ACCENT, 0);
    p->msg = lv_label_create(card);
    lv_label_set_text(p->msg, "");
    lv_obj_set_style_text_color(p->msg, UI_C_ERR, 0);
    lv_obj_set_style_text_font(p->msg, &lv_font_montserrat_16, 0);
    pin_update(p);

    ui_keypad_create(card, 420, 360, pin_key, p);

    lv_obj_t *c = ui_button(card, "Cancel", UI_C_SURFACE2);
    lv_obj_set_width(c, 420);
    lv_obj_set_height(c, 52);
    lv_obj_add_event_cb(c, pin_cancel_cb, LV_EVENT_CLICKED, p);
}
