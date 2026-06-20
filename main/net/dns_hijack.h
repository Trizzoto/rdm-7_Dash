/*
 * dns_hijack.h — Captive-portal DNS responder.
 *
 * Answers EVERY DNS A-query with the SoftAP address (192.168.4.1) so a phone
 * that joins the hotspot resolves any hostname to the device and the OS
 * captive-portal sheet opens straight into the editor. UDP :53, runs only while
 * the SoftAP is up. Pair with the SoftAP DHCP offering 192.168.4.1 as the DNS
 * server (otherwise clients never query us). Idempotent start/stop.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void dns_hijack_start(void);   /* start the UDP-53 responder task (no-op if running) */
void dns_hijack_stop(void);    /* signal the task to exit (it self-deletes within ~1 s) */

#ifdef __cplusplus
}
#endif
