/* mdns_service.h — advertise the dash as rdm7.local with service discovery.
 *
 * Works in both STA (joined WiFi) and SoftAP modes. When either interface
 * comes up, call mdns_service_refresh() and the daemon re-binds to the new
 * netif so browsers and the desktop app can find the dash at rdm7.local.
 *
 * Services advertised:
 *   _http._tcp      80        — web UI
 *   _rdm7._tcp      80        — app-specific discovery record (used by
 *                                desktop app's discover_devices command).
 *                                TXT fields: serial, version, schema.
 */

#ifndef RDM7_MDNS_SERVICE_H
#define RDM7_MDNS_SERVICE_H

#include "esp_err.h"

/* Initialise the mDNS daemon once (safe to call multiple times).
 * After first success, the "rdm7.local" hostname is reserved on the
 * local network — accessible from both STA and SoftAP clients. */
esp_err_t rdm7_mdns_init(void);

/* Re-advertise services. Call after WiFi STA gets an IP, after SoftAP
 * starts, and whenever the interface list changes. Harmless to call
 * repeatedly; the underlying mdns component handles dedup. */
esp_err_t rdm7_mdns_refresh(void);

/* Stop the daemon and release resources (factory reset, shutdown). */
void rdm7_mdns_stop(void);

#endif /* RDM7_MDNS_SERVICE_H */
