#include "lockcore.h"
#include <stdio.h>
#include <string.h>
#include "sha256.h"
#include "platform.h"

lock_cfg_t g_lock;
lock_log_t g_lock_log[LOCK_LOG_LEN];
int g_lock_log_count;

static int s_fails;
static int s_lockouts;
static uint32_t s_lockout_until;
static uint32_t s_relock_at;   /* uptime second at which a momentary unlock ends */

static void hash_code(const uint8_t salt[8], const char *code, uint8_t out[32])
{
    uint8_t buf[8 + 16];
    size_t n = strlen(code);
    if (n > 16) n = 16;
    memcpy(buf, salt, 8);
    memcpy(buf + 8, code, n);
    uint8_t h[32];
    sha256(buf, 8 + n, h);
    /* a few thousand rounds so stolen flash contents don't give the codes away instantly */
    for (int i = 0; i < 2000; i++) sha256(h, 32, h);
    memcpy(out, h, 32);
}

static bool matches(const uint8_t salt[8], const uint8_t hash[32], const char *code)
{
    uint8_t h[32];
    hash_code(salt, code, h);
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) d |= h[i] ^ hash[i];
    return d == 0;
}

void lock_set_defaults(lock_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = 1;
    snprintf(c->relay1_name, sizeof(c->relay1_name), "Door");
    snprintf(c->relay2_name, sizeof(c->relay2_name), "Relay 2");
    c->relay1_mode = RELAY1_LATCHED;
    c->relay1_pulse_s = 5;
    c->relay2_needs_pin = true;
    c->web_control = false;
    c->max_attempts = 5;
    c->lockout_s = 60;
    /* default user code 1234 */
    c->users[0].used = true;
    snprintf(c->users[0].label, sizeof(c->users[0].label), "User");
    plat_random(c->users[0].salt, 8);
    hash_code(c->users[0].salt, "1234", c->users[0].hash);
}

void lock_restore(void)
{
    if (g_lock.relay1_mode == RELAY1_MOMENTARY) g_lock.relay1_unlocked = false;
    plat_relay_set(1, g_lock.relay1_unlocked);
    plat_relay_set(2, g_lock.relay2_on);
    if (g_lock.relay1_unlocked || g_lock.relay2_on) lock_log(LEV_BOOT_RESTORE, "power on");
}

bool lock_locked_out(uint32_t *left)
{
    uint32_t now = plat_uptime_s();
    if (s_lockout_until > now) {
        if (left) *left = s_lockout_until - now;
        return true;
    }
    if (left) *left = 0;
    return false;
}

int lock_check(const char *code, bool allow_admin)
{
    if (lock_locked_out(NULL)) return -1;
    int r = -1;
    if (allow_admin && g_lock.admin_set && matches(g_lock.admin_salt, g_lock.admin_hash, code)) r = LOCK_ADMIN;
    for (int i = 0; r < 0 && i < LOCK_MAX_USERS; i++)
        if (g_lock.users[i].used && matches(g_lock.users[i].salt, g_lock.users[i].hash, code)) r = i;
    if (r >= 0) {
        s_fails = 0;
        s_lockouts = 0;
        return r;
    }
    lock_log(LEV_BAD_CODE, "keypad");
    if (++s_fails >= (g_lock.max_attempts ? g_lock.max_attempts : 5)) {
        uint32_t len = (g_lock.lockout_s ? g_lock.lockout_s : 60) << (s_lockouts < 5 ? s_lockouts : 5);
        s_lockout_until = plat_uptime_s() + len;
        s_lockouts++;
        s_fails = 0;
        lock_log(LEV_LOCKOUT, "keypad");
    }
    return -1;
}

void lock_unlock(const char *who)
{
    g_lock.relay1_unlocked = true;
    plat_relay_set(1, true);
    if (g_lock.relay1_mode == RELAY1_MOMENTARY) {
        s_relock_at = plat_uptime_s() + (g_lock.relay1_pulse_s ? g_lock.relay1_pulse_s : 5);
    } else {
        s_relock_at = 0;
        plat_lock_save();
    }
    lock_log(LEV_UNLOCK, who);
}

