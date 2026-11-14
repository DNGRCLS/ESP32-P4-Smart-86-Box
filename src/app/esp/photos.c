/*
 * Photos back end: microSD state, folder listing and JPEG decoding.
 *
 *  - Decoding uses Espressif's esp_new_jpeg (baseline JPEG, built into Arduino core 3.3.12+;
 *    the bundled TJpgDec on older cores) with its 1/2, 1/4, 1/8 scaling, then an area-average resampler that also applies the EXIF
 *    orientation and the fit / fill crop.  Output is RGB565 for LVGL.
 *  - Thumbnails use the small preview JPEG most cameras and phones embed in the EXIF data, and
 *    are cached on the card in /.p4thumbs so a folder opens instantly the second time.
 *  - One worker task serves two queues (full-screen images first, then thumbnails); a queue's
 *    pending work can be cancelled when the UI moves on.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#if __has_include("esp_jpeg_dec.h")
#define HAVE_ESP_NEW_JPEG 1   /* Arduino core 3.3.12+ ships Espressif's optimised decoder */
#include "esp_jpeg_dec.h"
#else
#define HAVE_ESP_NEW_JPEG 0
#endif
#include "../../jpeg/tjpgd.h"  /* bundled TJpgDec: used when esp_new_jpeg is missing or refuses a file */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
/* Stacks of tasks that never write to the SPI flash (and never exit) live in PSRAM. */
#define PSRAM_STACK (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#include "sdcard.h"
#include "../core/platform.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"

static const char *TAG = "photos";

#define MAX_FILE_BYTES   (20 * 1024 * 1024)
#define MAX_DECODE_BYTES (12 * 1024 * 1024)   /* largest intermediate RGB565 buffer */
#define HEAD_BYTES       (96 * 1024)          /* enough for EXIF incl. its preview image */
#define THUMB_DIR        SDCARD_MOUNT "/.p4thumbs"

/* ================================================================== card state */

static SemaphoreHandle_t s_mtx;
static sd_status_t s_sd;
static TaskHandle_t s_sd_task;

static void sd_task(void *arg)
{
    for (;;) {
        char err[64] = "";
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_sd.state = SD_MOUNTING;
        s_sd.error[0] = 0;
        xSemaphoreGive(s_mtx);
        bool ok = sdcard_mount(err, sizeof(err));
        uint64_t total = ok ? sdcard_total_bytes() : 0, used = ok ? sdcard_used_bytes() : 0;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_sd.state = ok ? SD_READY : SD_ERROR;
        s_sd.total_bytes = total;
        s_sd.used_bytes = used;
        snprintf(s_sd.error, sizeof(s_sd.error), "%s", err);
        xSemaphoreGive(s_mtx);
        if (ok) ESP_LOGI(TAG, "card mounted: %.1f GB, %.1f GB used", total / 1e9, used / 1e9);
        else ESP_LOGW(TAG, "card: %s", err);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for plat_sd_remount() */
    }
}

void plat_sd_status(sd_status_t *st)
{
    if (!s_mtx) { memset(st, 0, sizeof(*st)); return; }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *st = s_sd;
    xSemaphoreGive(s_mtx);
}

void plat_sd_remount(void)
{
    if (s_sd_task) xTaskNotifyGive(s_sd_task);
}

static bool sd_ready(void)
{
    sd_status_t st;
    plat_sd_status(&st);
    return st.state == SD_READY;
}

bool photos_abs_path(const char *rel, char *out, int n)
{
    if (!rel) rel = "";
    while (*rel == '/') rel++;
    if (strstr(rel, "..")) return false;
    int r = rel[0] ? snprintf(out, n, "%s/%s", SDCARD_MOUNT, rel) : snprintf(out, n, "%s", SDCARD_MOUNT);
    if (r <= 0 || r >= n) return false;
    size_t l = strlen(out);
    while (l > strlen(SDCARD_MOUNT) && out[l - 1] == '/') out[--l] = 0;
    return true;
}

/* ================================================================== listing */

static bool is_jpeg_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".jpe"));
}

static bool skip_name(const char *n)
{
    return n[0] == '.' || !strcasecmp(n, "System Volume Information") || !strcasecmp(n, "$RECYCLE.BIN") ||
           !strcasecmp(n, "LOST.DIR") || !strncmp(n, "._", 2);
}

