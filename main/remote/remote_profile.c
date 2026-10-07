#include "remote_profile.h"
#include "app_log.h"
#include "storage/config_store.h"

#include <string.h>

#define REMOTE_PROFILE_NS  "remote_cfg"
#define REMOTE_PROFILE_KEY "profile"

// ===========================================================================
// Xiaomi RC003 (Bluetooth Voice Remote 2 Pro)
//
// HOGP reports carry either standard 8-byte boot-keyboard reports or the
// RC003 vendor array of little-endian 16-bit keyboard usages. The raw usages
// for Home / Menu / TV are normalized to the canonical MI_KEY_* codes.
// ===========================================================================
static uint8_t rc003_canonicalize(uint8_t raw)
{
    switch (raw) {
        case MI_KEY_POWER_ALT: return MI_KEY_POWER;
        case MI_KEY_VOICE_ALT: return MI_KEY_VOICE;
        case MI_KEY_HOME_ALT:  return MI_KEY_HOME;
        case MI_KEY_MENU_ALT:  return MI_KEY_MENU;
        case MI_KEY_TV_ALT:    return MI_KEY_TV;
        default: return raw;
    }
}

static const remote_key_desc_t rc003_keys[] = {
    { MI_KEY_POWER,    "电源键", 0  },
    { MI_KEY_VOICE,    "语音键", 1  },
    { MI_KEY_UP,       "方向上", 2  },
    { MI_KEY_DOWN,     "方向下", 3  },
    { MI_KEY_LEFT,     "方向左", 4  },
    { MI_KEY_RIGHT,    "方向右", 5  },
    { MI_KEY_OK,       "确定键", 6  },
    { MI_KEY_BACK,     "返回键", 7  },
    { MI_KEY_HOME,     "主页键", 8  },
    { MI_KEY_MENU,     "菜单键", 9  },
    { MI_KEY_VOL_UP,   "音量+",  10 },
    { MI_KEY_VOL_DOWN, "音量-",  11 },
    { MI_KEY_TV,       "电视键", 12 },
};

const remote_profile_t g_remote_profile_rc003 = {
    .id = "rc003",
    .name = "MIRC 2 Pro",
    .ble_name_hint = "MI RC",
    .keys = rc003_keys,
    .key_count = sizeof(rc003_keys) / sizeof(rc003_keys[0]),
    .canonicalize = rc003_canonicalize,
};

// ===========================================================================
// Registry
// ===========================================================================
static const remote_profile_t *s_profiles[] = {
    &g_remote_profile_rc003,
};
#define REMOTE_PROFILE_COUNT (sizeof(s_profiles) / sizeof(s_profiles[0]))

static const remote_profile_t *s_active = &g_remote_profile_rc003;

void remote_profile_init(void)
{
    char id[24] = {0};
    if (config_store_get_str(REMOTE_PROFILE_NS, REMOTE_PROFILE_KEY, id, sizeof(id)) > 0) {
        const remote_profile_t *p = remote_profile_set_by_id(id);
        if (p) {
            app_log("REMOTE", "Loaded remote profile '%s'", p->id);
        } else {
            app_log("REMOTE", "Unknown saved profile '%s', using default", id);
        }
    } else {
        app_log("REMOTE", "Using default remote profile '%s'", s_active->id);
    }
}

const remote_profile_t *remote_profile_get(void)
{
    return s_active;
}

const remote_profile_t *remote_profile_find(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (size_t i = 0; i < REMOTE_PROFILE_COUNT; i++) {
        if (strcmp(s_profiles[i]->id, id) == 0) return s_profiles[i];
    }
    return NULL;
}

const remote_profile_t *remote_profile_set_by_id(const char *id)
{
    const remote_profile_t *p = remote_profile_find(id);
    if (!p) return NULL;
    s_active = p;
    config_store_set_str(REMOTE_PROFILE_NS, REMOTE_PROFILE_KEY, p->id);
    return p;
}

const remote_profile_t *remote_profile_autodetect(const char *ble_name)
{
    if (!ble_name) return s_active;
    for (size_t i = 0; i < REMOTE_PROFILE_COUNT; i++) {
        const char *hint = s_profiles[i]->ble_name_hint;
        if (hint && hint[0] && strstr(ble_name, hint)) {
            if (s_profiles[i] != s_active) {
                app_log("REMOTE", "Auto-detected profile '%s' from name '%s'",
                        s_profiles[i]->id, ble_name);
                remote_profile_set_by_id(s_profiles[i]->id);
            }
            return s_active;
        }
    }
    return s_active;
}

size_t remote_profile_list(const remote_profile_t **out, size_t max)
{
    size_t n = 0;
    if (!out) return 0;
    for (size_t i = 0; i < REMOTE_PROFILE_COUNT && n < max; i++) {
        out[n++] = s_profiles[i];
    }
    return n;
}

uint8_t remote_profile_canonical(uint8_t raw)
{
    if (s_active && s_active->canonicalize) {
        return s_active->canonicalize(raw);
    }
    return raw;
}

int remote_profile_slot(uint8_t raw)
{
    if (!s_active) return -1;
    uint8_t can = remote_profile_canonical(raw);
    for (size_t i = 0; i < s_active->key_count; i++) {
        if (s_active->keys[i].code == can) {
            return (int)s_active->keys[i].slot;
        }
    }
    return -1;
}

const remote_key_desc_t *remote_profile_key_at(size_t slot)
{
    if (!s_active) return NULL;
    for (size_t i = 0; i < s_active->key_count; i++) {
        if (s_active->keys[i].slot == slot) {
            return &s_active->keys[i];
        }
    }
    return NULL;
}

size_t remote_profile_key_count(void)
{
    return s_active ? s_active->key_count : 0;
}
