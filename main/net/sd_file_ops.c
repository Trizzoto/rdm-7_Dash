/**
 * sd_file_ops.c — SD-card directory listing + file copy.
 *
 * Extracted from web_server_assets.c / serial_commands.c, which each hand-
 * rolled their own copy of these two loops. serial_commands.c's version of
 * the directory walk was already parameterized by dir/ext (a cleaner
 * factoring than web_server_assets.c's three inlined copies) — this file
 * generalizes that version rather than reinventing it. sd_copy_file() below
 * is based on web_server_assets.c's version, which pre-checks the source
 * file size and rejects zero-byte sources as ESP_FAIL; the serial side's
 * old copy silently "succeeded" on an empty source (loop just never ran),
 * so serial's behavior changes slightly here — a zero-byte SD<->LittleFS
 * copy request now fails instead of silently producing an empty file.
 */
#include "sd_file_ops.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void sd_list_dir(cJSON *arr, const char *dir, const char *ext, size_t ext_len)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t flen = strlen(de->d_name);
        if (flen <= ext_len || strcmp(de->d_name + flen - ext_len, ext) != 0)
            continue;

        char path[96];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        char name[64];
        size_t copy = flen - ext_len;
        if (copy >= sizeof(name)) copy = sizeof(name) - 1;
        memcpy(name, de->d_name, copy);
        name[copy] = '\0';

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", name);
        cJSON_AddNumberToObject(obj, "size", st.st_size);
        cJSON_AddItemToArray(arr, obj);
    }
    closedir(d);
}

esp_err_t sd_copy_file(const char *src, const char *dst)
{
    FILE *fin = fopen(src, "rb");
    if (!fin) return ESP_ERR_NOT_FOUND;

    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(fin);
        return ESP_FAIL;
    }

    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        fclose(fin);
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        fclose(fin);
        fclose(fout);
        return ESP_FAIL;
    }

    size_t total_written = 0;
    while (total_written < (size_t)file_size) {
        size_t to_read = 4096;
        if (to_read > (size_t)file_size - total_written)
            to_read = (size_t)file_size - total_written;
        size_t nr = fread(buf, 1, to_read, fin);
        if (nr == 0) break;
        size_t nw = fwrite(buf, 1, nr, fout);
        if (nw != nr) {
            free(buf);
            fclose(fin);
            fclose(fout);
            remove(dst);
            return ESP_FAIL;
        }
        total_written += nw;
    }

    free(buf);
    fclose(fin);
    fclose(fout);
    return (total_written == (size_t)file_size) ? ESP_OK : ESP_FAIL;
}