/* "IMG_9.jpg" before "IMG_10.jpg" */
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *sa = a, *sb = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            int la = a - sa, lb = b - sb;
            if (la != lb) return la - lb;
            int c = strncmp(sa, sb, la);
            if (c) return c;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const photo_entry_t *a = pa, *b = pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return natural_cmp(a->name, b->name);
}

int plat_photo_list(const char *dir, photo_entry_t *out, int max)
{
    if (!sd_ready()) return -1;
    char path[300];
    if (!photos_abs_path(dir, path, sizeof(path))) return -1;
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (skip_name(e->d_name) || strlen(e->d_name) >= sizeof(out[0].name)) continue;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN) {
            char full[400];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (!is_dir && !is_jpeg_name(e->d_name)) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].is_dir = is_dir;
        out[n].size = 0;
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof(out[0]), entry_cmp);
    return n;
}

typedef struct {
    char **v;
    int n, cap, max;
} pathlist_t;

static int str_natural_cmp(const void *a, const void *b) { return natural_cmp(*(char *const *)a, *(char *const *)b); }

static void collect(pathlist_t *pl, const char *rel, bool recursive, int depth)
{
    char path[300];
    if (!photos_abs_path(rel, path, sizeof(path))) return;
    DIR *d = opendir(path);
    if (!d) return;
    int first = pl->n;
    char **subdirs = NULL;
    int nsub = 0, capsub = 0;
    struct dirent *e;
    while ((e = readdir(d)) && pl->n < pl->max) {
        if (skip_name(e->d_name)) continue;
        bool is_dir = e->d_type == DT_DIR;
        if (is_dir) {
            if (!recursive || depth >= 6) continue;
            if (nsub == capsub) {
                capsub = capsub ? capsub * 2 : 8;
                char **ns = realloc(subdirs, capsub * sizeof(char *));
                if (!ns) continue;
                subdirs = ns;
            }
            subdirs[nsub++] = strdup(e->d_name);
            continue;
        }
        if (!is_jpeg_name(e->d_name)) continue;
        char relp[300];
        int r = rel[0] ? snprintf(relp, sizeof(relp), "%s/%s", rel, e->d_name) : snprintf(relp, sizeof(relp), "%s", e->d_name);
        if (r <= 0 || r >= (int)sizeof(relp)) continue;
        if (pl->n == pl->cap) {
            int nc = pl->cap ? pl->cap * 2 : 64;
            char **nv = heap_caps_realloc(pl->v, nc * sizeof(char *), MALLOC_CAP_SPIRAM);
            if (!nv) break;
            pl->v = nv;
            pl->cap = nc;
        }
        pl->v[pl->n++] = strdup(relp);
    }
    closedir(d);
    qsort(pl->v + first, pl->n - first, sizeof(char *), str_natural_cmp);
    qsort(subdirs, nsub, sizeof(char *), str_natural_cmp);
    for (int i = 0; i < nsub; i++) {
        char relp[300];
        int r = rel[0] ? snprintf(relp, sizeof(relp), "%s/%s", rel, subdirs[i]) : snprintf(relp, sizeof(relp), "%s", subdirs[i]);
        if (r > 0 && r < (int)sizeof(relp) && pl->n < pl->max) collect(pl, relp, recursive, depth + 1);
        free(subdirs[i]);
    }
    free(subdirs);
}

char **plat_photo_paths(const char *dir, bool recursive, int max, int *count)
{
    *count = 0;
    if (!sd_ready()) return NULL;
    pathlist_t pl = {.max = max};
    collect(&pl, dir ? dir : "", recursive, 0);
    *count = pl.n;
    return pl.v;
}

void plat_photo_paths_free(char **paths, int count)
{
    if (!paths) return;
    for (int i = 0; i < count; i++) free(paths[i]);
    heap_caps_free(paths);
}

/* ================================================================== JPEG parsing */

typedef struct {
    int w, h;
    bool progressive;
    bool found_sof;
    int orientation;          /* EXIF 1..8 */
    int thumb_off, thumb_len; /* EXIF preview JPEG inside the file */
} jpeg_meta_t;

