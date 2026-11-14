/*
 * Canadian holidays.  Statutory/optional lists per province follow canada-holidays.ca (checked Oct 2026).
 */
#include "holidays.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const g_province_codes[13] = {"AB", "BC", "MB", "NB", "NL", "NS", "NT", "NU", "ON", "PE", "QC", "SK", "YT"};
static const char *const s_province_names[13] = {
    "Alberta", "British Columbia", "Manitoba", "New Brunswick", "Newfoundland and Labrador", "Nova Scotia",
    "Northwest Territories", "Nunavut", "Ontario", "Prince Edward Island", "Quebec", "Saskatchewan", "Yukon"};

const char *province_name(const char *code)
{
    for (int i = 0; i < 13; i++) if (!strcmp(code, g_province_codes[i])) return s_province_names[i];
    return code;
}

/* ------------------------------------------------------------------ date math */

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}

int day_of_week(int y, int m, int d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* n-th weekday (1-based) of a month; n = -1 for the last */
static int nth_wd(int y, int m, int wd, int n)
{
    if (n > 0) {
        int first = day_of_week(y, m, 1);
        return 1 + (wd - first + 7) % 7 + (n - 1) * 7;
    }
    int dim = days_in_month(y, m);
    int last = day_of_week(y, m, dim);
    return dim - (last - wd + 7) % 7;
}

/* Monday on or before the given day (Victoria Day = Monday before May 25) */
static int monday_before(int y, int m, int d)
{
    int wd = day_of_week(y, m, d);
    int back = (wd - 1 + 7) % 7;
    if (back == 0) back = 7;
    return d - back;
}

static void easter(int y, int *m, int *d)
{
    int a = y % 19, b = y / 100, c = y % 100, dd = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
    int h = (19 * a + b - dd - g + 15) % 30, i = c / 4, k = c % 4, l = (32 + 2 * e + 2 * i - h - k) % 7;
    int mm = (a + 11 * h + 22 * l) / 451;
    *m = (h + l - 7 * mm + 114) / 31;
    *d = ((h + l - 7 * mm + 114) % 31) + 1;
}

/* move a (month, day) by n days */
static void shift(int y, int *m, int *d, int n)
{
    *d += n;
    while (*d < 1) { (*m)--; *d += days_in_month(y, *m); }
    while (*d > days_in_month(y, *m)) { *d -= days_in_month(y, *m); (*m)++; }
}

/* nearest Monday to a date (Newfoundland government holidays) */
static void nearest_monday(int y, int *m, int *d)
{
    int wd = day_of_week(y, *m, *d);
    int delta = wd == 0 ? 1 : (wd <= 4 ? 1 - wd : 8 - wd);   /* Tue..Thu go back, Fri/Sat forward */
    shift(y, m, d, delta);
}

/* ------------------------------------------------------------------ table */

typedef struct {
    holiday_t *out;
    int n, max;
} list_t;

static void add(list_t *l, int m, int d, hol_kind_t k, const char *name)
{
    if (l->n >= l->max) return;
    for (int i = 0; i < l->n; i++)   /* one entry per name+date (stat beats optional) */
        if (l->out[i].month == m && l->out[i].day == d && !strcmp(l->out[i].name, name)) {
            if (k < l->out[i].kind) l->out[i].kind = (unsigned char)k;
            return;
        }
    holiday_t *h = &l->out[l->n++];
    h->month = (unsigned char)m;
    h->day = (unsigned char)d;
    h->kind = (unsigned char)k;
    snprintf(h->name, sizeof(h->name), "%s", name);
}

/* Stat holidays on a weekend get an observed weekday (shown separately). */
static void add_observed(list_t *l, int y, int m, int d, hol_kind_t k, const char *name, int sat_shift, int sun_shift)
{
    add(l, m, d, k, name);
    if (k > HOL_FEDERAL) return;
    int wd = day_of_week(y, m, d);
    int s = wd == 6 ? sat_shift : wd == 0 ? sun_shift : 0;
    if (!s) return;
    int mm = m, dd = d;
    shift(y, &mm, &dd, s);
    if (mm < 1 || mm > 12) return;
    char nm[48];
    snprintf(nm, sizeof(nm), "%s (observed)", name);
    add(l, mm, dd, k, nm);
}

static bool in(const char *prov, const char *list)
{
    return strstr(list, prov) != NULL;
}

static int cmp(const void *a, const void *b)
{
    const holiday_t *x = a, *y = b;
    if (x->month != y->month) return x->month - y->month;
    if (x->day != y->day) return x->day - y->day;
    return x->kind - y->kind;
}

int holidays_for_year(int y, const char *p, bool dst, holiday_t *out, int max)
{
    list_t L = {out, 0, max};
    int em, ed;
    easter(y, &em, &ed);
    int gm = em, gd = ed;
    shift(y, &gm, &gd, -2);       /* Good Friday */
    int mm = em, md = ed;
    shift(y, &mm, &md, 1);        /* Easter Monday */
    int vic = monday_before(y, 5, 25);
    int feb3 = nth_wd(y, 2, 1, 3);
    int aug1 = nth_wd(y, 8, 1, 1);
    int sep1 = nth_wd(y, 9, 1, 1);
    int oct2 = nth_wd(y, 10, 1, 2);

    /* ---- January ---- */
    add_observed(&L, y, 1, 1, HOL_STAT, "New Year's Day", 2, 1);
    if (in(p, "QC")) add(&L, 1, 2, HOL_OPTIONAL, "Day after New Year's");

    /* ---- February ---- */
    add(&L, 2, 2, HOL_OBSERVANCE, "Groundhog Day");
    add(&L, 2, 14, HOL_OBSERVANCE, "Valentine's Day");
    add(&L, 2, 15, HOL_OBSERVANCE, "National Flag of Canada Day");
    if (in(p, "AB BC NB ON SK")) add(&L, 2, feb3, HOL_STAT, "Family Day");
    else if (in(p, "MB")) add(&L, 2, feb3, HOL_STAT, "Louis Riel Day");
    else if (in(p, "NS")) add(&L, 2, feb3, HOL_STAT, "Heritage Day");
    else if (in(p, "PE")) add(&L, 2, feb3, HOL_STAT, "Islander Day");

    /* ---- March / April ---- */
    if (dst) add(&L, 3, nth_wd(y, 3, 0, 2), HOL_OBSERVANCE, "Clocks go forward (DST)");
    if (in(p, "NL")) {
        int m = 3, d = 17;
        nearest_monday(y, &m, &d);
        add(&L, m, d, HOL_STAT, "Saint Patrick's Day");
        m = 4; d = 23;
        nearest_monday(y, &m, &d);
        add(&L, m, d, HOL_STAT, "Saint George's Day");
    } else {
        add(&L, 3, 17, HOL_OBSERVANCE, "Saint Patrick's Day");
    }
    add(&L, 4, 1, HOL_OBSERVANCE, "April Fools' Day");
    add(&L, gm, gd, HOL_STAT, "Good Friday");
    add(&L, em, ed, HOL_OBSERVANCE, "Easter Sunday");
    if (in(p, "AB BC MB NS ON")) add(&L, mm, md, HOL_OPTIONAL, "Easter Monday");
    else add(&L, mm, md, HOL_FEDERAL, "Easter Monday");
    add(&L, 4, 22, HOL_OBSERVANCE, "Earth Day");

    /* ---- May / June ---- */
    add(&L, 5, nth_wd(y, 5, 0, 2), HOL_OBSERVANCE, "Mother's Day");
    if (in(p, "QC")) add(&L, 5, vic, HOL_STAT, "National Patriots' Day");
    else if (in(p, "NB")) add(&L, 5, vic, HOL_OPTIONAL, "Victoria Day");
    else if (in(p, "NL NS PE")) add(&L, 5, vic, HOL_FEDERAL, "Victoria Day");
    else add(&L, 5, vic, HOL_STAT, "Victoria Day");
    add(&L, 6, nth_wd(y, 6, 0, 3), HOL_OBSERVANCE, "Father's Day");
    if (in(p, "NT YT")) add(&L, 6, 21, HOL_STAT, "National Indigenous Peoples Day");
    else add(&L, 6, 21, HOL_OBSERVANCE, "National Indigenous Peoples Day");
    if (in(p, "QC")) add(&L, 6, 24, HOL_STAT, "Saint-Jean-Baptiste Day");
    if (in(p, "NL")) {
        int m = 6, d = 24;
        nearest_monday(y, &m, &d);
        add(&L, m, d, HOL_STAT, "Discovery Day");
    }

    /* ---- July ---- */
    if (in(p, "NL")) {
        add(&L, 7, 1, HOL_STAT, "Memorial Day");
        add(&L, 7, 1, HOL_FEDERAL, "Canada Day");
        int m = 7, d = 12;
        nearest_monday(y, &m, &d);
        add(&L, m, d, HOL_STAT, "Orangemen's Day");
    } else {
        /* Canada Day on a Sunday is observed Monday July 2 */
        add(&L, 7, 1, HOL_STAT, "Canada Day");
        if (day_of_week(y, 7, 1) == 0) add(&L, 7, 2, HOL_STAT, "Canada Day (observed)");
    }
    if (in(p, "NU")) add(&L, 7, 9, HOL_STAT, "Nunavut Day");

    /* ---- August ---- */
    if (in(p, "BC")) add(&L, 8, aug1, HOL_STAT, "British Columbia Day");
    else if (in(p, "NB")) add(&L, 8, aug1, HOL_STAT, "New Brunswick Day");
    else if (in(p, "SK")) add(&L, 8, aug1, HOL_STAT, "Saskatchewan Day");
    else if (in(p, "NT NU")) add(&L, 8, aug1, HOL_STAT, "Civic Holiday");
    else if (in(p, "AB")) add(&L, 8, aug1, HOL_OPTIONAL, "Heritage Day");
    else if (in(p, "MB")) add(&L, 8, aug1, HOL_OPTIONAL, "Terry Fox Day");
    else if (in(p, "ON")) add(&L, 8, aug1, HOL_OPTIONAL, "Civic Holiday");
    else add(&L, 8, aug1, HOL_FEDERAL, "Civic Holiday");
    if (in(p, "NL")) add(&L, 8, nth_wd(y, 8, 3, 1), HOL_STAT, "Regatta Day (St. John's)");
    if (in(p, "YT")) add(&L, 8, nth_wd(y, 8, 1, 3), HOL_STAT, "Discovery Day");

    /* ---- September / October ---- */
    add(&L, 9, sep1, HOL_STAT, "Labour Day");
    if (in(p, "BC NT PE YT")) add(&L, 9, 30, HOL_STAT, "National Day for Truth and Reconciliation");
    else if (in(p, "MB")) add(&L, 9, 30, HOL_STAT, "Orange Shirt Day (Truth and Reconciliation)");
    else if (in(p, "AB NB NU")) add(&L, 9, 30, HOL_OPTIONAL, "National Day for Truth and Reconciliation");
    else add(&L, 9, 30, HOL_FEDERAL, "National Day for Truth and Reconciliation");
    if (in(p, "NB NS")) add(&L, 10, oct2, HOL_OPTIONAL, "Thanksgiving");
    else if (in(p, "NL PE")) add(&L, 10, oct2, HOL_FEDERAL, "Thanksgiving");
    else add(&L, 10, oct2, HOL_STAT, "Thanksgiving");
    add(&L, 10, 31, HOL_OBSERVANCE, "Halloween");

    /* ---- November / December ---- */
    if (dst) add(&L, 11, nth_wd(y, 11, 0, 1), HOL_OBSERVANCE, "Clocks go back (DST ends)");
    if (in(p, "ON MB")) add(&L, 11, 11, HOL_OPTIONAL, "Remembrance Day");
    else if (in(p, "NS QC")) add(&L, 11, 11, HOL_FEDERAL, "Remembrance Day");
    else add(&L, 11, 11, HOL_STAT, "Remembrance Day");
    add(&L, 12, 24, HOL_OBSERVANCE, "Christmas Eve");
    {
        /* Christmas / Boxing Day observed dates */
        int wd = day_of_week(y, 12, 25);
        add(&L, 12, 25, HOL_STAT, "Christmas Day");
        hol_kind_t bk = in(p, "ON NL") ? HOL_STAT : in(p, "AB BC MB NS NB") ? HOL_OPTIONAL : HOL_FEDERAL;
        add(&L, 12, 26, bk, "Boxing Day");
        if (wd == 6) {          /* Sat: Christmas -> Mon 27, Boxing -> Tue 28 */
            add(&L, 12, 27, HOL_STAT, "Christmas Day (observed)");
            add(&L, 12, 28, bk, "Boxing Day (observed)");
        } else if (wd == 0) {   /* Sun: Christmas -> Tue 27? Boxing Mon 26, Christmas observed Tue 27 */
            add(&L, 12, 27, HOL_STAT, "Christmas Day (observed)");
        } else if (wd == 5) {   /* Fri: Boxing Day Sat -> Mon 28 */
            add(&L, 12, 28, bk, "Boxing Day (observed)");
        }
    }
    add(&L, 12, 31, HOL_OBSERVANCE, "New Year's Eve");

    qsort(out, L.n, sizeof(holiday_t), cmp);
    return L.n;
}
