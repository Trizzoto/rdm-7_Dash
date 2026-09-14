#ifndef OTA_UPDATE_DIALOG_H
#define OTA_UPDATE_DIALOG_H

#include "lvgl.h"
#include "ota_handler.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function declarations
void show_ota_update_dialog(const char* current_version, const char* new_version, float file_size_mb, const char* release_notes);
/* Do what pressing Install does. The dialog must already be up — the boot
 * resume path shows it first, then calls this, so the install it starts has
 * the same progress bar and the same error handling as a manual one. */
void ota_update_dialog_begin_install(void);
void show_ota_checking_dialog(void);
/* The on-glass "Check for updates": says so at once when there is no internet
 * to check with, otherwise shows the checking dialog, asks the server off the
 * LVGL task, and follows up with update available / up to date / failed —
 * unless the person cancelled while it was asking. */
void ota_update_dialog_check_now(void);
void show_ota_up_to_date_dialog(const char* current_version);
void show_ota_check_failed_dialog(void);
void close_ota_update_dialog(void);
void update_ota_progress_dialog(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_UPDATE_DIALOG_H */ 