#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define LOCK_MAX_USERS 8
#define LOCK_LOG_LEN   64

typedef enum { RELAY1_LATCHED = 0, RELAY1_MOMENTARY = 1 } relay1_mode_t;

typedef struct {
    bool used;
    char label[24];
    uint8_t salt[8];
    uint8_t hash[32];
} lock_code_t;

typedef struct {
    uint16_t version;
    bool admin_set;
    uint8_t admin_salt[8];
    uint8_t admin_hash[32];
    lock_code_t users[LOCK_MAX_USERS];
    char relay1_name[32];
    char relay2_name[32];
    uint8_t relay1_mode;        /* relay1_mode_t */
    uint16_t relay1_pulse_s;    /* momentary unlock time */
    bool relay2_needs_pin;
    bool web_control;           /* allow lock/unlock from the web page */
    uint8_t max_attempts;       /* wrong codes before lockout */
    uint16_t lockout_s;         /* first lockout length, doubles each time */
    /* persisted relay states (restored after power loss) */
    bool relay1_unlocked;
    bool relay2_on;
} lock_cfg_t;

typedef enum {
    LEV_UNLOCK = 1, LEV_LOCK, LEV_AUTO_LOCK, LEV_R2_ON, LEV_R2_OFF, LEV_BAD_CODE, LEV_LOCKOUT,
    LEV_ADMIN, LEV_CODES_CHANGED, LEV_BOOT_RESTORE,
} lock_ev_t;

typedef struct {
    time_t t;
    uint8_t type;
    char who[23];
} lock_log_t;

extern lock_cfg_t g_lock;
extern lock_log_t g_lock_log[LOCK_LOG_LEN];
extern int g_lock_log_count;   /* newest last, ring of LOCK_LOG_LEN */

void lock_set_defaults(lock_cfg_t *c);
/* Called at boot after loading: drives the relays to the saved state. */
void lock_restore(void);

/* Returns user index (0..), LOCK_ADMIN for the admin code, or -1. Applies lockout. */
#define LOCK_ADMIN 100
int lock_check(const char *code, bool allow_admin);
bool lock_locked_out(uint32_t *seconds_left);

void lock_unlock(const char *who);
void lock_lock(const char *who, bool automatic);
void lock_relay2_set(bool on, const char *who);
int lock_relock_seconds_left(void);   /* momentary mode countdown, 0 if none */
void lock_tick(void);                 /* call about once a second */

bool lock_code_valid(const char *code, int min_len, int max_len);
void lock_set_admin(const char *code);
void lock_set_user(int idx, const char *label, const char *code);
void lock_delete_user(int idx);
bool lock_code_in_use(const char *code, int except_user);  /* also checks admin */
int lock_user_count(void);

void lock_log(lock_ev_t type, const char *who);
const char *lock_ev_text(uint8_t type);

/* platform hooks */
void plat_relay_set(int relay, bool on);
void plat_lock_save(void);       /* persist g_lock */
void plat_lock_log_save(void);   /* persist g_lock_log */
void plat_random(void *buf, int len);
uint32_t plat_uptime_s(void);