static uint16_t rd16(const uint8_t *p, bool le) { return le ? (p[0] | p[1] << 8) : (p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p, bool le)
{
    return le ? (p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) : ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

static void parse_exif(const uint8_t *t, int len, int tiff_file_off, jpeg_meta_t *m)
{
    if (len < 8) return;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;
    uint32_t ifd = rd32(t + 4, le);
    for (int which = 0; which < 2 && ifd && ifd + 2 <= (uint32_t)len; which++) {
        int n = rd16(t + ifd, le);
        uint32_t toff = 0, tlen = 0;
        for (int i = 0; i < n; i++) {
            uint32_t e = ifd + 2 + i * 12;
            if (e + 12 > (uint32_t)len) return;
            int tag = rd16(t + e, le);
            int type = rd16(t + e + 2, le);
            uint32_t val = type == 3 ? rd16(t + e + 8, le) : rd32(t + e + 8, le);
            if (which == 0 && tag == 0x0112) m->orientation = (val >= 1 && val <= 8) ? (int)val : 1;
            if (which == 1 && tag == 0x0201) toff = val;
            if (which == 1 && tag == 0x0202) tlen = val;
        }
        if (which == 1 && toff && tlen && toff + tlen <= (uint32_t)len) {
            m->thumb_off = tiff_file_off + toff;
            m->thumb_len = tlen;
        }
        uint32_t next = ifd + 2 + n * 12;
        ifd = next + 4 <= (uint32_t)len ? rd32(t + next, le) : 0;
    }
}

/* Walks the JPEG markers in the first bytes of the file. */
static void parse_jpeg(const uint8_t *b, int len, jpeg_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->orientation = 1;
    if (len < 4 || b[0] != 0xFF || b[1] != 0xD8) return;
    int p = 2;
    while (p + 4 <= len) {
        if (b[p] != 0xFF) { p++; continue; }
        int mk = b[p + 1];
        if (mk == 0xFF) { p++; continue; }
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) { p += 2; continue; }
        int seg = (b[p + 2] << 8) | b[p + 3];
        if (seg < 2) return;
        const uint8_t *d = b + p + 4;
        int dlen = seg - 2;
        if (mk == 0xE1 && p + 4 + 6 <= len && !memcmp(d, "Exif\0\0", 6)) {
            int avail = len - (p + 4 + 6);
            parse_exif(d + 6, dlen - 6 < avail ? dlen - 6 : avail, p + 4 + 6, m);
        } else if ((mk >= 0xC0 && mk <= 0xCF) && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            if (p + 4 + 5 <= len) {
                m->h = (d[1] << 8) | d[2];
                m->w = (d[3] << 8) | d[4];
                m->found_sof = true;
                m->progressive = mk == 0xC2 || mk == 0xC6 || mk == 0xCA || mk == 0xCE;
            }
            return;
        } else if (mk == 0xDA) {
            return;
        }
        p += 2 + seg;
    }
}

/* ================================================================== decode + resample */

#if HAVE_ESP_NEW_JPEG
/* Decodes with esp_new_jpeg at 1/div scale.  Returns an RGB565 buffer (16-byte aligned). */
static uint16_t *newjpeg_decode(const uint8_t *data, int len, int w, int h, int div, int *ow, int *oh, char *err, int errlen)
{
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (div > 1) {
        cfg.scale.width = (uint16_t)((w / div) & ~7);
        cfg.scale.height = (uint16_t)((h / div) & ~7);
        if (cfg.scale.width < 8 || cfg.scale.height < 8) {
            cfg.scale.width = cfg.scale.height = 0;
        }
    }
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        snprintf(err, errlen, "Decoder could not start");
        return NULL;
    }
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)data, .inbuf_len = len};
    jpeg_dec_header_info_t info;
    uint16_t *out = NULL;
    int outlen = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        snprintf(err, errlen, "Not a supported JPEG");
    } else if (jpeg_dec_get_outbuf_len(dec, &outlen) != JPEG_ERR_OK || outlen <= 0 || outlen > MAX_DECODE_BYTES) {
        snprintf(err, errlen, "Photo is too large (%dx%d)", info.width, info.height);
    } else if (!(out = heap_caps_aligned_alloc(16, outlen, MALLOC_CAP_SPIRAM))) {
        snprintf(err, errlen, "Not enough memory");
    } else {
        io.outbuf = (uint8_t *)out;
        if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
            heap_caps_free(out);
            out = NULL;
            snprintf(err, errlen, "Could not decode this JPEG");
        } else if (cfg.scale.width) {
            *ow = cfg.scale.width;
            *oh = cfg.scale.height;
        } else {
            *ow = info.width;
            *oh = info.height;
        }
    }
    jpeg_dec_close(dec);
    return out;
}
#endif

