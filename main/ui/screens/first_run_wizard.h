/* first_run_wizard.h — one-shot onboarding overlay for brand-new devices.
 *
 * Call show_first_run_wizard() once, after the dashboard screen is loaded,
 * when config_store_load_first_run_done() reports false. The wizard presents
 * a five-step path: auto-detect CAN bitrate, OBD2 discovery scan, ECU
 * auto-detect, channels review (with automatic OBD2 gap-fill when the scan
 * found the car answering), then Wi-Fi/finish. Dismissal marks
 * first_run_done = true via NVS so subsequent boots skip it. */

#ifndef RDM7_FIRST_RUN_WIZARD_H
#define RDM7_FIRST_RUN_WIZARD_H

#include <stdbool.h>

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

/* Apply an ECU preconfig set to the live signal registry, the active
 * layout's signals[], and the channel registry — the "pick your ECU and
 * get its channels" bulk path.
 *
 * Exported so /api/channels/import-preset can use the SAME resolution the
 * wizard uses (ECU alias table -> canonical id -> custom_<name> fallback,
 * with the collision guard). Studio previously had no bulk path at all:
 * a channel had to be created by hand before a preset signal could be
 * bound to it, one at a time (ADR-0030). Reimplementing the alias
 * resolution in JavaScript would have been a second copy destined to
 * drift, so the endpoint calls this instead.
 *
 *   labels  == NULL -> every non-OBD2 signal in the set.
 *              else -> only the `n_labels` preconfig labels listed, which
 *                      is what the Studio picker's tick list sends.
 *   replace == true -> clear existing vehicle channel bindings first and
 *                      record the ECU as the device's active one (the
 *                      wizard's "this IS my ECU" semantics).
 *              false -> purely additive; existing bindings and the stored
 *                      ECU identity are left alone.
 *
 * Returns the number of signals applied. LVGL task only (or with the LVGL
 * lock held). */
int first_run_wizard_apply_ecu(const char *ecu, const char *version,
                               const char *const *labels, int n_labels,
                               bool replace);

#ifdef __cplusplus
}
#endif

#endif /* RDM7_FIRST_RUN_WIZARD_H */
