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

/* Open just the Channels editor (the wizard's split-pane Step 3) as a
 * standalone modal — no CAN scan / ECU detect / Wi-Fi steps. Launched
 * from Device Settings → "Channels". LVGL task only. */
void first_run_wizard_open_channels(void);

/* Forward decl — full definition in widgets/widget_types.h. */
typedef struct widget_t widget_t;

/* Open the Channels editor targeting a specific dashboard widget (from a
 * long-press). Same split-pane editor, opened on the widget's current
 * channel, plus an "Apply to widget" action that binds the selected
 * channel to `w` and reloads the dashboard. LVGL task only. */
void first_run_wizard_open_channels_for_widget(widget_t *w);

#ifdef __cplusplus
}
#endif

#endif /* RDM7_FIRST_RUN_WIZARD_H */
