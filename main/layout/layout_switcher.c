/**
 * layout_switcher.c — see header.
 *
 * Two-source resolution:
 *   1. NVS pinned CSV (parsed into a stack-local name array).
 *   2. Fallback: layout_manager_list() with system layouts (leading '_')
 *      filtered out.
 *
 * Both branches end up as a `names[][LAYOUT_MAX_NAME]` array; the
 * navigation logic is shared. We re-resolve every call rather than
 * caching — pinned lists are short (≤8 names) and reads are NVS-cheap.
 */
#include "layout_switcher.h"

#include "esp_log.h"
#include "layout_manager.h"
#include "storage/config_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "lay_switch";

#define MAX_PINNED 8

/* Fill @p out with the current cycle. Returns the count; 0 = nothing.
 * The fallback branch lists every non-system layout in filesystem order;
 * the pinned branch parses the NVS CSV and drops any entry that no longer
 * exists on disk (so a deleted layout in the saved list quietly skips). */
static int _load_cycle(char out[MAX_PINNED][LAYOUT_MAX_NAME])
{
    char csv[LAYOUT_SWITCHER_CSV_MAX];
    esp_err_t err = config_store_load_layout_switcher(csv, sizeof(csv));
    int count = 0;

    if (err == ESP_OK && csv[0] != '\0') {
        /* Parse CSV — skip blank tokens, validate existence on LittleFS. */
        char *save = NULL;
        for (char *tok = strtok_r(csv, ",", &save); tok && count < MAX_PINNED;
             tok = strtok_r(NULL, ",", &save)) {
            /* Trim surrounding whitespace (forgiving for hand-edits). */
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok);
            while (end > tok && (end[-1] == ' ' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
            if (*tok == '\0') continue;
            /* Stat path: /lfs/layouts/<name>.json. */
            char path[64];
            snprintf(path, sizeof(path), "%s/%s.json", LFS_LAYOUT_DIR, tok);
            struct stat st;
            if (stat(path, &st) != 0) {
                ESP_LOGW(TAG, "pinned layout '%s' missing, skipping", tok);
                continue;
            }
            snprintf(out[count], LAYOUT_MAX_NAME, "%s", tok);
            count++;
        }
        if (count > 0) return count;
        /* Fall through to fs fallback if every pinned name was stale. */
    }

    /* Fallback: all non-system layouts in filesystem order. */
    char all[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int total = layout_manager_list(all, LAYOUT_MAX_COUNT);
    if (total < 0) return 0;
    for (int i = 0; i < total && count < MAX_PINNED; i++) {
        if (all[i][0] == '_') continue;
        snprintf(out[count], LAYOUT_MAX_NAME, "%s", all[i]);
        count++;
    }
    return count;
}

/* Find @p current in @p names; returns -1 if absent. */
static int _index_of(char names[MAX_PINNED][LAYOUT_MAX_NAME], int n,
                     const char *current)
{
    if (!current) return -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], current) == 0) return i;
    }
    return -1;
}

static bool _step(const char *current, char *out, size_t cap, int direction)
{
    if (!out || cap == 0) return false;
    char names[MAX_PINNED][LAYOUT_MAX_NAME];
    int n = _load_cycle(names);
    if (n < 2) return false;
    int idx = _index_of(names, n, current);
    if (idx < 0) {
        /* current isn't in the cycle: jump to the first entry on either
         * arrow so the user lands on a defined layout. */
        snprintf(out, cap, "%s", names[0]);
        return true;
    }
    int next = (idx + direction + n) % n;
    snprintf(out, cap, "%s", names[next]);
    return true;
}

bool layout_switcher_get_next(const char *current, char *out, size_t cap)
{
    return _step(current, out, cap, +1);
}

bool layout_switcher_get_prev(const char *current, char *out, size_t cap)
{
    return _step(current, out, cap, -1);
}

int layout_switcher_count(void)
{
    char names[MAX_PINNED][LAYOUT_MAX_NAME];
    return _load_cycle(names);
}
