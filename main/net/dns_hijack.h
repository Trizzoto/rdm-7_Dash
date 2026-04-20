#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal DNS responder for captive-portal detection.
 *
 * Runs a UDP server on port 53 that answers every query with the ESP's
 * AP IP (default 192.168.4.1). Combined with the HTTP captive-portal
 * handlers in web_server.c, this triggers the "Sign in to network"
 * sheet on iOS/Android so the user can reach the dash editor even when
 * the phone's OS has decided the hotspot has "no internet" and normally
 * refuses to route browser traffic to it.
 *
 * Start once after the AP netif is up; safe to call repeatedly. Stop
 * is idempotent; it tears down the socket and task. */
esp_err_t dns_hijack_start(void);
void      dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif
