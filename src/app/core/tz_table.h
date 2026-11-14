#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "settings.h"

typedef struct {
    const char *id;        /* stable key stored in settings */
    const char *region;    /* group label */
    const char *city;      /* display name */
    const char *std_abbr;  /* standard-time abbreviation */
    int std_offset_min;    /* minutes EAST of UTC for standard time (e.g. -300 for EST) */
    const char *dst_abbr;  /* NULL if the zone has no seasonal change */
    const char *dst_rule;  /* POSIX rule "start,end", NULL if no seasonal change */
    const char *note;      /* optional remark shown in UI */
} tz_entry_t;

extern const tz_entry_t g_tz_table[];
extern const size_t g_tz_count;
extern const char *const g_tz_regions[];
extern const size_t g_tz_region_count;

const tz_entry_t *tz_find(const char *id);
int tz_index(const char *id);

/* Builds the POSIX TZ string honouring dst_mode and the custom override. */
void tz_build_posix(const settings_t *s, char *out, size_t out_len);
