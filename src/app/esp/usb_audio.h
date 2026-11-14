#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usb_audio_init(void);
bool usb_audio_connected(void);         /* a USB Audio 1.0 speaker / Bluetooth transmitter is plugged in */
uint32_t usb_audio_gen(void);           /* changes on every connect / disconnect */
void usb_audio_name(char *out, size_t n);
uint32_t usb_audio_rate(void);          /* rate of the running stream, 0 = stopped */
/* Starts (or re-starts) the stream for a source rate; returns the device rate used (0 = failed). */
uint32_t usb_audio_start(uint32_t stream_rate);
void usb_audio_stop(void);
/* Interleaved stereo s16 at the device rate; blocks while the buffer is full. false = device gone. */
bool usb_audio_write(const int16_t *pcm, uint32_t frames, uint32_t timeout_ms);
