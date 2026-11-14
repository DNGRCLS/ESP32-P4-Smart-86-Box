/*
 * Internet radio engine.
 *
 *   stream task : HTTP(S) client -> playlists (.pls/.m3u), redirects, ICY metadata
 *                 -> 512 KB ring buffer in PSRAM
 *   decode task : ring buffer -> Helix MP3 / Helix HE-AAC (ADTS) decoder
 *                 -> mono downmix -> I2S -> ES8311 codec -> NS4150B amplifier -> speaker
 *
 * A "session" counter separates streams: the stream task bumps it for every new connection and
 * waits until the decode task has flushed its state, so audio from two stations never mixes.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "radio.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "cJSON.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "usb_audio.h"
#include "mp3dec.h"
#include "../../audio/helix_aac/aacdec.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
/* Stacks of tasks that never write to the SPI flash (and never exit) live in PSRAM. */
#define PSRAM_STACK (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char *TAG = "radio";

#define RING_SIZE     (512 * 1024)
#define PREBUF_FIRST  (64 * 1024)    /* ~4 s at 128 kbps before playback starts */
#define PREBUF_AGAIN  (24 * 1024)    /* after an underrun */
#define IN_CHUNK      (8 * 1024)
#define USER_AGENT    "P4Box/1.0 (ESP32-P4; +http://p4box.local)"

#define I2S_PORT      I2S_NUM_1
#define PIN_MCLK      13
#define PIN_BCLK      12
#define PIN_WS        10
#define PIN_DOUT      9
#define PIN_DIN       11

typedef enum { CMD_PLAY, CMD_STOP } cmd_type_t;
typedef struct {
    cmd_type_t type;
    char name[64];
    char url[256];
} cmd_t;

static QueueHandle_t s_cmdq;
static StreamBufferHandle_t s_ring;
static SemaphoreHandle_t s_mtx;
static radio_status_t s_st;
static i2s_chan_handle_t s_tx;
static bool s_spk_ok;
static int s_volume = 60;

static volatile uint32_t s_session;       /* bumped by the stream task */
static volatile uint32_t s_dec_session;   /* acknowledged by the decode task */
static volatile bool s_session_active;    /* a stream is feeding the ring buffer */
static volatile bool s_stream_eof;        /* current stream ended: drain the buffer */
static volatile int s_dec_type;           /* DEC_MP3 / DEC_AAC for the session */
static volatile int s_buzz;               /* alarm tone: 0 off, 1 beeps, 2 buzzer */
static volatile bool s_pref_usb = true;   /* play on a USB audio device (Bluetooth transmitter) when one is plugged in */
static volatile bool s_force_spk;         /* alarms: always the built-in speaker */
static volatile bool s_out_usb;           /* the stream is going to USB right now */

enum { DEC_NONE = 0, DEC_MP3, DEC_AAC, DEC_OGG, DEC_FLAC, DEC_M4A };

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static void set_state(radio_state_t st, const char *err)
{
    LOCK();
    s_st.state = st;
    if (err) snprintf(s_st.error, sizeof(s_st.error), "%s", err);
    else if (st != RADIO_ERROR && st != RADIO_RECONNECTING) s_st.error[0] = 0;
    UNLOCK();
}

/* ================================================================== audio output */

static esp_err_t speaker_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");
    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED},
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");   /* MCLK must run before the codec is set up */
    ESP_RETURN_ON_ERROR(es8311_init(board_i2c(), ES8311_ADDR), TAG, "es8311");
    es8311_set_volume(s_volume);
    s_spk_ok = true;
    return ESP_OK;
}

static void speaker_set_rate(uint32_t rate)
{
    if (!s_tx) return;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    i2s_channel_disable(s_tx);
    i2s_channel_reconfig_std_clock(s_tx, &clk);
    i2s_channel_enable(s_tx);
}

static void speaker_amp(bool on)
{
    gpio_set_level(BOARD_PA_CTRL, on ? 1 : 0);
}

/* ================================================================== decode task */

/* Interleaved s16 (1 or 2+ channels) -> the same mono mix on left and right, in place.
 * The speaker is mono; sending it on both I2S slots works whichever slot the codec takes. */
static uint32_t to_dual_mono(int16_t *pcm, uint32_t total_samples, int ch)
{
    uint32_t frames = ch > 1 ? total_samples / ch : total_samples;
    if (ch > 1) {
        for (uint32_t f = 0; f < frames; f++) pcm[f] = (int16_t)(((int32_t)pcm[f * ch] + pcm[f * ch + 1]) / 2);
    }
    for (int32_t f = (int32_t)frames - 1; f >= 0; f--) {
        int16_t v = pcm[f];
        pcm[2 * f] = v;
        pcm[2 * f + 1] = v;
    }
    return frames;
}

