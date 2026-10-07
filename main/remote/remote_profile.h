#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "keymap/key_definitions.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Remote profile abstraction
 * ==========================
 *
 * The key engine and the stored keymap are *model independent*: they only ever
 * see canonical key codes (MI_KEY_*). A remote profile describes one remote
 * model and is responsible for:
 *
 *   1. translating the model's raw HID usages into canonical codes
 *      (`canonicalize`), and
 *   2. declaring the ordered set of physical keys the model exposes
 *      (`keys`, each with a stable `slot` used to index the engine state).
 *
 * To support a new remote, add a `remote_profile_t` describing its key table
 * and normalization, then register it in `remote_profile.c`. Nothing else in
 * the firmware needs to change: BLE report decoding feeds raw codes into the
 * engine, and the WebUSB API exposes the active profile + key list so the web
 * UI can render the correct remote automatically.
 *
 * Slots must stay stable across firmware versions so stored keymaps keep
 * working; append new keys with fresh slots instead of renumbering.
 */

typedef struct {
    uint8_t     code;   /* canonical MI_KEY_* code */
    const char *name;   /* human-readable name shown in the web UI */
    uint8_t     slot;   /* engine state slot (0 .. MAX_KEY_BINDINGS-1) */
} remote_key_desc_t;

typedef struct remote_profile {
    const char *id;            /* stable machine id (used in JSON / NVS) */
    const char *name;          /* human-readable profile name */
    const char *ble_name_hint; /* BLE advertised-name substring for auto-detect */
    const remote_key_desc_t *keys;
    size_t      key_count;
    uint8_t   (*canonicalize)(uint8_t raw);
} remote_profile_t;

/** @brief Load the persisted profile (or the default) at boot. */
void remote_profile_init(void);

/** @brief Currently active profile (never NULL). */
const remote_profile_t *remote_profile_get(void);

/** @brief Look up a registered profile by id, or NULL when unknown. */
const remote_profile_t *remote_profile_find(const char *id);

/** @brief Switch profile by id, persist it and return the new profile. */
const remote_profile_t *remote_profile_set_by_id(const char *id);

/**
 * @brief Pick a profile from a BLE advertised name. Keeps the current profile
 * when nothing matches. Returns the (possibly unchanged) profile.
 */
const remote_profile_t *remote_profile_autodetect(const char *ble_name);

/** @brief Enumerate registered profiles. Returns the number written. */
size_t remote_profile_list(const remote_profile_t **out, size_t max);

/** @brief Normalize a raw code using the active profile (raw on failure). */
uint8_t remote_profile_canonical(uint8_t raw);

/** @brief Engine slot for a raw code, or -1 when the active profile lacks it. */
int remote_profile_slot(uint8_t raw);

/** @brief Key descriptor at a given engine slot, or NULL when out of range. */
const remote_key_desc_t *remote_profile_key_at(size_t slot);

/** @brief Number of physical keys exposed by the active profile. */
size_t remote_profile_key_count(void);

#ifdef __cplusplus
}
#endif
