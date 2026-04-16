/* first_run_wizard.h — one-shot onboarding overlay for brand-new devices.
 *
 * Call show_first_run_wizard() once, after the dashboard screen is loaded,
 * when config_store_load_first_run_done() reports false. The wizard presents
 * a three-step path: auto-detect CAN bitrate, connect to Wi-Fi, then finish.
 * Dismissal marks first_run_done = true via NVS so subsequent boots skip it. */

#ifndef RDM7_FIRST_RUN_WIZARD_H
#define RDM7_FIRST_RUN_WIZARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create and show the wizard overlay. Safe to call from the LVGL task only. */
void show_first_run_wizard(void);

#ifdef __cplusplus
}
#endif

#endif /* RDM7_FIRST_RUN_WIZARD_H */