/* ================================================================== alarm tones */

#define BUZZ_RATE   22050
#define BUZZ_CHUNK  (BUZZ_RATE / 50)        /* 20 ms per I2S write */

/*
 * Four short bursts, then a pause:  on 120 ms / off 90 ms  x 4,  then 660 ms quiet  (1.5 s cycle).
 *   beeps  : 2.7 kHz, the classic digital alarm-clock beep (sine with some 3rd harmonic)
 *   buzzer : 440 Hz square wave with a 60 Hz rasp, like an old clock-radio buzzer
 * A 4 ms fade at each edge avoids clicks.
 */
static int buzz_fill(int16_t *out, int frames, int kind, uint32_t *pos, float *phase)
{
    const uint32_t on = BUZZ_RATE * 120 / 1000, gap = BUZZ_RATE * 90 / 1000;
    const uint32_t burst = on + gap, cycle = BUZZ_RATE * 1500 / 1000, edge = BUZZ_RATE * 4 / 1000;
    const float f = kind == 1 ? 2700.0f : 440.0f;
    const float step = 2.0f * 3.14159265f * f / BUZZ_RATE;
    for (int i = 0; i < frames; i++) {
        uint32_t p = (*pos)++ % cycle;
        float v = 0;
        if (p < burst * 4) {
            uint32_t q = p % burst;
            if (q < on) {
                float env = 1.0f;
                if (q < edge) env = (float)q / edge;
                else if (on - q < edge) env = (float)(on - q) / edge;
                if (kind == 1) {
                    v = sinf(*phase) + 0.25f * sinf(3.0f * *phase);
                    v *= 0.85f;
                } else {
                    v = sinf(*phase) >= 0 ? 0.72f : -0.72f;
                    float rasp = sinf(2.0f * 3.14159265f * 60.0f * (float)q / BUZZ_RATE);
                    v *= 0.75f + 0.25f * rasp;
                }
                v *= env;
            }
        }
        *phase += step;
        if (*phase > 6.2831853f) *phase -= 6.2831853f;
        int16_t sample = (int16_t)(v * 30000.0f);
        out[2 * i] = sample;
        out[2 * i + 1] = sample;
    }
    return frames;
}

#define PCM_MAX_SAMPLES 4608   /* HE-AAC: 2048 samples x 2 ch; MP3: 1152 x 2 (also room for dual-mono) */

/* ================================================================== USB output */

#define USB_OUT_MAX_FRAMES 13000           /* one decoded frame resampled up to 6x (8 kHz -> 48 kHz) */

typedef struct {
    uint32_t src_rate, dev_rate;
    uint32_t pos;                          /* Q16 position from the previous frame */
    int16_t prev[2];
    int16_t *out;                          /* PSRAM */
} usb_out_t;

static usb_out_t s_uo;

/* Interleaved s16, ch channels -> stereo in place (pcm has room for 2 x frames). */
static uint32_t to_stereo(int16_t *pcm, uint32_t total_samples, int ch)
{
    if (ch == 2) return total_samples / 2;
    uint32_t frames = ch > 2 ? total_samples / ch : total_samples;
    if (ch > 2) {
        for (uint32_t f = 0; f < frames; f++) { pcm[2 * f] = pcm[f * ch]; pcm[2 * f + 1] = pcm[f * ch + 1]; }
        return frames;
    }
    for (int32_t f = (int32_t)frames - 1; f >= 0; f--) { pcm[2 * f + 1] = pcm[f]; pcm[2 * f] = pcm[f]; }
    return frames;
}

/* Linear-interpolation resampler, stereo s16; returns output frames. */
static uint32_t resample(usb_out_t *u, const int16_t *in, uint32_t n, int16_t *out, uint32_t max_out)
{
    uint32_t step = (uint32_t)(((uint64_t)u->src_rate << 16) / u->dev_rate);
    uint32_t p = u->pos, o = 0;
    while ((p >> 16) < n && o < max_out) {
        uint32_t i = p >> 16, f = p & 0xFFFF;
        for (int c = 0; c < 2; c++) {
            int32_t a = i ? in[(i - 1) * 2 + c] : u->prev[c];
            int32_t b = in[i * 2 + c];
            out[o * 2 + c] = (int16_t)(a + (((b - a) * (int32_t)f) >> 16));
        }
        o++;
        p += step;
    }
    u->pos = p - (n << 16);
    if (n) { u->prev[0] = in[(n - 1) * 2]; u->prev[1] = in[(n - 1) * 2 + 1]; }
    return o;
}

static void usb_off(void)
{
    if (s_out_usb) usb_audio_stop();
    s_out_usb = false;
    s_uo.src_rate = 0;
}