void lock_lock(const char *who, bool automatic)
{
    g_lock.relay1_unlocked = false;
    s_relock_at = 0;
    plat_relay_set(1, false);
    plat_lock_save();
    lock_log(automatic ? LEV_AUTO_LOCK : LEV_LOCK, who);
}

void lock_relay2_set(bool on, const char *who)
{
    g_lock.relay2_on = on;
    plat_relay_set(2, on);
    plat_lock_save();
    lock_log(on ? LEV_R2_ON : LEV_R2_OFF, who);
}

int lock_relock_seconds_left(void)
{
    if (!s_relock_at) return 0;
    uint32_t now = plat_uptime_s();
    return s_relock_at > now ? (int)(s_relock_at - now) : 0;
}

void lock_tick(void)
{
    if (s_relock_at && plat_uptime_s() >= s_relock_at) lock_lock("timer", true);
}

bool lock_code_valid(const char *code, int min_len, int max_len)
{
    int n = (int)strlen(code);
    if (n < min_len || n > max_len) return false;
    for (int i = 0; i < n; i++) if (code[i] < '0' || code[i] > '9') return false;
    return true;
}

bool lock_code_in_use(const char *code, int except_user)
{
    if (g_lock.admin_set && except_user != LOCK_ADMIN && matches(g_lock.admin_salt, g_lock.admin_hash, code)) return true;
    for (int i = 0; i < LOCK_MAX_USERS; i++)
        if (i != except_user && g_lock.users[i].used && matches(g_lock.users[i].salt, g_lock.users[i].hash, code)) return true;
    return false;
}

void lock_set_admin(const char *code)
{
    plat_random(g_lock.admin_salt, 8);
    hash_code(g_lock.admin_salt, code, g_lock.admin_hash);
    g_lock.admin_set = true;
    plat_lock_save();
    lock_log(LEV_CODES_CHANGED, "admin code");
}

void lock_set_user(int idx, const char *label, const char *code)
{
    if (idx < 0 || idx >= LOCK_MAX_USERS) return;
    lock_code_t *u = &g_lock.users[idx];
    u->used = true;
    if (label && label[0]) snprintf(u->label, sizeof(u->label), "%s", label);
    else if (!u->label[0]) snprintf(u->label, sizeof(u->label), "User %d", idx + 1);
    if (code) {
        plat_random(u->salt, 8);
        hash_code(u->salt, code, u->hash);
    }
    plat_lock_save();
    lock_log(LEV_CODES_CHANGED, u->label);
}

void lock_delete_user(int idx)
{
    if (idx < 0 || idx >= LOCK_MAX_USERS) return;
    char who[24];
    snprintf(who, sizeof(who), "%s", g_lock.users[idx].label);
    memset(&g_lock.users[idx], 0, sizeof(g_lock.users[idx]));
    plat_lock_save();
    lock_log(LEV_CODES_CHANGED, who);
}

int lock_user_count(void)
{
    int n = 0;
    for (int i = 0; i < LOCK_MAX_USERS; i++) n += g_lock.users[i].used;
    return n;
}

void lock_log(lock_ev_t type, const char *who)
{
    if (g_lock_log_count >= LOCK_LOG_LEN) {
        memmove(g_lock_log, g_lock_log + 1, sizeof(lock_log_t) * (LOCK_LOG_LEN - 1));
        g_lock_log_count = LOCK_LOG_LEN - 1;
    }
    lock_log_t *e = &g_lock_log[g_lock_log_count++];
    e->t = plat_now();
    e->type = (uint8_t)type;
    snprintf(e->who, sizeof(e->who), "%s", who ? who : "");
    plat_lock_log_save();
}

const char *lock_ev_text(uint8_t type)
{
    switch (type) {
    case LEV_UNLOCK: return "Unlocked";
    case LEV_LOCK: return "Locked";
    case LEV_AUTO_LOCK: return "Locked automatically";
    case LEV_R2_ON: return "Relay 2 on";
    case LEV_R2_OFF: return "Relay 2 off";
    case LEV_BAD_CODE: return "Wrong code";
    case LEV_LOCKOUT: return "Keypad locked out";
    case LEV_ADMIN: return "Admin signed in";
    case LEV_CODES_CHANGED: return "Codes changed";
    case LEV_BOOT_RESTORE: return "State restored after power-up";
    default: return "Event";
    }
}
