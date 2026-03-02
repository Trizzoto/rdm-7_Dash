#ifndef OTA_UPDATE_DIALOG_H
#define OTA_UPDATE_DIALOG_H

#include "lvgl.h"
#include "ota_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function declarations
void show_ota_update_dialog(const char* current_version, const char* new_version, const char* update_type, float file_size_mb, const char* release_notes);
void close_ota_update_dialog(void);
void update_ota_progress_dialog(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_DIALOG_H */ 