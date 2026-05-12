/**
 * user_signals.h - persistent library of user-defined CAN signals.
 *
 * Lives at /lfs/dbc/user.json. Holds every signal the user has defined
 * via the new-signal wizard's manual-entry path - reused across layouts
 * so the user doesn't have to re-type a custom signal each time they
 * create a new dashboard.
 *
 * NOT a DBC file format - it's a small JSON list. The file name nods at
 * DBC's role as the canonical signal library; using JSON keeps the
 * firmware free of a DBC parser, and the on-disk format mirrors the
 * layout's signals[] entries field-for-field so a future migration is
 * trivial. The web editor doesn't need to read this file - it manages
 * its own DBC import flow that feeds signals into the layout directly.
 */
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USER_SIGNALS_MAX 64

typedef struct {
    char     name[32];
    uint32_t can_id;
    uint8_t  start_bit;
    uint8_t  length;
    float    scale;
    float    offset;
    bool     is_signed;
    uint8_t  endian;       /* 0=Motorola, 1=Intel */
    char     unit[8];
} user_signal_t;

/** Load /lfs/dbc/user.json into the in-memory cache. Call once at boot.
 *  Idempotent. */
esp_err_t            user_signals_init(void);
uint16_t             user_signals_count(void);
const user_signal_t *user_signals_get(uint16_t index);
const user_signal_t *user_signals_find(const char *name);

/** Append a signal (or replace an existing one with the same name) and
 *  persist the library to disk. */
esp_err_t            user_signals_append(const user_signal_t *sig);

#ifdef __cplusplus
}
#endif
