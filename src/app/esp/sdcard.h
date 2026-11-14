#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_MOUNT "/sdcard"

bool sdcard_mount(char *err, int errlen);
void sdcard_unmount(void);
uint64_t sdcard_total_bytes(void);
uint64_t sdcard_used_bytes(void);

/* photos.c */
void photos_init(void);
/* Full path on the VFS for a card-relative path; false if it tries to escape the card. */
bool photos_abs_path(const char *rel, char *out, int n);

#ifdef __cplusplus
}
#endif