typedef struct {
    const uint8_t *data;
    int len, pos;
    uint16_t *out;
    int ow, oh;
} tj_io_t;

static size_t tj_in(JDEC *jd, uint8_t *buf, size_t n)
{
    tj_io_t *io = (tj_io_t *)jd->device;
    if ((int)n > io->len - io->pos) n = io->len - io->pos;
    if (buf) memcpy(buf, io->data + io->pos, n);
    io->pos += n;
    return n;
}

static int tj_out(JDEC *jd, void *bitmap, JRECT *r)
{
    tj_io_t *io = (tj_io_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;
    int w = r->right - r->left + 1;
    if (r->left >= io->ow) return 1;
    int cw = r->left + w > io->ow ? io->ow - r->left : w;
    for (int y = r->top; y <= r->bottom && y < io->oh; y++)
        memcpy(io->out + (size_t)y * io->ow + r->left, src + (size_t)(y - r->top) * w, cw * 2);
    return 1;
}

/* Decodes with TJpgDec at 1/div scale (div = 1, 2, 4 or 8).  Returns an RGB565 buffer. */
static uint16_t *tjpgd_decode(const uint8_t *data, int len, int w, int h, int div, int *ow, int *oh, char *err, int errlen)
{
    (void)w;
    (void)h;
    void *pool = heap_caps_malloc(32 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pool) pool = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    if (!pool) {
        snprintf(err, errlen, "Not enough memory");
        return NULL;
    }
    JDEC jd;
    memset(&jd, 0, sizeof(jd));   /* also clears the byte-swap option (keep native RGB565) */
    tj_io_t io = {.data = data, .len = len};
    uint16_t *out = NULL;
    if (jd_prepare(&jd, tj_in, pool, 32 * 1024, &io) != JDR_OK) {
        snprintf(err, errlen, "Not a supported JPEG");
    } else {
        int s = div >= 8 ? 3 : div >= 4 ? 2 : div >= 2 ? 1 : 0;
        io.ow = (jd.width + (1 << s) - 1) >> s;
        io.oh = (jd.height + (1 << s) - 1) >> s;
        if ((int64_t)io.ow * io.oh * 2 > MAX_DECODE_BYTES) {
            snprintf(err, errlen, "Photo is too large (%dx%d)", jd.width, jd.height);
        } else if (!(io.out = out = heap_caps_aligned_alloc(16, (size_t)io.ow * io.oh * 2, MALLOC_CAP_SPIRAM))) {
            snprintf(err, errlen, "Not enough memory");
        } else if (jd_decomp(&jd, tj_out, s) != JDR_OK) {
            heap_caps_free(out);
            out = NULL;
            snprintf(err, errlen, "Could not decode this JPEG");
        } else {
            *ow = io.ow;
            *oh = io.oh;
        }
    }
    heap_caps_free(pool);
    return out;
}

static uint16_t *jpeg_decode(const uint8_t *data, int len, int w, int h, int div, int *ow, int *oh, char *err, int errlen)
{
#if HAVE_ESP_NEW_JPEG
    uint16_t *out = newjpeg_decode(data, len, w, h, div, ow, oh, err, errlen);
    if (out) return out;
    ESP_LOGW(TAG, "esp_new_jpeg failed (%s), trying TJpgDec", err);
#endif
    return tjpgd_decode(data, len, w, h, div, ow, oh, err, errlen);
}

/* Maps a pixel of the displayed (EXIF-oriented) image to the stored image. */
static inline void orient_map(int o, int u, int v, int W, int H, int *x, int *y)
{
    switch (o) {
    case 2: *x = W - 1 - u; *y = v; break;
    case 3: *x = W - 1 - u; *y = H - 1 - v; break;
    case 4: *x = u; *y = H - 1 - v; break;
    case 5: *x = v; *y = u; break;
    case 6: *x = v; *y = H - 1 - u; break;
    case 7: *x = W - 1 - v; *y = H - 1 - u; break;
    case 8: *x = W - 1 - v; *y = u; break;
    default: *x = u; *y = v; break;
    }
}

/*
 * Area-average resample of src (W x H, stored orientation) into dst (ow x oh), taking the
 * rectangle (cx, cy, cw, ch) of the oriented image.
 */
static void resample(const uint16_t *src, int W, int H, int o, int cx, int cy, int cw, int ch, uint16_t *dst, int ow, int oh)
{
    for (int y = 0; y < oh; y++) {
        int v0 = cy + (int)((int64_t)y * ch / oh);
        int v1 = cy + (int)((int64_t)(y + 1) * ch / oh);
        if (v1 <= v0) v1 = v0 + 1;
        int vstep = (v1 - v0 + 3) / 4;   /* at most 4 samples per axis */
        for (int x = 0; x < ow; x++) {
            int u0 = cx + (int)((int64_t)x * cw / ow);
            int u1 = cx + (int)((int64_t)(x + 1) * cw / ow);
            if (u1 <= u0) u1 = u0 + 1;
            int ustep = (u1 - u0 + 3) / 4;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int v = v0; v < v1; v += vstep) {
                for (int u = u0; u < u1; u += ustep) {
                    int sx, sy;
                    orient_map(o, u, v, W, H, &sx, &sy);
                    if ((unsigned)sx >= (unsigned)W || (unsigned)sy >= (unsigned)H) continue;
                    uint16_t p = src[sy * W + sx];
                    r += p >> 11;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                    n++;
                }
            }
            if (!n) n = 1;
            dst[y * ow + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
}

typedef struct {
    photo_queue_t q;
    uint32_t gen;
    char path[300];       /* card-relative */
    int w, h;
    photo_fit_t fit;
    bool thumb;
    photo_cb_t cb;
    void *user;
} req_t;

/* Builds the final w x h (or smaller, for FIT) image from a decoded buffer. */
static bool finish(const uint16_t *dec, int dw, int dh, int orient, const req_t *rq, photo_img_t *img)
{
    bool rot = orient >= 5;
    int OW = rot ? dh : dw, OH = rot ? dw : dh;
    int cx = 0, cy = 0, cw = OW, ch = OH, ow, oh;
    if (rq->fit == PHOTO_FILL) {
        /* crop the oriented image to the box's aspect ratio */
        if ((int64_t)OW * rq->h > (int64_t)OH * rq->w) {
            cw = (int)((int64_t)OH * rq->w / rq->h);
            cx = (OW - cw) / 2;
        } else {
            ch = (int)((int64_t)OW * rq->h / rq->w);
            cy = (OH - ch) / 2;
        }
        ow = rq->w;
        oh = rq->h;
    } else {
        if ((int64_t)OW * rq->h > (int64_t)OH * rq->w) {
            ow = rq->w;
            oh = (int)((int64_t)OH * rq->w / OW);
        } else {
            oh = rq->h;
            ow = (int)((int64_t)OW * rq->h / OH);
        }
        if (OW <= rq->w && OH <= rq->h) { ow = OW; oh = OH; }   /* never enlarge small images */
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
    }
    uint16_t *out = heap_caps_aligned_alloc(16, (size_t)ow * oh * 2, MALLOC_CAP_SPIRAM);
    if (!out) {
        snprintf(img->error, sizeof(img->error), "Not enough memory");
        return false;
    }
    resample(dec, dw, dh, orient, cx, cy, cw, ch, out, ow, oh);
    img->pixels = out;
    img->w = ow;
    img->h = oh;
    return true;
}

/* ---- thumbnail cache ---- */

static uint32_t fnv1a(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void cache_name(const req_t *rq, const struct stat *st, char *out, int n)
{
    uint32_t h = fnv1a(2166136261u, rq->path, strlen(rq->path));
    uint32_t extra[5] = {(uint32_t)st->st_size, (uint32_t)st->st_mtime, (uint32_t)rq->w, (uint32_t)rq->h, (uint32_t)rq->fit};
    h = fnv1a(h, extra, sizeof(extra));
    snprintf(out, n, THUMB_DIR "/%08lx.t", (unsigned long)h);
}

static bool cache_load(const char *name, const req_t *rq, photo_img_t *img)
{
    FILE *f = fopen(name, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    bool ok = false;
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "P4T1", 4)) {
        int w = hdr[4] | hdr[5] << 8, h = hdr[6] | hdr[7] << 8;
        int sw = hdr[8] | hdr[9] << 8, sh = hdr[10] | hdr[11] << 8;
        if (w > 0 && h > 0 && w <= rq->w && h <= rq->h) {
            uint16_t *px = heap_caps_aligned_alloc(16, (size_t)w * h * 2, MALLOC_CAP_SPIRAM);
            if (px && fread(px, 2, (size_t)w * h, f) == (size_t)w * h) {
                img->pixels = px;
                img->w = w;
                img->h = h;
                img->src_w = sw;
                img->src_h = sh;
                ok = true;
            } else if (px) {
                heap_caps_free(px);
            }
        }
    }
    fclose(f);
    return ok;
}

static void cache_save(const char *name, const photo_img_t *img)
{
    mkdir(THUMB_DIR, 0775);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    uint8_t hdr[12] = {'P', '4', 'T', '1', img->w & 255, img->w >> 8, img->h & 255, img->h >> 8,
                       img->src_w & 255, (img->src_w >> 8) & 255, img->src_h & 255, (img->src_h >> 8) & 255};
    bool ok = fwrite(hdr, 1, 12, f) == 12 && fwrite(img->pixels, 2, (size_t)img->w * img->h, f) == (size_t)img->w * img->h;
    fclose(f);
    if (!ok) unlink(name);
}

void plat_photo_clear_cache(void)
{
    DIR *d = opendir(THUMB_DIR);
    if (!d) return;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), THUMB_DIR "/%s", e->d_name);
        unlink(p);
    }
    closedir(d);
    ESP_LOGI(TAG, "thumbnail cache cleared");
}

