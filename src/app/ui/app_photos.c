/*
 * Photos: browse the microSD card, view JPEGs full screen, run a slideshow, and serve as the
 * photo-frame screensaver.
 *
 *   Browser  : 4 x 4 thumbnail grid per page, folders first.  Swipe or use the arrows to page.
 *   Viewer   : full screen on the top layer.  Swipe left/right for next/previous, tap to show or
 *              hide the controls.  The next photo is decoded in the background while one is shown.
 *   Frame    : the viewer without controls, optionally with the time, used by the screensaver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"

#define GRID_COLS   4
#define GRID_ROWS   4
#define PER_PAGE    (GRID_COLS * GRID_ROWS)
#define TILE_W      168
#define TILE_H      120
#define TILE_GAP    8
#define MAX_ENTRIES 4000
#define MAX_SHOW    5000

static const uint8_t s_intervals[] = {3, 5, 10, 15, 30, 60, 120, 250};
static const char *const s_interval_names[] = {"3 s", "5 s", "10 s", "15 s", "30 s", "1 min", "2 min", "4 min"};
#define N_INTERVALS (int)(sizeof(s_intervals) / sizeof(s_intervals[0]))

/* ================================================================== shared image helper */

static void dsc_set(lv_image_dsc_t *d, uint16_t *px, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 2;
    d->data = (const uint8_t *)px;
    d->data_size = (uint32_t)w * h * 2;
}

/* ================================================================== viewer / frame */

typedef struct {
    lv_obj_t *root, *img, *spin, *err, *top, *bottom, *name, *idx, *play_lbl, *fill_lbl;
    lv_obj_t *clock, *date;
    bool frame;                 /* screensaver photo frame: no controls */
    bool playing, controls, gestured;
    char **paths;
    int count;
    int *order;                 /* display order -> index into paths */
    int pos;                    /* position in order currently wanted */
    bool shown;                 /* image for pos is on screen */
    uint16_t *px;               /* on screen */
    lv_image_dsc_t dsc;
    uint16_t *next_px;          /* prefetched image for pos + 1 */
    int next_pos, next_w, next_h;
    int fifo[4], fh, fn;        /* positions of requests in flight, in order */
    lv_timer_t *timer;
    uint32_t last_change_ms, controls_ms;
} viewer_t;

static viewer_t *s_viewer;      /* open viewer (top layer), NULL if none */

static int wrap(const viewer_t *v, int p) { return ((p % v->count) + v->count) % v->count; }
static const char *path_at(const viewer_t *v, int p) { return v->paths[v->order[wrap(v, p)]]; }

static void viewer_request(viewer_t *v, int pos);

