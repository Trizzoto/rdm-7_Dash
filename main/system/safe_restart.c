/* See safe_restart.h for why this exists. */
#include "system/safe_restart.h"

#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_private/system_internal.h"
#include "sdkconfig.h"

static const char *TAG = "restart";

void rdm_safe_restart(void)
{
	/* Log before stalling: once the other core is held, nothing scheduled on
	 * it runs again, and the UART log hook may be waiting on it. */
	ESP_LOGI(TAG, "restarting");

#if !CONFIG_FREERTOS_UNICORE
	/* Hold the other core so it cannot fetch from a cache that is about to be
	 * switched off underneath it. A hardware stall through the RTC control
	 * registers, so it needs no cooperation from the core being stopped —
	 * which matters, because that core is running LVGL and is in no position
	 * to be asked politely.
	 *
	 * This must happen as late as possible. The first version of this stalled
	 * the core and then called esp_restart(), which runs the registered
	 * shutdown handlers first — WiFi and BT teardown, hundreds of
	 * milliseconds — with one core held. The interrupt watchdog noticed and
	 * killed the dash every single time: 8 reboots, 8 INT_WDT resets, worse
	 * than the fault being fixed. Hence esp_restart_noos() below rather than
	 * esp_restart(): from here to the reset is a few microseconds, far inside
	 * the 800 ms the interrupt watchdog allows. */
	esp_cpu_stall(esp_cpu_get_core_id() == 0 ? 1 : 0);
#endif

	/* Deliberately NOT esp_restart(): that would run the shutdown handlers
	 * (see above). Skipping them costs nothing here, because
	 * esp_restart_noos() resets the digital peripherals on its way out, so the
	 * radios end up in the same state a hard reset leaves them in — and the
	 * dash already boots from hard resets and watchdog resets routinely.
	 *
	 * Callers wanting the restart recorded as deliberate still call
	 * crash_log_mark_clean_shutdown() beforehand; that writes NVS, which is
	 * complete before we get here. */
	esp_restart_noos();
}
