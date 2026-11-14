/*
 * Time-zone table.  Offsets are minutes EAST of UTC.  Rules use POSIX TZ syntax.
 *
 * Canadian status as of October 2026:
 *   - British Columbia: permanent daylight time since March 2026 (except eastern BC).
 *   - Manitoba: permanent daylight time from 1 Nov 2026 (no fall-back).
 *   - NW Ontario (Kenora, west of 90 W): permanent daylight proposed for 1 Nov 2026.
 *   - Alberta: permanent daylight legislation pending; seasonal changes still apply.
 *   - Saskatchewan, Yukon: no seasonal change.
 * The DST override in Settings and the custom POSIX field cover any later change.
 */
#include "tz_table.h"
#include <stdio.h>
#include <string.h>

#define NA_RULE "M3.2.0,M11.1.0"
#define EU_RULE "M3.5.0,M10.5.0/3"

/* perm_dst zones are encoded with dst_rule = "PERM" */
#define PERM "PERM"

const char *const g_tz_regions[] = {
    "Canada", "United States & Mexico", "Caribbean & South America", "Europe",
    "Middle East & Africa", "Asia", "Oceania", "UTC",
};
const size_t g_tz_region_count = sizeof(g_tz_regions) / sizeof(g_tz_regions[0]);

const tz_entry_t g_tz_table[] = {
    /* ---- Canada ---- */
    {"ca_stjohns",     "Canada", "St. John's, NL (Newfoundland)",     "NST", -210, "NDT", NA_RULE, NULL},
    {"ca_halifax",     "Canada", "Halifax / Moncton / PEI (Atlantic)", "AST", -240, "ADT", NA_RULE, NULL},
    {"ca_blancsablon", "Canada", "Blanc-Sablon, QC (Atlantic, no DST)", "AST", -240, NULL, NULL, NULL},
    {"ca_toronto",     "Canada", "Toronto / Ottawa (Eastern)",         "EST", -300, "EDT", NA_RULE, NULL},
    {"ca_montreal",    "Canada", "Montreal / Quebec City (Eastern)",   "EST", -300, "EDT", NA_RULE, NULL},
    {"ca_iqaluit",     "Canada", "Iqaluit, NU (Eastern)",              "EST", -300, "EDT", NA_RULE, NULL},
    {"ca_atikokan",    "Canada", "Atikokan, ON (Eastern, no DST)",     "EST", -300, NULL, NULL, NULL},
    {"ca_kenora",      "Canada", "Kenora / NW Ontario (Central)",      "CST", -360, "CDT", NA_RULE,
     "Permanent daylight time proposed from Nov 1, 2026. Choose 'Always daylight' if adopted."},
    {"ca_winnipeg",    "Canada", "Winnipeg (Manitoba)",                "CST", -360, "CDT", PERM,
     "Manitoba keeps daylight time year-round from Nov 1, 2026."},
    {"ca_rankin",      "Canada", "Rankin Inlet, NU (Central)",         "CST", -360, "CDT", NA_RULE, NULL},
    {"ca_regina",      "Canada", "Regina / Saskatoon (Saskatchewan)",  "CST", -360, NULL, NULL, NULL},
    {"ca_edmonton",    "Canada", "Edmonton / Calgary (Mountain)",      "MST", -420, "MDT", NA_RULE,
     "Alberta permanent daylight time is pending. Choose 'Always daylight' if it takes effect."},
    {"ca_yellowknife", "Canada", "Yellowknife, NT (Mountain)",         "MST", -420, "MDT", NA_RULE, NULL},
    {"ca_cambridge",   "Canada", "Cambridge Bay, NU (Mountain)",       "MST", -420, "MDT", NA_RULE, NULL},
    {"ca_cranbrook",   "Canada", "Cranbrook / Golden, BC (Mountain)",  "MST", -420, "MDT", NA_RULE, NULL},
    {"ca_dawsoncreek", "Canada", "Dawson Creek / Fort St. John, BC",   "MST", -420, NULL, NULL, NULL},
    {"ca_whitehorse",  "Canada", "Whitehorse / Dawson (Yukon)",        "MST", -420, NULL, NULL, NULL},
    {"ca_vancouver",   "Canada", "Vancouver / Victoria (BC)",          "PST", -480, "PCT", PERM,
     "BC has kept daylight time year-round since March 2026."},

    /* ---- United States & Mexico ---- */
    {"us_newyork",     "United States & Mexico", "New York / Miami (Eastern)",  "EST", -300, "EDT", NA_RULE, NULL},
    {"us_chicago",     "United States & Mexico", "Chicago / Dallas (Central)",  "CST", -360, "CDT", NA_RULE, NULL},
    {"us_denver",      "United States & Mexico", "Denver (Mountain)",           "MST", -420, "MDT", NA_RULE, NULL},
    {"us_phoenix",     "United States & Mexico", "Phoenix (Arizona)",           "MST", -420, NULL, NULL, NULL},
    {"us_losangeles",  "United States & Mexico", "Los Angeles / Seattle (Pacific)", "PST", -480, "PDT", NA_RULE, NULL},
    {"us_anchorage",   "United States & Mexico", "Anchorage (Alaska)",          "AKST", -540, "AKDT", NA_RULE, NULL},
    {"us_honolulu",    "United States & Mexico", "Honolulu (Hawaii)",           "HST", -600, NULL, NULL, NULL},
    {"mx_mexicocity",  "United States & Mexico", "Mexico City",                 "CST", -360, NULL, NULL, NULL},
    {"mx_cancun",      "United States & Mexico", "Cancun",                      "EST", -300, NULL, NULL, NULL},

    /* ---- Caribbean & South America ---- */
    {"pr_sanjuan",     "Caribbean & South America", "San Juan / Barbados",  "AST", -240, NULL, NULL, NULL},
    {"jm_kingston",    "Caribbean & South America", "Kingston, Jamaica",    "EST", -300, NULL, NULL, NULL},
    {"cu_havana",      "Caribbean & South America", "Havana",               "CST", -300, "CDT", "M3.2.0/0,M11.1.0/1", NULL},
    {"co_bogota",      "Caribbean & South America", "Bogota / Lima",        "<-05>", -300, NULL, NULL, NULL},
    {"cl_santiago",    "Caribbean & South America", "Santiago",             "<-04>", -240, "<-03>", "M9.1.6/24,M4.1.6/24", NULL},
    {"br_saopaulo",    "Caribbean & South America", "Sao Paulo / Rio",      "<-03>", -180, NULL, NULL, NULL},
    {"ar_buenosaires", "Caribbean & South America", "Buenos Aires",         "<-03>", -180, NULL, NULL, NULL},

    /* ---- Europe ---- */
    {"is_reykjavik",   "Europe", "Reykjavik",                 "GMT", 0, NULL, NULL, NULL},
    {"gb_london",      "Europe", "London",                    "GMT", 0, "BST", "M3.5.0/1,M10.5.0", NULL},
    {"ie_dublin",      "Europe", "Dublin",                    "GMT", 0, "IST", "M3.5.0/1,M10.5.0", NULL},
    {"pt_lisbon",      "Europe", "Lisbon",                    "WET", 0, "WEST", "M3.5.0/1,M10.5.0", NULL},
    {"fr_paris",       "Europe", "Paris / Brussels",          "CET", 60, "CEST", EU_RULE, NULL},
    {"de_berlin",      "Europe", "Berlin / Vienna / Zurich",  "CET", 60, "CEST", EU_RULE, NULL},
    {"nl_amsterdam",   "Europe", "Amsterdam / Copenhagen",    "CET", 60, "CEST", EU_RULE, NULL},
    {"it_rome",        "Europe", "Rome",                      "CET", 60, "CEST", EU_RULE, NULL},
    {"es_madrid",      "Europe", "Madrid",                    "CET", 60, "CEST", EU_RULE, NULL},
    {"pl_warsaw",      "Europe", "Warsaw / Prague",           "CET", 60, "CEST", EU_RULE, NULL},
    {"se_stockholm",   "Europe", "Stockholm / Oslo",          "CET", 60, "CEST", EU_RULE, NULL},
    {"gr_athens",      "Europe", "Athens",                    "EET", 120, "EEST", "M3.5.0/3,M10.5.0/4", NULL},
    {"fi_helsinki",    "Europe", "Helsinki",                  "EET", 120, "EEST", "M3.5.0/3,M10.5.0/4", NULL},
    {"ua_kyiv",        "Europe", "Kyiv",                      "EET", 120, "EEST", "M3.5.0/3,M10.5.0/4", NULL},
    {"tr_istanbul",    "Europe", "Istanbul",                  "<+03>", 180, NULL, NULL, NULL},
    {"ru_moscow",      "Europe", "Moscow",                    "MSK", 180, NULL, NULL, NULL},

    /* ---- Middle East & Africa ---- */
    {"ma_casablanca",  "Middle East & Africa", "Casablanca",        "<+01>", 60, NULL, NULL, NULL},
    {"ng_lagos",       "Middle East & Africa", "Lagos",             "WAT", 60, NULL, NULL, NULL},
    {"eg_cairo",       "Middle East & Africa", "Cairo",             "EET", 120, "EEST", "M4.5.5/0,M10.5.4/24", NULL},
    {"za_johannesburg","Middle East & Africa", "Johannesburg",      "SAST", 120, NULL, NULL, NULL},
    {"il_jerusalem",   "Middle East & Africa", "Jerusalem",         "IST", 120, "IDT", "M3.4.4/26,M10.5.0", NULL},
    {"ke_nairobi",     "Middle East & Africa", "Nairobi",           "EAT", 180, NULL, NULL, NULL},
    {"sa_riyadh",      "Middle East & Africa", "Riyadh / Doha",     "<+03>", 180, NULL, NULL, NULL},
    {"ir_tehran",      "Middle East & Africa", "Tehran",            "<+0330>", 210, NULL, NULL, NULL},
    {"ae_dubai",       "Middle East & Africa", "Dubai / Abu Dhabi", "<+04>", 240, NULL, NULL, NULL},

    /* ---- Asia ---- */
    {"pk_karachi",     "Asia", "Karachi",                   "PKT", 300, NULL, NULL, NULL},
    {"in_kolkata",     "Asia", "India (Delhi / Mumbai)",    "IST", 330, NULL, NULL, NULL},
    {"np_kathmandu",   "Asia", "Kathmandu",                 "<+0545>", 345, NULL, NULL, NULL},
    {"bd_dhaka",       "Asia", "Dhaka",                     "<+06>", 360, NULL, NULL, NULL},
    {"th_bangkok",     "Asia", "Bangkok / Hanoi",           "<+07>", 420, NULL, NULL, NULL},
    {"id_jakarta",     "Asia", "Jakarta",                   "WIB", 420, NULL, NULL, NULL},
    {"sg_singapore",   "Asia", "Singapore / Kuala Lumpur",  "<+08>", 480, NULL, NULL, NULL},
    {"cn_shanghai",    "Asia", "Beijing / Shanghai",        "CST", 480, NULL, NULL, NULL},
    {"hk_hongkong",    "Asia", "Hong Kong",                 "HKT", 480, NULL, NULL, NULL},
    {"tw_taipei",      "Asia", "Taipei",                    "CST", 480, NULL, NULL, NULL},
    {"ph_manila",      "Asia", "Manila",                    "PST", 480, NULL, NULL, NULL},
    {"kr_seoul",       "Asia", "Seoul",                     "KST", 540, NULL, NULL, NULL},
    {"jp_tokyo",       "Asia", "Tokyo",                     "JST", 540, NULL, NULL, NULL},

    /* ---- Oceania ---- */
    {"au_perth",       "Oceania", "Perth",                  "AWST", 480, NULL, NULL, NULL},
    {"au_darwin",      "Oceania", "Darwin",                 "ACST", 570, NULL, NULL, NULL},
    {"au_adelaide",    "Oceania", "Adelaide",               "ACST", 570, "ACDT", "M10.1.0,M4.1.0/3", NULL},
    {"au_brisbane",    "Oceania", "Brisbane",               "AEST", 600, NULL, NULL, NULL},
    {"au_sydney",      "Oceania", "Sydney / Melbourne",     "AEST", 600, "AEDT", "M10.1.0,M4.1.0/3", NULL},
    {"nz_auckland",    "Oceania", "Auckland",               "NZST", 720, "NZDT", "M9.5.0,M4.1.0/3", NULL},
    {"fj_fiji",        "Oceania", "Fiji",                   "<+12>", 720, NULL, NULL, NULL},

    /* ---- UTC ---- */
    {"utc",            "UTC", "UTC (Coordinated Universal Time)", "UTC", 0, NULL, NULL, NULL},
};
const size_t g_tz_count = sizeof(g_tz_table) / sizeof(g_tz_table[0]);

