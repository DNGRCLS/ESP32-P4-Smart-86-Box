#pragma once
#include <stdbool.h>

typedef enum {
    HOL_STAT = 0,      /* statutory in the chosen province/territory */
    HOL_FEDERAL,       /* federal statutory only (federal employees / federally regulated) */
    HOL_OPTIONAL,      /* optional or widely observed, not statutory there */
    HOL_OBSERVANCE,    /* cultural dates: Mother's Day, Halloween, ... */
} hol_kind_t;

typedef struct {
    unsigned char month;   /* 1..12 */
    unsigned char day;     /* 1..31 */
    unsigned char kind;    /* hol_kind_t */
    char name[48];
} holiday_t;

/* Fills out[] (sorted by date) for one year. prov is a two-letter code ("ON").
   dst adds "clocks forward/back" reminders. Returns the number of entries. */
int holidays_for_year(int year, const char *prov, bool dst, holiday_t *out, int max);

int days_in_month(int year, int month);
int day_of_week(int year, int month, int day);   /* 0 = Sunday */
const char *province_name(const char *code);
extern const char *const g_province_codes[13];