/* Sends one decoded block to the USB device. false: not on USB (use the speaker). */
static bool usb_play(int16_t *pcm, uint32_t samples, int ch, uint32_t rate)
{
    bool want = s_pref_usb && !s_force_spk && usb_audio_connected();
    if (!want) {
        if (s_out_usb) { usb_off(); ESP_LOGI(TAG, "output: built-in speaker"); }
        return false;
    }
    if (!s_uo.out) s_uo.out = heap_caps_malloc(USB_OUT_MAX_FRAMES * 4, MALLOC_CAP_SPIRAM);
    if (!s_uo.out) return false;
    if (!s_out_usb || s_uo.src_rate != rate) {
        uint32_t r = usb_audio_start(rate);
        if (!r) { usb_off(); return false; }
        s_uo.src_rate = rate;
        s_uo.dev_rate = r;
        s_uo.pos = 0;
        s_uo.prev[0] = s_uo.prev[1] = 0;
        if (!s_out_usb) ESP_LOGI(TAG, "output: USB audio (%u Hz)", (unsigned)r);
        s_out_usb = true;
    }
    if (ch == 1 && samples * 2 > PCM_MAX_SAMPLES) return true;   /* cannot happen with MP3/AAC; drop */
    uint32_t frames = to_stereo(pcm, samples, ch);
    /* volume: the speaker codec is not in the path, so scale the samples (square law ~ loudness) */
    int v = s_volume;
    int32_t g = v * v * 32768 / 10000;
    if (g < 32768)
        for (uint32_t i = 0; i < frames * 2; i++) pcm[i] = (int16_t)((pcm[i] * g) >> 15);
    const int16_t *src = pcm;
    uint32_t n = frames;
    if (s_uo.dev_rate != rate) {
        n = resample(&s_uo, pcm, frames, s_uo.out, USB_OUT_MAX_FRAMES);
        src = s_uo.out;
    }
    if (!usb_audio_write(src, n, 500)) {
        ESP_LOGW(TAG, "USB audio stopped responding: back to the built-in speaker");
        usb_off();
        return false;
    }
    return true;
}