int tz_index(const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < g_tz_count; i++) {
        if (strcmp(g_tz_table[i].id, id) == 0) return (int)i;
    }
    return -1;
}

const tz_entry_t *tz_find(const char *id)
{
    int i = tz_index(id);
    return i >= 0 ? &g_tz_table[i] : NULL;
}

/* POSIX offsets are hours WEST of UTC: EST (UTC-5) -> "5" */
static void fmt_posix_offset(char *out, size_t n, int east_min)
{
    int west = -east_min;
    const char *sign = "";
    if (west < 0) { sign = "-"; west = -west; }
    int h = west / 60, m = west % 60;
    if (m) snprintf(out, n, "%s%d:%02d", sign, h, m);
    else snprintf(out, n, "%s%d", sign, h);
}

void tz_build_posix(const settings_t *s, char *out, size_t out_len)
{
    if (s->tz_custom[0]) {
        snprintf(out, out_len, "%s", s->tz_custom);
        return;
    }
    const tz_entry_t *z = tz_find(s->tz_id);
    if (!z) z = tz_find("ca_toronto");

    char std_off[12], dst_off[12];
    fmt_posix_offset(std_off, sizeof(std_off), z->std_offset_min);
    fmt_posix_offset(dst_off, sizeof(dst_off), z->std_offset_min + 60);

    bool has_dst = z->dst_abbr != NULL;
    bool perm = has_dst && z->dst_rule && strcmp(z->dst_rule, PERM) == 0;

    if (!has_dst) {
        snprintf(out, out_len, "%s%s", z->std_abbr, std_off);
        return;
    }
    switch ((dst_mode_t)s->dst_mode) {
    case DST_ALWAYS_STANDARD:
        snprintf(out, out_len, "%s%s", z->std_abbr, std_off);
        break;
    case DST_ALWAYS_DAYLIGHT:
        snprintf(out, out_len, "%s%s", z->dst_abbr, dst_off);
        break;
    case DST_AUTO:
    default:
        if (perm) snprintf(out, out_len, "%s%s", z->dst_abbr, dst_off);
        else snprintf(out, out_len, "%s%s%s,%s", z->std_abbr, std_off, z->dst_abbr, z->dst_rule);
        break;
    }
}