/* ---- one request ---- */

static uint8_t *read_file(const char *path, int max, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    if (size > max) size = max;
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (buf && fread(buf, 1, size, f) != (size_t)size) {
        heap_caps_free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (int)size;
    return buf;
}

static void process(const req_t *rq, photo_img_t *img)
{
    memset(img, 0, sizeof(*img));
    char path[340];
    struct stat st;
    if (!photos_abs_path(rq->path, path, sizeof(path)) || stat(path, &st) != 0) {
        snprintf(img->error, sizeof(img->error), "File not found");
        return;
    }
    if (st.st_size > MAX_FILE_BYTES) {
        snprintf(img->error, sizeof(img->error), "File is larger than 20 MB");
        return;
    }
    char cname[64] = "";
    if (rq->thumb) {
        cache_name(rq, &st, cname, sizeof(cname));
        if (cache_load(cname, rq, img)) return;
    }

    int64_t t0 = esp_timer_get_time();
    int len = 0;
    uint8_t *data = read_file(path, rq->thumb ? HEAD_BYTES : MAX_FILE_BYTES, &len);
    if (!data) {
        snprintf(img->error, sizeof(img->error), "Could not read the file");
        return;
    }
    jpeg_meta_t m;
    parse_jpeg(data, len, &m);
    if (!m.found_sof && rq->thumb && len < st.st_size) {
        /* big APP segments in front of the image header: read it all */
        heap_caps_free(data);
        data = read_file(path, MAX_FILE_BYTES, &len);
        if (!data) {
            snprintf(img->error, sizeof(img->error), "Could not read the file");
            return;
        }
        parse_jpeg(data, len, &m);
    }
    if (!m.found_sof) {
        heap_caps_free(data);
        snprintf(img->error, sizeof(img->error), "Not a JPEG file");
        return;
    }
    bool rot = m.orientation >= 5;
    img->src_w = rot ? m.h : m.w;
    img->src_h = rot ? m.w : m.h;

    char err[64] = "";
    uint16_t *dec = NULL;
    int dw = 0, dh = 0;

    /* 1) thumbnails: the EXIF preview is plenty and ~100x faster than the full image */
    if (rq->thumb && m.thumb_len > 0 && m.thumb_off + m.thumb_len <= len) {
        jpeg_meta_t tm;
        parse_jpeg(data + m.thumb_off, m.thumb_len, &tm);
        if (tm.found_sof && !tm.progressive && tm.w >= rq->w / 2)
            dec = jpeg_decode(data + m.thumb_off, m.thumb_len, tm.w, tm.h, 1, &dw, &dh, err, sizeof(err));
    }

    /* 2) the photo itself, at the coarsest 1/2^n scale that still covers the box */
    if (!dec) {
        if (m.progressive) {
            heap_caps_free(data);
            snprintf(img->error, sizeof(img->error), "Progressive JPEGs are not supported");
            return;
        }
        if (len < st.st_size) {   /* thumbnail request that only read the head of the file */
            heap_caps_free(data);
            data = read_file(path, MAX_FILE_BYTES, &len);
            if (!data) {
                snprintf(img->error, sizeof(img->error), "Could not read the file");
                return;
            }
        }
        int need_w = rot ? rq->h : rq->w, need_h = rot ? rq->w : rq->h;   /* in stored orientation */
        double sx = (double)need_w / m.w, sy = (double)need_h / m.h;
        double s = rq->fit == PHOTO_FILL ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        int div = 1;
        while (div < 8 && s * div * 2 <= 1.0) div *= 2;
        while ((int64_t)(m.w / div) * (m.h / div) * 2 > MAX_DECODE_BYTES && div < 8) div *= 2;
        dec = jpeg_decode(data, len, m.w, m.h, div, &dw, &dh, err, sizeof(err));
        if (!dec && div > 1 && (int64_t)m.w * m.h * 2 <= MAX_DECODE_BYTES)
            dec = jpeg_decode(data, len, m.w, m.h, 1, &dw, &dh, err, sizeof(err));   /* scaler refused */
    }
    heap_caps_free(data);
    if (!dec) {
        snprintf(img->error, sizeof(img->error), "%s", err[0] ? err : "Could not decode this JPEG");
        return;
    }
    finish(dec, dw, dh, m.orientation, rq, img);
    heap_caps_free(dec);
    ESP_LOGD(TAG, "%s: %dx%d -> %dx%d in %d ms", rq->path, m.w, m.h, img->w, img->h,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (img->pixels && rq->thumb && cname[0]) cache_save(cname, img);
}

/* ================================================================== worker */

static QueueHandle_t s_q[2];
static SemaphoreHandle_t s_work;
static volatile uint32_t s_gen[2];

static void worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_work, portMAX_DELAY);
        req_t *rq = NULL;
        if (xQueueReceive(s_q[PHOTO_Q_VIEW], &rq, 0) != pdTRUE && xQueueReceive(s_q[PHOTO_Q_THUMB], &rq, 0) != pdTRUE)
            continue;
        if (rq->gen != s_gen[rq->q]) { free(rq); continue; }   /* cancelled */
        photo_img_t img;
        process(rq, &img);
        bool delivered = false;
        if (lvgl_port_lock(0)) {
            if (rq->gen == s_gen[rq->q]) {   /* re-check under the LVGL lock: the UI may be gone */
                rq->cb(&img, rq->user);
                delivered = true;
            }
            lvgl_port_unlock();
        }
        if (!delivered && img.pixels) heap_caps_free(img.pixels);
        free(rq);
    }
}