static void decode_task(void *arg)
{
    uint8_t *in = heap_caps_malloc(IN_CHUNK * 2, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(PCM_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t in_len = 0;
    HMP3Decoder mp3 = NULL;
    HAACDecoder aac = NULL;
    bool started = false, amp_on = false;
    uint32_t cur_rate = 0;
    int cur_ch = 0, errors = 0;
    uint32_t prebuf = PREBUF_FIRST;
    uint32_t my_session = 0;
    bool buzzing = false;
    int idle_loops = 0;
    uint32_t buzz_pos = 0;
    float buzz_phase = 0;

    for (;;) {
        int kind = s_buzz;
        if (kind) {
            /* alarm tone has priority over the stream */
            if (!buzzing) {
                buzzing = true;
                usb_off();   /* alarm tones are always on the built-in speaker */
                buzz_pos = 0;
                buzz_phase = 0;
                speaker_set_rate(BUZZ_RATE);
                cur_rate = 0;   /* the next stream sets its own rate again */
                if (!amp_on) { speaker_amp(true); amp_on = true; }
            }
            buzz_fill(pcm, BUZZ_CHUNK, kind, &buzz_pos, &buzz_phase);
            size_t wrote = 0;
            if (s_spk_ok) i2s_channel_write(s_tx, pcm, BUZZ_CHUNK * 4, &wrote, pdMS_TO_TICKS(200));
            else vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (buzzing) {
            buzzing = false;
            if (amp_on) { speaker_amp(false); amp_on = false; }
            started = false;
        }
        if (my_session != s_session) {
            /* new stream (or stop): drop everything from the previous one */
            if (mp3) { MP3FreeDecoder(mp3); mp3 = NULL; }
            if (aac) { AACFreeDecoder(aac); aac = NULL; }
            if (amp_on) { speaker_amp(false); amp_on = false; }
            xStreamBufferReset(s_ring);
            in_len = 0;
            started = false;
            cur_rate = 0;
            cur_ch = 0;
            errors = 0;
            prebuf = PREBUF_FIRST;
            my_session = s_session;
            s_dec_session = my_session;
            continue;
        }
        if (!s_session_active && xStreamBufferBytesAvailable(s_ring) == 0 && in_len < 64) {
            if (amp_on && s_stream_eof) { speaker_amp(false); amp_on = false; }
            if (s_out_usb && ++idle_loops > 100) usb_off();   /* let the dongle rest when the radio is off */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        idle_loops = 0;

        size_t avail = xStreamBufferBytesAvailable(s_ring);
        LOCK();
        s_st.buffer_pct = (int)(avail * 100 / RING_SIZE);
        UNLOCK();
        if (!started) {
            if (avail < prebuf && !s_stream_eof) {
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
            started = true;
        }

        size_t room = IN_CHUNK * 2 - in_len;
        size_t n = xStreamBufferReceive(s_ring, in + in_len, room < IN_CHUNK ? room : IN_CHUNK, pdMS_TO_TICKS(30));
        in_len += n;
        if (n == 0 && in_len < 2048) {
            if (!s_stream_eof) {
                /* underrun: wait for the network to catch up */
                started = false;
                prebuf = PREBUF_AGAIN;
                set_state(RADIO_BUFFERING, NULL);
            } else {
                in_len = 0;   /* end of a finite stream: drop the tail */
            }
            continue;
        }

        int type = s_dec_type;
        if (type == DEC_MP3 && !mp3) mp3 = MP3InitDecoder();
        if (type == DEC_AAC && !aac) aac = AACInitDecoder();
        if ((type == DEC_MP3 && !mp3) || (type == DEC_AAC && !aac) || (type != DEC_MP3 && type != DEC_AAC)) {
            set_state(RADIO_ERROR, (type == DEC_MP3 || type == DEC_AAC) ? "Out of memory for the decoder"
                                                                          : "Only MP3 and AAC streams are supported");
            vTaskDelay(pdMS_TO_TICKS(200));
            in_len = 0;
            continue;
        }

        unsigned char *p = in;
        int left = (int)in_len;
        while (left > 0 && my_session == s_session) {
            int off = type == DEC_MP3 ? MP3FindSyncWord(p, left) : AACFindSyncWord(p, left);
            if (off < 0) {
                /* no frame start in what we have: keep the last bytes in case a sync word straddles */
                if (left > 3) { p += left - 3; left = 3; }
                break;
            }
            p += off;
            left -= off;
            unsigned char *before = p;
            int before_left = left;
            int err = type == DEC_MP3 ? MP3Decode(mp3, &p, &left, pcm, 0) : AACDecode(aac, &p, &left, pcm);
            if (err != 0) {
                bool underflow = type == DEC_MP3 ? (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW)
                                                 : (err == ERR_AAC_INDATA_UNDERFLOW);
                if (underflow) {
                    if (type == DEC_MP3 && err == ERR_MP3_MAINDATA_UNDERFLOW && p != before) continue;
                    p = before;
                    left = before_left;
                    break;   /* need more input */
                }
                if (++errors > 200) {
                    ESP_LOGW(TAG, "decoder keeps failing (%d): resetting", err);
                    if (mp3) { MP3FreeDecoder(mp3); mp3 = MP3InitDecoder(); }
                    if (aac) { AACFreeDecoder(aac); aac = AACInitDecoder(); }
                    errors = 0;
                }
                /* skip past this false sync word */
                p = before + 1;
                left = before_left - 1;
                continue;
            }
            errors = 0;
            uint32_t rate = 0, samples = 0;
            int ch = 1, kbps = 0;
            if (type == DEC_MP3) {
                MP3FrameInfo fi;
                MP3GetLastFrameInfo(mp3, &fi);
                rate = fi.samprate; ch = fi.nChans; samples = fi.outputSamps; kbps = fi.bitrate / 1000;
            } else {
                AACFrameInfo fi;
                AACGetLastFrameInfo(aac, &fi);
                rate = fi.sampRateOut; ch = fi.nChans; samples = fi.outputSamps; kbps = fi.bitRate / 1000;
            }
            if (!rate || !samples || samples > PCM_MAX_SAMPLES) continue;
            if (rate != cur_rate || ch != cur_ch) {
                speaker_set_rate(rate);
                cur_rate = rate;
                cur_ch = ch;
                LOCK();
                s_st.sample_rate = rate;
                s_st.channels = ch;
                if (kbps && !s_st.bitrate_kbps) s_st.bitrate_kbps = kbps;
                UNLOCK();
                ESP_LOGI(TAG, "playing %u Hz, %d ch", (unsigned)rate, ch);
            }
            if (usb_play(pcm, samples, ch, rate)) {
                if (amp_on) { speaker_amp(false); amp_on = false; }
                LOCK();
                if (s_st.state != RADIO_PLAYING) { s_st.state = RADIO_PLAYING; s_st.error[0] = 0; }
                UNLOCK();
                continue;
            }
            if (!amp_on) { speaker_amp(true); amp_on = true; }
            if (ch == 1 && samples * 2 > PCM_MAX_SAMPLES) continue;
            uint32_t frames = to_dual_mono(pcm, samples, ch);
            size_t wrote = 0;
            if (s_spk_ok) i2s_channel_write(s_tx, pcm, frames * 4, &wrote, pdMS_TO_TICKS(500));
            LOCK();
            if (s_st.state != RADIO_PLAYING) { s_st.state = RADIO_PLAYING; s_st.error[0] = 0; }
            UNLOCK();
        }
        if (left > 0 && p != in) memmove(in, p, left);
        in_len = left > 0 ? (uint32_t)left : 0;
        if (in_len >= IN_CHUNK * 2) in_len = 0;   /* never let leftovers fill the buffer */
    }
}

/* ================================================================== stream task */

typedef struct {
    char content_type[64];
    char location[256];
    char icy_name[64];
    int icy_metaint;
    int icy_br;
} hdrs_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    hdrs_t *h = (hdrs_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h) {
        if (!strcasecmp(e->header_key, "Content-Type")) snprintf(h->content_type, sizeof(h->content_type), "%s", e->header_value);
        else if (!strcasecmp(e->header_key, "icy-metaint")) h->icy_metaint = atoi(e->header_value);
        else if (!strcasecmp(e->header_key, "icy-br")) h->icy_br = atoi(e->header_value);
        else if (!strcasecmp(e->header_key, "icy-name")) snprintf(h->icy_name, sizeof(h->icy_name), "%s", e->header_value);
    }
    return ESP_OK;
}

static bool cmd_pending(void) { return uxQueueMessagesWaiting(s_cmdq) > 0; }

static bool ends_with_ci(const char *s, const char *suffix)
{
    const char *q = strchr(s, '?');
    size_t n = q ? (size_t)(q - s) : strlen(s), m = strlen(suffix);
    return n >= m && strncasecmp(s + n - m, suffix, m) == 0;
}

static int type_from_content(const char *ct)
{
    char lc[64];
    size_t i;
    for (i = 0; ct[i] && i < sizeof(lc) - 1; i++) lc[i] = tolower((unsigned char)ct[i]);
    lc[i] = 0;
    if (strstr(lc, "mpeg") || strstr(lc, "mp3")) return DEC_MP3;
    if (strstr(lc, "aac")) return DEC_AAC;
    if (strstr(lc, "ogg") || strstr(lc, "opus") || strstr(lc, "vorbis")) return DEC_OGG;
    if (strstr(lc, "flac")) return DEC_FLAC;
    if (strstr(lc, "mp4") || strstr(lc, "m4a")) return DEC_M4A;
    return 0;
}

static int type_from_bytes(const uint8_t *b, size_t n)
{
    if (n < 4) return 0;
    if (!memcmp(b, "OggS", 4)) return DEC_OGG;
    if (!memcmp(b, "fLaC", 4)) return DEC_FLAC;
    if (!memcmp(b, "ID3", 3)) return DEC_MP3;
    for (size_t i = 0; i + 1 < n && i < 2048; i++) {
        if (b[i] == 0xFF && (b[i + 1] & 0xF6) == 0xF0) return DEC_AAC;   /* ADTS */
        if (b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0) return DEC_MP3;
    }
    return 0;
}

static const char *type_name(int t)
{
    switch (t) {
    case DEC_MP3: return "MP3";
    case DEC_AAC: return "AAC";
    case DEC_OGG: return "OGG";
    case DEC_FLAC: return "FLAC";
    case DEC_M4A: return "M4A";
    default: return "?";
    }
}

static void parse_icy_meta(const char *m)
{
    const char *p = strstr(m, "StreamTitle='");
    if (!p) return;
    p += 13;
    const char *e = strstr(p, "';");
    if (!e) e = p + strlen(p);
    size_t n = e - p;
    char t[160];
    if (n >= sizeof(t)) n = sizeof(t) - 1;
    memcpy(t, p, n);
    t[n] = 0;
    /* keep it printable for the Montserrat font (ASCII) */
    for (char *c = t; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    LOCK();
    snprintf(s_st.title, sizeof(s_st.title), "%s", t);
    UNLOCK();
}

/* Pull the first stream URL out of a .pls / .m3u body. */
static bool playlist_first_url(const char *body, char *out, size_t n)
{
    const char *p = body;
    while (*p) {
        const char *line = p;
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *h = NULL;
        for (size_t i = 0; i + 7 < len; i++) {
            if (!strncasecmp(line + i, "http://", 7) || !strncasecmp(line + i, "https://", 8)) { h = line + i; break; }
        }
        if (h && line[0] != '#') {
            size_t l = len - (h - line);
            if (l >= n) l = n - 1;
            memcpy(out, h, l);
            out[l] = 0;
            while (l && isspace((unsigned char)out[l - 1])) out[--l] = 0;
            return true;
        }
        if (!eol) break;
        p = eol + 1;
        while (*p == '\r' || *p == '\n') p++;
    }
    return false;
}

typedef enum { RC_CMD, RC_EOF, RC_RETRY, RC_FATAL } rc_t;

static esp_http_client_handle_t open_url(const char *url, hdrs_t *h, int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt,
        .user_data = h,
        .timeout_ms = 10000,
        .buffer_size = 8192,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = USER_AGENT,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    esp_http_client_set_header(c, "Icy-MetaData", "1");
    esp_http_client_set_header(c, "Accept", "*/*");
    for (int redir = 0; redir < 6; redir++) {
        memset(h, 0, sizeof(*h));
        if (esp_http_client_open(c, 0) != ESP_OK) {
            esp_http_client_cleanup(c);
            *status = -1;
            return NULL;
        }
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        return c;
    }
    esp_http_client_cleanup(c);
    *status = -2;
    return NULL;
}

static void begin_session(void)
{
    s_session_active = false;
    s_stream_eof = false;
    s_session++;
    for (int i = 0; i < 100 && s_dec_session != s_session; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

static rc_t play_stream(const cmd_t *c)
{
    char url[256];
    snprintf(url, sizeof(url), "%s", c->url);
    hdrs_t h;
    int status = 0;
    esp_http_client_handle_t cl = NULL;

    set_state(RADIO_CONNECTING, NULL);
    for (int depth = 0; depth < 3; depth++) {
        if (ends_with_ci(url, ".m3u8")) {
            set_state(RADIO_ERROR, "HLS (.m3u8) streams are not supported yet");
            return RC_FATAL;
        }
        cl = open_url(url, &h, &status);
        if (!cl) {
            set_state(RADIO_RECONNECTING, status == -2 ? "Too many redirects" : "Could not connect");
            return RC_RETRY;
        }
        if (status != 200) {
            char e[64];
            snprintf(e, sizeof(e), "Server answered HTTP %d", status);
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            if (status >= 400 && status < 500) { set_state(RADIO_ERROR, e); return RC_FATAL; }
            set_state(RADIO_RECONNECTING, e);
            return RC_RETRY;
        }
        bool is_pls = strcasestr(h.content_type, "scpls") || strcasestr(h.content_type, "mpegurl") ||
                      ends_with_ci(url, ".pls") || ends_with_ci(url, ".m3u");
        if (strcasestr(h.content_type, "vnd.apple.mpegurl")) {
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
            set_state(RADIO_ERROR, "HLS streams are not supported yet");
            return RC_FATAL;
        }
        if (!is_pls) break;
        /* playlist: read it and follow the first entry */
        char *body = calloc(1, 16 * 1024);
        int got = 0, r;
        while (body && got < 16 * 1024 - 1 && (r = esp_http_client_read(cl, body + got, 16 * 1024 - 1 - got)) > 0) got += r;
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
        cl = NULL;
        bool ok = body && playlist_first_url(body, url, sizeof(url));
        free(body);
        if (!ok) { set_state(RADIO_ERROR, "Playlist contains no stream address"); return RC_FATAL; }
        ESP_LOGI(TAG, "playlist -> %s", url);
    }
    if (!cl) { set_state(RADIO_ERROR, "Playlist nesting too deep"); return RC_FATAL; }

    int type = type_from_content(h.content_type);
    LOCK();
    s_st.bitrate_kbps = h.icy_br;
    if (!c->name[0] && h.icy_name[0]) snprintf(s_st.name, sizeof(s_st.name), "%s", h.icy_name);
    UNLOCK();
    ESP_LOGI(TAG, "connected: %s, type %s, metaint %d", h.content_type, type_name(type), h.icy_metaint);

    uint8_t *buf = heap_caps_malloc(IN_CHUNK, MALLOC_CAP_SPIRAM);
    char *meta = malloc(4096 + 1);
    int until_meta = h.icy_metaint;
    rc_t rc = RC_EOF;
    bool first = true;
    while (!cmd_pending()) {
        int want = IN_CHUNK;
        if (h.icy_metaint > 0 && until_meta < want) want = until_meta;
        int n = esp_http_client_read(cl, (char *)buf, want);
        if (n < 0) { rc = RC_RETRY; set_state(RADIO_RECONNECTING, "Connection lost"); break; }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(cl)) { rc = RC_EOF; break; }
            rc = RC_RETRY;
            set_state(RADIO_RECONNECTING, "Stream stalled");
            break;
        }
        if (first) {
            first = false;
            if (!type) type = type_from_bytes(buf, n);
            if (!type) {
                set_state(RADIO_ERROR, "Unknown audio format");
                rc = RC_FATAL;
                break;
            }
            if (type != DEC_MP3 && type != DEC_AAC) {
                set_state(RADIO_ERROR, "Only MP3 and AAC streams are supported");
                rc = RC_FATAL;
                break;
            }
            s_dec_type = type;
            s_session_active = true;
            LOCK();
            snprintf(s_st.codec, sizeof(s_st.codec), "%s", type_name(type));
            if (s_st.state == RADIO_CONNECTING) s_st.state = RADIO_BUFFERING;
            UNLOCK();
        }
        size_t off = 0;
        while (off < (size_t)n && !cmd_pending()) off += xStreamBufferSend(s_ring, buf + off, n - off, pdMS_TO_TICKS(100));
        if (h.icy_metaint > 0) {
            until_meta -= n;
            if (until_meta == 0) {
                uint8_t lenb = 0;
                if (esp_http_client_read(cl, (char *)&lenb, 1) != 1) { rc = RC_RETRY; break; }
                int mlen = lenb * 16, got = 0;
                while (got < mlen) {
                    int r = esp_http_client_read(cl, meta + got, mlen - got);
                    if (r <= 0) break;
                    got += r;
                }
                meta[got] = 0;
                if (got) parse_icy_meta(meta);
                until_meta = h.icy_metaint;
            }
        }
    }
    if (cmd_pending()) rc = RC_CMD;
    free(buf);
    free(meta);
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    s_session_active = false;
    if (rc == RC_EOF) s_stream_eof = true;
    return rc;
}

static void stream_task(void *arg)
{
    static const int backoff_s[] = {2, 4, 8, 15, 30};
    cmd_t cur = {0};
    bool active = false;
    int retries = 0;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(s_cmdq, &c, active ? 0 : portMAX_DELAY) == pdTRUE) {
            if (c.type == CMD_STOP) {
                active = false;
                begin_session();
                LOCK();
                memset(&s_st, 0, sizeof(s_st));
                s_st.state = RADIO_IDLE;
                UNLOCK();
                continue;
            }
            cur = c;
            active = true;
            retries = 0;
            LOCK();
            memset(&s_st, 0, sizeof(s_st));
            snprintf(s_st.name, sizeof(s_st.name), "%s", c.name);
            snprintf(s_st.url, sizeof(s_st.url), "%s", c.url);
            s_st.state = RADIO_CONNECTING;
            UNLOCK();
        }
        if (!active) continue;

        begin_session();
        rc_t rc = play_stream(&cur);
        if (rc == RC_CMD) continue;
        if (rc == RC_FATAL) {
            active = false;
            s_session_active = false;
            continue;
        }
        if (rc == RC_RETRY || rc == RC_EOF) {
            /* let the buffered audio finish, then reconnect with backoff */
            int wait = backoff_s[retries < 5 ? retries : 4];
            if (retries > 0 || rc == RC_RETRY) {
                LOCK();
                s_st.state = RADIO_RECONNECTING;
                UNLOCK();
            }
            retries++;
            if (xQueuePeek(s_cmdq, &c, pdMS_TO_TICKS(wait * 1000)) == pdTRUE) continue;
        }
    }
}

/* ================================================================== search (radio-browser.info) */

typedef struct {
    char query[96];
    radio_search_cb_t cb;
    void *user;
} search_req_t;

typedef struct {
    char *buf;
    int len, cap;
} grow_t;

static esp_err_t search_evt(esp_http_client_event_t *e)
{
    grow_t *g = (grow_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && g) {
        if (g->len + e->data_len + 1 > g->cap) {
            int ncap = (g->len + e->data_len + 1) * 2;
            if (ncap > 768 * 1024) return ESP_FAIL;
            char *nb = heap_caps_realloc(g->buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_FAIL;
            g->buf = nb;
            g->cap = ncap;
        }
        memcpy(g->buf + g->len, e->data, e->data_len);
        g->len += e->data_len;
        g->buf[g->len] = 0;
    }
    return ESP_OK;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') out[o++] = ch;
        else if (ch == ' ') { out[o++] = '%'; out[o++] = '2'; out[o++] = '0'; }
        else { out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15]; }
    }
    out[o] = 0;
}

static void search_task(void *arg)
{
    search_req_t *rq = arg;
    static const char *mirrors[] = {"de1.api.radio-browser.info", "de2.api.radio-browser.info",
                                    "fi1.api.radio-browser.info", "nl1.api.radio-browser.info"};
    char q[300];
    url_encode(rq->query, q, sizeof(q));
    grow_t g = {0};
    const char *err = "Search failed - is the panel online?";
    for (size_t m = 0; m < sizeof(mirrors) / sizeof(mirrors[0]); m++) {
        char url[512];
        snprintf(url, sizeof(url),
                 "https://%s/json/stations/search?name=%s&limit=60&hidebroken=true&order=clickcount&reverse=true",
                 mirrors[m], q);
        g.len = 0;
        esp_http_client_config_t cfg = {
            .url = url, .event_handler = search_evt, .user_data = &g, .timeout_ms = 10000,
            .crt_bundle_attach = esp_crt_bundle_attach, .user_agent = USER_AGENT,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        esp_err_t e = esp_http_client_perform(c);
        int st = esp_http_client_get_status_code(c);
        esp_http_client_cleanup(c);
        if (e == ESP_OK && st == 200 && g.len > 0) { err = NULL; break; }
    }

    radio_search_rec_t *recs = calloc(30, sizeof(radio_search_rec_t));
    int n = 0;
    if (!err && recs) {
        cJSON *arr = cJSON_Parse(g.buf);
        cJSON *o;
        cJSON_ArrayForEach(o, arr) {
            if (n >= 30) break;
            cJSON *name = cJSON_GetObjectItem(o, "name");
            cJSON *u = cJSON_GetObjectItem(o, "url_resolved");
            cJSON *codec = cJSON_GetObjectItem(o, "codec");
            cJSON *br = cJSON_GetObjectItem(o, "bitrate");
            cJSON *cc = cJSON_GetObjectItem(o, "countrycode");
            cJSON *tags = cJSON_GetObjectItem(o, "tags");
            if (!cJSON_IsString(u) || !cJSON_IsString(name)) continue;
            const char *cs = cJSON_IsString(codec) ? codec->valuestring : "";
            bool ok = !strcasecmp(cs, "MP3") || !strcasecmp(cs, "AAC") || !strcasecmp(cs, "AAC+");
            if (!ok || strcasestr(u->valuestring, ".m3u8") || strlen(u->valuestring) >= sizeof(recs[0].url)) continue;
            radio_search_rec_t *r = &recs[n++];
            snprintf(r->name, sizeof(r->name), "%s", name->valuestring);
            for (char *p = r->name; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
            snprintf(r->url, sizeof(r->url), "%s", u->valuestring);
            snprintf(r->codec, sizeof(r->codec), "%s", cs);
            snprintf(r->country, sizeof(r->country), "%s", cJSON_IsString(cc) ? cc->valuestring : "");
            snprintf(r->tags, sizeof(r->tags), "%s", cJSON_IsString(tags) ? tags->valuestring : "");
            r->bitrate = cJSON_IsNumber(br) ? br->valueint : 0;
        }
        cJSON_Delete(arr);
        if (n == 0) err = "No playable stations found";
    }
    free(g.buf);
    if (lvgl_port_lock(0)) {
        rq->cb(recs, n, err, rq->user);
        lvgl_port_unlock();
    }
    free(recs);
    free(rq);
    vTaskDelete(NULL);
}

/* ================================================================== public */

void radio_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_cmdq = xQueueCreate(4, sizeof(cmd_t));
    s_ring = xStreamBufferCreateWithCaps(RING_SIZE, 1, MALLOC_CAP_SPIRAM);
    s_st.state = RADIO_IDLE;
    if (speaker_init() != ESP_OK) ESP_LOGE(TAG, "speaker codec init failed");
    xTaskCreatePinnedToCoreWithCaps(decode_task, "radio_dec", 12 * 1024, NULL, 7, NULL, 0, PSRAM_STACK);
    xTaskCreatePinnedToCoreWithCaps(stream_task, "radio_net", 8 * 1024, NULL, 6, NULL, 0, PSRAM_STACK);
}

void radio_play(const char *name, const char *url)
{
    cmd_t c = {.type = CMD_PLAY};
    snprintf(c.name, sizeof(c.name), "%s", name ? name : "");
    snprintf(c.url, sizeof(c.url), "%s", url);
    xQueueReset(s_cmdq);
    xQueueSend(s_cmdq, &c, 0);
}

void radio_stop(void)
{
    cmd_t c = {.type = CMD_STOP};
    xQueueReset(s_cmdq);
    xQueueSend(s_cmdq, &c, 0);
}

void radio_output_usb(bool prefer) { s_pref_usb = prefer; }
void radio_force_speaker(bool on) { s_force_spk = on; }
bool radio_output_is_usb(void) { return s_out_usb; }

void radio_buzzer(int kind)
{
    s_buzz = kind < 0 || kind > 2 ? 0 : kind;
}

void radio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (s_spk_ok) es8311_set_volume(percent);
}

void radio_get_status(radio_status_t *st)
{
    if (!s_mtx) { memset(st, 0, sizeof(*st)); return; }
    LOCK();
    *st = s_st;
    UNLOCK();
}

void radio_search(const char *query, radio_search_cb_t cb, void *user)
{
    search_req_t *rq = calloc(1, sizeof(*rq));
    snprintf(rq->query, sizeof(rq->query), "%s", query);
    rq->cb = cb;
    rq->user = user;
    if (xTaskCreate(search_task, "rsearch", 8192, rq, 3, NULL) != pdPASS) {
        free(rq);
        cb(NULL, 0, "Out of memory", user);
    }
}