static void show_pixels(viewer_t *v, uint16_t *px, int w, int h)
{
    lv_image_set_src(v->img, NULL);
    if (v->px) {
        lv_image_cache_drop(&v->dsc);
        plat_photo_free(v->px);
    }
    v->px = px;
    dsc_set(&v->dsc, px, w, h);
    lv_image_set_src(v->img, &v->dsc);
    lv_obj_center(v->img);
    if (v->playing || v->frame) lv_obj_fade_in(v->img, 350, 0);
    lv_obj_add_flag(v->spin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(v->err, LV_OBJ_FLAG_HIDDEN);
    v->shown = true;
    v->last_change_ms = lv_tick_get();
}

static void update_labels(viewer_t *v)
{
    if (v->frame) return;
    const char *p = path_at(v, v->pos);
    const char *slash = strrchr(p, '/');
    lv_label_set_text(v->name, slash ? slash + 1 : p);
    lv_label_set_text_fmt(v->idx, "%d / %d", wrap(v, v->pos) + 1, v->count);
    lv_label_set_text(v->play_lbl, v->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(v->fill_lbl, g_settings.photo_fill ? "Fit" : "Fill");
}

static void decoded_cb(const photo_img_t *img, void *user)
{
    viewer_t *v = (viewer_t *)user;
    if (v->fn <= 0) {   /* should not happen; drop it */
        plat_photo_free(img->pixels);
        return;
    }
    int pos = v->fifo[v->fh];
    v->fh = (v->fh + 1) % 4;
    v->fn--;
    if (pos == v->pos && !v->shown) {
        if (img->pixels) {
            show_pixels(v, img->pixels, img->w, img->h);
        } else {
            lv_label_set_text_fmt(v->err, LV_SYMBOL_WARNING "  %s\n%s", img->error, path_at(v, pos));
            lv_obj_remove_flag(v->err, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(v->spin, LV_OBJ_FLAG_HIDDEN);
            v->shown = true;   /* nothing to wait for; the slideshow moves on */
            v->last_change_ms = lv_tick_get();
        }
        if (v->count > 1 && v->fn == 0) viewer_request(v, v->pos + 1);   /* prefetch */
    } else if (pos == v->pos + 1 && img->pixels) {
        if (v->next_px) plat_photo_free(v->next_px);
        v->next_px = img->pixels;
        v->next_pos = pos;
        v->next_w = img->w;
        v->next_h = img->h;
    } else {
        plat_photo_free(img->pixels);
    }
}

static void viewer_request(viewer_t *v, int pos)
{
    if (v->fn >= 4) return;
    v->fifo[(v->fh + v->fn) % 4] = pos;
    v->fn++;
    plat_photo_decode(PHOTO_Q_VIEW, path_at(v, pos), UI_W, UI_H, g_settings.photo_fill ? PHOTO_FILL : PHOTO_FIT, false,
                      decoded_cb, v);
}

static void viewer_go(viewer_t *v, int pos)
{
    if (v->count <= 0) return;
    v->pos = pos;
    v->shown = false;
    update_labels(v);
    if (v->next_px && v->next_pos == pos) {
        uint16_t *px = v->next_px;
        v->next_px = NULL;
        show_pixels(v, px, v->next_w, v->next_h);
        /* the prefetch may still be decoding pos; if so its result becomes the new prefetch slot */
        bool pending_next = false;
        for (int i = 0; i < v->fn; i++) if (v->fifo[(v->fh + i) % 4] == pos + 1) pending_next = true;
        if (!pending_next && v->count > 1) {
            plat_photo_cancel(PHOTO_Q_VIEW);
            v->fn = 0;
            viewer_request(v, pos + 1);
        }
        return;
    }
    /* not prefetched: drop whatever is queued and decode this one first */
    plat_photo_cancel(PHOTO_Q_VIEW);
    v->fn = 0;
    if (v->next_px) {
        plat_photo_free(v->next_px);
        v->next_px = NULL;
    }
    lv_obj_remove_flag(v->spin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(v->err, LV_OBJ_FLAG_HIDDEN);
    viewer_request(v, pos);
}

static void controls_show(viewer_t *v, bool show)
{
    if (v->frame) return;
    v->controls = show;
    if (show) {
        lv_obj_remove_flag(v->top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v->bottom, LV_OBJ_FLAG_HIDDEN);
        v->controls_ms = lv_tick_get();
    } else {
        lv_obj_add_flag(v->top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_clock(viewer_t *v)
{
    if (!v->clock) return;
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    ui_fmt_time(buf, sizeof(buf), &t, false, true);
    lv_label_set_text(v->clock, buf);
    strftime(buf, sizeof(buf), "%A, %B %e", &t);
    lv_label_set_text(v->date, buf);
}

static void viewer_timer_cb(lv_timer_t *t)
{
    viewer_t *v = (viewer_t *)lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    update_clock(v);
    if (v->controls && v->playing && now - v->controls_ms > 4000) controls_show(v, false);
    if ((v->playing || v->frame) && v->shown && v->count > 1 &&
        now - v->last_change_ms >= (uint32_t)g_settings.photo_interval_s * 1000u) {
        viewer_go(v, v->pos + 1);
    }
}

static void viewer_free(viewer_t *v)
{
    plat_photo_cancel(PHOTO_Q_VIEW);
    if (v->timer) lv_timer_delete(v->timer);
    lv_image_set_src(v->img, NULL);
    if (v->px) {
        lv_image_cache_drop(&v->dsc);
        plat_photo_free(v->px);
    }
    if (v->next_px) plat_photo_free(v->next_px);
    plat_photo_paths_free(v->paths, v->count);
    free(v->order);
    if (s_viewer == v) s_viewer = NULL;
    free(v);
}

static void root_delete_cb(lv_event_t *e) { viewer_free((viewer_t *)lv_event_get_user_data(e)); }

static void root_event_cb(lv_event_t *e)
{
    viewer_t *v = (viewer_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        v->gestured = false;
    } else if (code == LV_EVENT_GESTURE) {
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        v->gestured = true;
        if (v->frame) return;
        if (d == LV_DIR_LEFT) viewer_go(v, v->pos + 1);
        else if (d == LV_DIR_RIGHT) viewer_go(v, v->pos - 1);
        else if (d == LV_DIR_BOTTOM) lv_obj_delete_async(v->root);   /* swipe down closes */
    } else if (code == LV_EVENT_CLICKED) {
        if (v->gestured || v->frame) return;   /* the frame's taps are handled by the screensaver */
        controls_show(v, !v->controls);
    }
}

static void btn_close_cb(lv_event_t *e) { lv_obj_delete_async(((viewer_t *)lv_event_get_user_data(e))->root); }
static void btn_prev_cb(lv_event_t *e) { viewer_t *v = lv_event_get_user_data(e); viewer_go(v, v->pos - 1); v->controls_ms = lv_tick_get(); }
static void btn_next_cb(lv_event_t *e) { viewer_t *v = lv_event_get_user_data(e); viewer_go(v, v->pos + 1); v->controls_ms = lv_tick_get(); }

static void btn_play_cb(lv_event_t *e)
{
    viewer_t *v = lv_event_get_user_data(e);
    v->playing = !v->playing;
    v->last_change_ms = lv_tick_get();
    v->controls_ms = lv_tick_get();
    update_labels(v);
}

static void btn_fill_cb(lv_event_t *e)
{
    viewer_t *v = lv_event_get_user_data(e);
    g_settings.photo_fill = !g_settings.photo_fill;
    ui_settings_changed();
    if (v->next_px) {
        plat_photo_free(v->next_px);
        v->next_px = NULL;
    }
    viewer_go(v, v->pos);   /* re-decode with the new crop */
    v->controls_ms = lv_tick_get();
}

static lv_obj_t *ctl_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, viewer_t *v, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 96, 64);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A3240), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, UI_C_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, v);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *bar(lv_obj_t *parent, lv_align_t align, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, UI_W, h);
    lv_obj_align(b, align, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

/* Shared by the viewer (controls) and the photo frame (clock, no controls). */
static viewer_t *viewer_build(lv_obj_t *parent, char **paths, int count, int start, bool frame, bool play)
{
    viewer_t *v = calloc(1, sizeof(viewer_t));
    if (!v) return NULL;
    v->paths = paths;
    v->count = count;
    v->frame = frame;
    v->playing = play;
    v->order = malloc(sizeof(int) * (count > 0 ? count : 1));
    for (int i = 0; i < count; i++) v->order[i] = i;
    if (g_settings.photo_shuffle && (play || frame) && count > 1) {
        for (int i = count - 1; i > 0; i--) {
            int j = (int)lv_rand(0, i);
            int t = v->order[i];
            v->order[i] = v->order[j];
            v->order[j] = t;
        }
        start = 0;
    }

    lv_obj_t *r = lv_obj_create(parent);
    v->root = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, UI_W, UI_H);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (frame) {
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_EVENT_BUBBLE);
    } else {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(r, root_event_cb, LV_EVENT_ALL, v);
    lv_obj_add_event_cb(r, root_delete_cb, LV_EVENT_DELETE, v);

    v->img = lv_image_create(r);
    lv_obj_remove_flag(v->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(v->img);

    v->spin = lv_spinner_create(r);
    lv_obj_set_size(v->spin, 72, 72);
    lv_obj_center(v->spin);
    lv_obj_remove_flag(v->spin, LV_OBJ_FLAG_CLICKABLE);

    v->err = lv_label_create(r);
    lv_obj_set_width(v->err, 600);
    lv_label_set_long_mode(v->err, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v->err, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(v->err, UI_C_MUTED, 0);
    lv_obj_set_style_text_font(v->err, &lv_font_montserrat_20, 0);
    lv_obj_center(v->err);
    lv_obj_add_flag(v->err, LV_OBJ_FLAG_HIDDEN);

    if (frame) {
        if (g_settings.photo_frame_clock) {
            lv_obj_t *pill = lv_obj_create(r);
            lv_obj_remove_style_all(pill);
            lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(pill, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(pill, LV_OPA_50, 0);
            lv_obj_set_style_radius(pill, 18, 0);
            lv_obj_set_style_pad_hor(pill, 22, 0);
            lv_obj_set_style_pad_ver(pill, 10, 0);
            lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_COLUMN);
            lv_obj_align(pill, LV_ALIGN_BOTTOM_LEFT, 24, -24);
            lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(pill, LV_OBJ_FLAG_EVENT_BUBBLE);
            v->clock = lv_label_create(pill);
            lv_obj_set_style_text_font(v->clock, ui_font(UI_FW_LIGHT, 64), 0);
            lv_obj_set_style_text_color(v->clock, lv_color_white(), 0);
            v->date = lv_label_create(pill);
            lv_obj_set_style_text_font(v->date, ui_font(UI_FW_REGULAR, 22), 0);
            lv_obj_set_style_text_color(v->date, lv_color_hex(0xDDE3EA), 0);
            update_clock(v);
        }
    } else {
        v->top = bar(r, LV_ALIGN_TOP_MID, 76);
        lv_obj_t *cb = ctl_button(v->top, LV_SYMBOL_CLOSE, btn_close_cb, v, NULL);
        lv_obj_set_size(cb, 64, 56);
        lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
        v->name = lv_label_create(v->top);
        lv_obj_set_width(v->name, 440);
        lv_label_set_long_mode(v->name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(v->name, ui_font(UI_FW_MEDIUM, 24), 0);
        lv_obj_set_style_text_color(v->name, lv_color_white(), 0);
        lv_obj_align(v->name, LV_ALIGN_LEFT_MID, 84, 0);
        v->idx = lv_label_create(v->top);
        lv_obj_set_style_text_font(v->idx, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(v->idx, lv_color_hex(0xC8D0DA), 0);
        lv_obj_align(v->idx, LV_ALIGN_RIGHT_MID, 0, 0);

        v->bottom = bar(r, LV_ALIGN_BOTTOM_MID, 96);
        lv_obj_set_flex_flow(v->bottom, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(v->bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ctl_button(v->bottom, LV_SYMBOL_LEFT, btn_prev_cb, v, NULL);
        ctl_button(v->bottom, LV_SYMBOL_PLAY, btn_play_cb, v, &v->play_lbl);
        ctl_button(v->bottom, LV_SYMBOL_RIGHT, btn_next_cb, v, NULL);
        lv_obj_t *fb = ctl_button(v->bottom, "Fill", btn_fill_cb, v, &v->fill_lbl);
        lv_obj_set_style_text_font(v->fill_lbl, &lv_font_montserrat_20, 0);
        (void)fb;
        v->controls = true;
        v->controls_ms = lv_tick_get();
        if (count <= 1) {
            lv_obj_add_state(lv_obj_get_child(v->bottom, 0), LV_STATE_DISABLED);
            lv_obj_add_state(lv_obj_get_child(v->bottom, 1), LV_STATE_DISABLED);
            lv_obj_add_state(lv_obj_get_child(v->bottom, 2), LV_STATE_DISABLED);
        }
    }

    v->timer = lv_timer_create(viewer_timer_cb, 500, v);
    viewer_go(v, start);
    return v;
}

/* Opens the full-screen viewer; takes ownership of paths. */
static void viewer_open(char **paths, int count, int start, bool play)
{
    if (s_viewer || count <= 0) {
        plat_photo_paths_free(paths, count);
        return;
    }
    s_viewer = viewer_build(lv_layer_top(), paths, count, start, false, play);
    if (!s_viewer) plat_photo_paths_free(paths, count);
}

lv_obj_t *photo_frame_create(lv_obj_t *parent)
{
    sd_status_t st;
    plat_sd_status(&st);
    if (st.state != SD_READY) return NULL;
    int count = 0;
    char **paths = plat_photo_paths(g_settings.photo_dir, g_settings.photo_recursive, MAX_SHOW, &count);
    if (!paths || count == 0) {
        plat_photo_paths_free(paths, count);
        return NULL;
    }
    viewer_t *v = viewer_build(parent, paths, count, count > 1 ? (int)lv_rand(0, count - 1) : 0, true, true);
    if (!v) {
        plat_photo_paths_free(paths, count);
        return NULL;
    }
    return v->root;
}

bool photo_viewer_open(void) { return s_viewer != NULL; }

/* ================================================================== browser */

typedef struct {
    lv_obj_t *root, *path_lbl, *up_btn, *grid, *pager, *page_lbl, *prev_btn, *next_btn, *msg, *msg_btn, *show_btn;
    char dir[200];
    photo_entry_t *entries;
    int count, nphotos, page, pages;
    lv_obj_t *tile[PER_PAGE], *timg[PER_PAGE];
    uint16_t *tpx[PER_PAGE];
    lv_image_dsc_t tdsc[PER_PAGE];
    int tentry[PER_PAGE];
    uint32_t token;
    bool gestured;
    lv_timer_t *poll;
    sd_state_t last_state;
} browser_t;

static browser_t *B;

static void browser_load_dir(void);

static void free_tiles(void)
{
    for (int i = 0; i < PER_PAGE; i++) {
        if (B->timg[i]) lv_image_set_src(B->timg[i], NULL);
        if (B->tpx[i]) {
            lv_image_cache_drop(&B->tdsc[i]);
            plat_photo_free(B->tpx[i]);
            B->tpx[i] = NULL;
        }
        B->tile[i] = B->timg[i] = NULL;
    }
}

static void thumb_cb(const photo_img_t *img, void *user)
{
    uintptr_t u = (uintptr_t)user;
    int slot = (int)(u & 0xFF);
    uint32_t token = (uint32_t)(u >> 8);
    if (!B || token != (B->token & 0xFFFFFF) || slot >= PER_PAGE || !B->timg[slot]) {
        plat_photo_free(img->pixels);
        return;
    }
    if (!img->pixels) {
        lv_obj_t *l = lv_label_create(B->tile[slot]);
        lv_label_set_text(l, LV_SYMBOL_WARNING);
        lv_obj_set_style_text_color(l, UI_C_WARN, 0);
        lv_obj_center(l);
        return;
    }
    B->tpx[slot] = img->pixels;
    dsc_set(&B->tdsc[slot], img->pixels, img->w, img->h);
    lv_image_set_src(B->timg[slot], &B->tdsc[slot]);
    lv_obj_center(B->timg[slot]);
}

static void rel_join(char *out, size_t n, const char *dir, const char *name)
{
    if (dir[0]) snprintf(out, n, "%s/%s", dir, name);
    else snprintf(out, n, "%s", name);
}

static void tile_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        B->gestured = false;
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || B->gestured) return;
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int ei = B->tentry[slot];
    if (ei < 0 || ei >= B->count) return;
    photo_entry_t *en = &B->entries[ei];
    if (en->is_dir) {
        char nd[200];
        rel_join(nd, sizeof(nd), B->dir, en->name);
        snprintf(B->dir, sizeof(B->dir), "%s", nd);
        B->page = 0;
        browser_load_dir();
        return;
    }
    /* open the viewer on this folder's photos */
    int nfolders = B->count - B->nphotos;
    char **paths = malloc(sizeof(char *) * (B->nphotos ? B->nphotos : 1));
    if (!paths) return;
    for (int i = 0; i < B->nphotos; i++) {
        char p[300];
        rel_join(p, sizeof(p), B->dir, B->entries[nfolders + i].name);
        paths[i] = strdup(p);
    }
    viewer_open(paths, B->nphotos, ei - nfolders, false);
}

static void browser_show_page(void)
{
    plat_photo_cancel(PHOTO_Q_THUMB);
    free_tiles();
    lv_obj_clean(B->grid);
    B->token++;
    B->pages = (B->count + PER_PAGE - 1) / PER_PAGE;
    if (B->pages < 1) B->pages = 1;
    if (B->page >= B->pages) B->page = B->pages - 1;
    if (B->page < 0) B->page = 0;

    for (int i = 0; i < PER_PAGE; i++) {
        int ei = B->page * PER_PAGE + i;
        B->tentry[i] = ei < B->count ? ei : -1;
        if (ei >= B->count) continue;
        photo_entry_t *en = &B->entries[ei];
        lv_obj_t *t = lv_obj_create(B->grid);
        B->tile[i] = t;
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_pos(t, (i % GRID_COLS) * (TILE_W + TILE_GAP), (i / GRID_COLS) * (TILE_H + TILE_GAP));
        lv_obj_set_style_bg_color(t, UI_C_SURFACE, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_clip_corner(t, true, 0);
        lv_obj_set_style_border_color(t, UI_C_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(t, 3, LV_STATE_PRESSED);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        if (en->is_dir) {
            lv_obj_set_style_bg_color(t, UI_C_SURFACE2, 0);
            lv_obj_t *ic = lv_label_create(t);
            lv_label_set_text(ic, LV_SYMBOL_DIRECTORY);
            lv_obj_set_style_text_font(ic, &lv_font_montserrat_40, 0);
            lv_obj_set_style_text_color(ic, UI_C_WARN, 0);
            lv_obj_align(ic, LV_ALIGN_CENTER, 0, -14);
            lv_obj_t *nl = lv_label_create(t);
            lv_label_set_text(nl, en->name);
            lv_label_set_long_mode(nl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nl, TILE_W - 16);
            lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(nl, &lv_font_montserrat_16, 0);
            lv_obj_align(nl, LV_ALIGN_BOTTOM_MID, 0, -10);
        } else {
            lv_obj_t *im = lv_image_create(t);
            B->timg[i] = im;
            lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(im, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_center(im);
            char p[300];
            rel_join(p, sizeof(p), B->dir, en->name);
            uintptr_t user = ((uintptr_t)(B->token & 0xFFFFFF) << 8) | (uintptr_t)i;
            plat_photo_decode(PHOTO_Q_THUMB, p, TILE_W, TILE_H, PHOTO_FILL, true, thumb_cb, (void *)user);
        }
    }
    lv_label_set_text_fmt(B->page_lbl, "Page %d of %d   \xE2\x80\xA2   %d photo%s", B->page + 1, B->pages, B->nphotos,
                          B->nphotos == 1 ? "" : "s");
    if (B->page > 0) lv_obj_remove_state(B->prev_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(B->prev_btn, LV_STATE_DISABLED);
    if (B->page < B->pages - 1) lv_obj_remove_state(B->next_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(B->next_btn, LV_STATE_DISABLED);
}

static void show_message(const char *icon, const char *text, const char *button)
{
    lv_obj_add_flag(B->grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(B->pager, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(B->msg, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(lv_obj_get_child(B->msg, 0), "%s", icon);
    lv_label_set_text(lv_obj_get_child(B->msg, 1), text);
    if (button) {
        lv_obj_remove_flag(B->msg_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(B->msg_btn, 0), button);
    } else {
        lv_obj_add_flag(B->msg_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void browser_load_dir(void)
{
    plat_photo_cancel(PHOTO_Q_THUMB);
    free_tiles();
    lv_obj_clean(B->grid);
    char shown[220];
    snprintf(shown, sizeof(shown), LV_SYMBOL_SD_CARD "  %s", B->dir[0] ? B->dir : "microSD card");
    lv_label_set_text(B->path_lbl, shown);
    if (B->dir[0]) lv_obj_remove_flag(B->up_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(B->up_btn, LV_OBJ_FLAG_HIDDEN);

    sd_status_t st;
    plat_sd_status(&st);
    B->last_state = st.state;
    lv_obj_add_state(B->show_btn, LV_STATE_DISABLED);
    if (st.state == SD_MOUNTING || st.state == SD_NONE) {
        show_message(LV_SYMBOL_SD_CARD, "Reading the microSD card...", NULL);
        return;
    }
    if (st.state != SD_READY) {
        char t[160];
        snprintf(t, sizeof(t), "No microSD card found\n%s", st.error);
        show_message(LV_SYMBOL_SD_CARD, t, LV_SYMBOL_REFRESH "  Try again");
        return;
    }
    int n = plat_photo_list(B->dir, B->entries, MAX_ENTRIES);
    if (n < 0 && B->dir[0]) {   /* folder vanished (deleted from the web page?) */
        B->dir[0] = 0;
        browser_load_dir();
        return;
    }
    B->count = n < 0 ? 0 : n;
    B->nphotos = 0;
    for (int i = 0; i < B->count; i++) if (!B->entries[i].is_dir) B->nphotos++;
    if (B->count == 0) {
        show_message(LV_SYMBOL_IMAGE,
                     B->dir[0] ? "This folder is empty."
                               : "No photos yet.\nCopy .jpg files to the card (any folders), or upload them\n"
                                 "from the Photos section of http://p4box.local",
                     LV_SYMBOL_REFRESH "  Re-scan card");
        return;
    }
    lv_obj_remove_flag(B->grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(B->pager, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(B->msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(B->show_btn, LV_STATE_DISABLED);
    browser_show_page();
}

static void up_cb(lv_event_t *e)
{
    (void)e;
    char *s = strrchr(B->dir, '/');
    if (s) *s = 0;
    else B->dir[0] = 0;
    B->page = 0;
    browser_load_dir();
}

static void page_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int np = B->page + d;
    if (np < 0 || np >= B->pages) return;
    B->page = np;
    browser_show_page();
}

static void grid_event_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) B->gestured = false;
    else if (c == LV_EVENT_GESTURE) {
        B->gestured = true;
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if (d == LV_DIR_LEFT && B->page < B->pages - 1) { B->page++; browser_show_page(); }
        else if (d == LV_DIR_RIGHT && B->page > 0) { B->page--; browser_show_page(); }
    }
}

static void msg_btn_cb(lv_event_t *e)
{
    (void)e;
    sd_status_t st;
    plat_sd_status(&st);
    if (st.state != SD_READY) {
        plat_sd_remount();
        show_message(LV_SYMBOL_SD_CARD, "Reading the microSD card...", NULL);
        B->last_state = SD_MOUNTING;
    } else {
        browser_load_dir();
    }
}

static void slideshow_cb(lv_event_t *e)
{
    (void)e;
    int count = 0;
    char **paths = plat_photo_paths(B->dir, g_settings.photo_recursive, MAX_SHOW, &count);
    if (!paths || count == 0) {
        plat_photo_paths_free(paths, count);
        ui_toast("No photos in this folder");
        return;
    }
    viewer_open(paths, count, 0, true);
}

/* ---- options ---- */

static void options_open(void);

static void options_pick(int i, void *user)
{
    (void)user;
    if (i < 0) return;
    switch (i) {
    case 0: {
        int k = 0;
        for (int j = 0; j < N_INTERVALS; j++) if (s_intervals[j] == g_settings.photo_interval_s) k = j;
        g_settings.photo_interval_s = s_intervals[(k + 1) % N_INTERVALS];
        break;
    }
    case 1: g_settings.photo_shuffle = !g_settings.photo_shuffle; break;
    case 2: g_settings.photo_fill = !g_settings.photo_fill; break;
    case 3: g_settings.photo_recursive = !g_settings.photo_recursive; break;
    case 4:
        snprintf(g_settings.photo_dir, sizeof(g_settings.photo_dir), "%s", B->dir);
        g_settings.saver_mode = SAVER_PHOTOS;
        ui_settings_changed();
        ui_toast(B->dir[0] ? "Photo frame will show this folder" : "Photo frame will show the whole card");
        return;
    case 5:
        plat_photo_clear_cache();
        ui_toast("Thumbnail cache cleared");
        browser_show_page();
        return;
    case 6:
        plat_sd_remount();
        B->last_state = SD_MOUNTING;
        show_message(LV_SYMBOL_SD_CARD, "Reading the microSD card...", NULL);
        return;
    }
    ui_settings_changed();
    options_open();   /* reopen so the toggles can be flipped in a row */
}

static void options_open(void)
{
    static char items[5][64];
    static const char *ptr[7];
    const char *iv = s_interval_names[2];
    for (int j = 0; j < N_INTERVALS; j++) if (s_intervals[j] == g_settings.photo_interval_s) iv = s_interval_names[j];
    snprintf(items[0], sizeof(items[0]), LV_SYMBOL_LOOP "  Slideshow: every %s", iv);
    snprintf(items[1], sizeof(items[1]), LV_SYMBOL_SHUFFLE "  Order: %s", g_settings.photo_shuffle ? "shuffled" : "by name");
    snprintf(items[2], sizeof(items[2]), LV_SYMBOL_IMAGE "  Show: %s", g_settings.photo_fill ? "fill the screen" : "whole photo");
    snprintf(items[3], sizeof(items[3]), LV_SYMBOL_DIRECTORY "  Include sub-folders: %s", g_settings.photo_recursive ? "yes" : "no");
    snprintf(items[4], sizeof(items[4]), LV_SYMBOL_HOME "  Use this folder as photo frame");
    for (int i = 0; i < 5; i++) ptr[i] = items[i];
    ptr[5] = LV_SYMBOL_TRASH "  Clear thumbnail cache";
    ptr[6] = LV_SYMBOL_REFRESH "  Re-scan card";
    ui_menu("Photo options", ptr, 7, options_pick, NULL);
}

static void options_cb(lv_event_t *e)
{
    (void)e;
    options_open();
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    sd_status_t st;
    plat_sd_status(&st);
    if (st.state != B->last_state && st.state != SD_MOUNTING) browser_load_dir();
}

static lv_obj_t *hdr_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_button(parent, text, UI_C_SURFACE2);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void photos_create(lv_obj_t *parent)
{
    B = calloc(1, sizeof(browser_t));
    B->entries = malloc(sizeof(photo_entry_t) * MAX_ENTRIES);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *r = lv_obj_create(parent);
    B->root = r;
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, UI_W, UI_H - UI_BAR_H);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

    /* header */
    lv_obj_t *h = lv_obj_create(r);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, UI_W - 24, 56);
    lv_obj_set_pos(h, 12, 4);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, 10, 0);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    B->up_btn = hdr_button(h, LV_SYMBOL_UP, up_cb);
    B->path_lbl = lv_label_create(h);
    lv_obj_set_flex_grow(B->path_lbl, 1);
    lv_label_set_long_mode(B->path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(B->path_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(B->path_lbl, UI_C_MUTED, 0);
    B->show_btn = hdr_button(h, LV_SYMBOL_PLAY "  Slideshow", slideshow_cb);
    lv_obj_set_style_bg_color(B->show_btn, UI_C_ACCENT, 0);
    hdr_button(h, LV_SYMBOL_SETTINGS, options_cb);

    /* grid */
    B->grid = lv_obj_create(r);
    lv_obj_remove_style_all(B->grid);
    lv_obj_set_size(B->grid, GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP, GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP);
    lv_obj_set_pos(B->grid, (UI_W - (GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP)) / 2, 64);
    lv_obj_remove_flag(B->grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(B->grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(B->grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(B->grid, grid_event_cb, LV_EVENT_ALL, NULL);

    /* pager */
    B->pager = lv_obj_create(r);
    lv_obj_remove_style_all(B->pager);
    lv_obj_set_size(B->pager, UI_W - 24, 52);
    lv_obj_set_pos(B->pager, 12, 64 + GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP + 12);
    lv_obj_remove_flag(B->pager, LV_OBJ_FLAG_SCROLLABLE);
    B->prev_btn = hdr_button(B->pager, LV_SYMBOL_LEFT, page_cb);
    lv_obj_remove_event_cb(B->prev_btn, page_cb);
    lv_obj_add_event_cb(B->prev_btn, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_set_width(B->prev_btn, 96);
    lv_obj_align(B->prev_btn, LV_ALIGN_LEFT_MID, 0, 0);
    B->next_btn = hdr_button(B->pager, LV_SYMBOL_RIGHT, page_cb);
    lv_obj_remove_event_cb(B->next_btn, page_cb);
    lv_obj_add_event_cb(B->next_btn, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_set_width(B->next_btn, 96);
    lv_obj_align(B->next_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    B->page_lbl = lv_label_create(B->pager);
    lv_obj_set_style_text_font(B->page_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(B->page_lbl, UI_C_MUTED, 0);
    lv_obj_center(B->page_lbl);

    /* message (no card / empty) */
    B->msg = lv_obj_create(r);
    lv_obj_remove_style_all(B->msg);
    lv_obj_set_size(B->msg, UI_W - 80, 480);
    lv_obj_align(B->msg, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_flex_flow(B->msg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(B->msg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(B->msg, 22, 0);
    lv_obj_remove_flag(B->msg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mi = lv_label_create(B->msg);
    lv_obj_set_style_text_font(mi, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(mi, UI_C_ACCENT, 0);
    lv_obj_t *mt = lv_label_create(B->msg);
    lv_obj_set_style_text_align(mt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(mt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(mt, UI_C_MUTED, 0);
    lv_obj_set_style_text_line_space(mt, 8, 0);
    B->msg_btn = ui_button(B->msg, "", UI_C_SURFACE2);
    lv_obj_add_event_cb(B->msg_btn, msg_btn_cb, LV_EVENT_CLICKED, NULL);

    B->poll = lv_timer_create(poll_cb, 700, NULL);
    browser_load_dir();
}

static void photos_destroy(void)
{
    if (!B) return;
    plat_photo_cancel(PHOTO_Q_THUMB);
    if (B->poll) lv_timer_delete(B->poll);
    free_tiles();
    free(B->entries);
    free(B);
    B = NULL;
}

const ui_app_t app_photos = {.name = "Photos", .icon = &icon_photos, .create = photos_create, .destroy = photos_destroy};