void plat_photo_decode(photo_queue_t q, const char *path, int w, int h, photo_fit_t fit, bool thumbnail,
                       photo_cb_t cb, void *user)
{
    if (!s_work || w <= 0 || h <= 0) return;
    req_t *rq = calloc(1, sizeof(*rq));
    if (!rq) return;
    rq->q = q;
    rq->gen = s_gen[q];
    snprintf(rq->path, sizeof(rq->path), "%s", path);
    rq->w = w;
    rq->h = h;
    rq->fit = fit;
    rq->thumb = thumbnail;
    rq->cb = cb;
    rq->user = user;
    if (xQueueSend(s_q[q], &rq, 0) != pdTRUE) {
        free(rq);
        return;
    }
    xSemaphoreGive(s_work);
}

void plat_photo_cancel(photo_queue_t q) { s_gen[q]++; }

void plat_photo_free(uint16_t *pixels)
{
    if (pixels) heap_caps_free(pixels);
}

void photos_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_q[0] = xQueueCreate(64, sizeof(req_t *));
    s_q[1] = xQueueCreate(8, sizeof(req_t *));
    s_work = xSemaphoreCreateCounting(128, 0);
    xTaskCreatePinnedToCoreWithCaps(sd_task, "sdcard", 6144, NULL, 3, &s_sd_task, 1, PSRAM_STACK);
    xTaskCreatePinnedToCoreWithCaps(worker, "photo_dec", 12 * 1024, NULL, 3, NULL, 1, PSRAM_STACK);   /* core 1: the task watchdog only watches core 0 idle */
}
