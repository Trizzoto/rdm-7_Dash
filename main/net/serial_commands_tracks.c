/**
 * serial_commands_tracks.c — track-map asset serial JSON-RPC handlers.
 *
 * Methods: track.list, track.delete.
 *
 * The USB counterparts of the HTTP endpoints in web_server_assets.c. They
 * exist because a dash on a bench is on a USB cable, not on WiFi: without
 * these, pushing a circuit meant getting the dash onto a network first, which
 * is the one thing you cannot do in a pit garage.
 *
 * Uploading a track lives in serial_commands_upload.c with the other chunked
 * transfers — it shares the one upload session.
 */
#include "serial_commands_internal.h"

#include "cJSON.h"
#include "widgets/track_map_geo.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ── track.list ──────────────────────────────────────────────────────────── */

/* Mirrors GET /api/track/list: `name` is the filename a widget references,
 * `track` is the circuit's own name from inside the file. They differ often
 * enough (rename the file, keep the circuit) that showing only one of them
 * makes the widget's Track field impossible to fill in confidently. */
void _handle_track_list(int id, cJSON *params)
{
    (void)params;
    _ensure_dir(TRACK_MAP_LFS_DIR);

    const size_t ext_len = strlen(TRACK_MAP_EXT);

    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(TRACK_MAP_LFS_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t flen = strlen(de->d_name);
            if (flen <= ext_len ||
                strcmp(de->d_name + flen - ext_len, TRACK_MAP_EXT) != 0)
                continue;

            char name_buf[TRACK_MAP_NAME_LEN];
            size_t name_len = flen - ext_len;
            if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
            memcpy(name_buf, de->d_name, name_len);
            name_buf[name_len] = '\0';

            char path[96];
            snprintf(path, sizeof(path), "%s/%s", TRACK_MAP_LFS_DIR, de->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;

            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", name_buf);
            cJSON_AddNumberToObject(obj, "size", st.st_size);

            /* Header only. A track is small, but reading every point of every
             * track to print a list would still be work for nothing — the name
             * and the count both live in the first 52 bytes. */
            FILE *f = fopen(path, "rb");
            if (f) {
                uint8_t hdr[TRACK_MAP_HEADER];
                size_t got = fread(hdr, 1, sizeof(hdr), f);
                fclose(f);
                if (got == sizeof(hdr) &&
                    memcmp(hdr, TRACK_MAP_MAGIC, TRACK_MAP_MAGIC_LEN) == 0) {
                    char tname[TRACK_MAP_NAME_LEN];
                    memcpy(tname, hdr + 8, TRACK_MAP_NAME_LEN - 1);
                    tname[TRACK_MAP_NAME_LEN - 1] = '\0';
                    cJSON_AddStringToObject(obj, "track", tname);
                    cJSON_AddNumberToObject(obj, "version", hdr[6]);
                    cJSON_AddNumberToObject(obj, "points",
                                            (uint16_t)hdr[48] | ((uint16_t)hdr[49] << 8));
                } else {
                    cJSON_AddStringToObject(obj, "track", "");
                    cJSON_AddNumberToObject(obj, "points", 0);
                }
            }
            cJSON_AddItemToArray(arr, obj);
        }
        closedir(d);
    }
    _send_response(id, arr, NULL);
}

/* ── track.delete ────────────────────────────────────────────────────────── */

void _handle_track_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name");
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s" TRACK_MAP_EXT, TRACK_MAP_LFS_DIR,
             name_item->valuestring);
    if (remove(path) != 0) {
        _send_error(id, "Track not found");
        return;
    }
    _send_ok(id);
}
