# RDM-7 Pre-Launch Hardening Sweep — Triage

_Updated 2026-07-05. 42 candidate defects from a 12-finder parallel audit, each adversarially verified by an independent high-effort agent. **36 confirmed, 5 refuted, 1 pre-fixed.**_

**30 of 36 confirmed defects fixed** across 4 commits (all build-clean, boot-verified panic_count=0, native suite 34/34, key fixes live-verified on hardware). **6 remain** — 2 product/security-policy decisions and 4 deferred with rationale.

## Fixed (30)

| # | Sev | Commit | Location | Defect |
|---|---|---|---|---|
| 0 | high | `7518519` | `main/net/web_server_capture.c:294` | /api/screenshot/hash int-overflow OOB framebuffer read |
| 7 | high | `7518519` | `main/net/ota_handler.c:272` | dead OTA reentrancy guard -> concurrent-check double-free/UAF |
| 8 | high | `7518519` | `main/net/ota_handler.c:231` | check during download re-opened install guard -> concurrent OTA |
| 17 | high | `7518519` | `main/widgets/signal.c:367` | CAN 0x000 corrupting every OBD2/internal signal |
| 25 | high | `7518519` | `main/can/obd2.c:395` | obd2_stop froze an in-progress discovery scan |
| 27 | high | `83baaf3` | `main/data/channel_manager.c:1457` | custom_* channels dropped on reboot (Phase-2 skip) |
| 29 | high | `83baaf3` | `main/data/channel_manager.c:1516` | unguarded serialize on lock timeout -> read freed memory |
| 30 | high | `83baaf3` | `main/data/channel_manager.c:1396` | power-loss between the two renames -> reseed truncates new .tmp |
| 32 | high | `7518519` | `main/net/web_server_logger.c:325` | /api/log/delete could remove the logger's open file |
| 34 | high | `7518519` | `main/storage/signal_replay.c:204` | signal_replay walked whole CSV on LVGL task -> freeze/TWDT |
| 36 | high | `7518519` | `main/net/web_server_logger.c:378` | /api/log/upload 2nd handle on active LFS log |
| 1 | medium | `d19b9f6` | `main/net/web_server.c:192` | recv_body_raw infinite timeout retry wedged the httpd task |
| 2 | medium | `d19b9f6` | `main/net/serial_commands_upload.c:157` | serial upload wrote a truncated buffer as the live asset |
| 3 | medium | `d19b9f6` | `main/net/serial_commands_upload.c:188` | serial upload.finish non-atomic in-place write |
| 4 | medium | `d19b9f6` | `main/net/web_server_assets.c:531` | web font upload non-atomic in-place write |
| 6 | medium | `d19b9f6` | `main/net/serial_commands_upload.c:62` | serial upload bypassed the protected-asset guard |
| 10 | medium | `7518519` | `main/net/ota_handler.c:641` | esp_https_ota_get_image_len_read after abort freed handle (UAF) |
| 15 | medium | `7518519` | `main/can/can_manager.c:335` | extended 29-bit IDs truncated -> those channels never decode |
| 16 | medium | `7518519` | `main/can/obd2.c:1597` | OBD2 ISO-TP memcpy'd unclamped DLC (up to 14) from 8-byte frame |
| 21 | medium | `fceb678` | `main/can/obd2.c:617` | one-shot timeouts didn't tick when polling idle -> bricked req |
| 22 | medium | `fceb678` | `main/can/obd2.c:1696` | Mode 09 NRC wrongly cancelled VIN when ECU-name also pending |
| 23 | medium | `fceb678` | `main/can/obd2.c:1544` | strict 1-byte Mode 04 clear-ack dropped -> success read as fail |
| 28 | medium | `83baaf3` | `main/data/channel_manager.c:1464` | duplicate-id double-subscribe -> dangling subscriber on delete |
| 31 | medium | `83baaf3` | `main/data/channel_manager.c:1271` | transient read failure mistaken for corruption -> data demote |
| 33 | medium | `d19b9f6` | `main/storage/data_logger.c:203` | log filename seconds-since-boot collision truncated prior log |
| 37 | medium | `d19b9f6` | `main/net/serial_commands_logger.c:171` | serial replay.start path traversal (../ + any absolute path) |
| 38 | medium | `7518519` | `main/main.c:1394` | boot lv_timer_create without the LVGL lock (race) |
| 5 | low | `d19b9f6` | `main/net/serial_commands_upload.c:67` | serial font upload had no size cap |
| 14 | low | `d19b9f6` | `main/storage/can_upload.c:239` | CAN cloud upload CRLF header injection |
| 35 | low | `d19b9f6` | `main/storage/boot_assets.c:78` | boot_assets ignored fclose ENOSPC, left partial file 'valid' |

Commits: `7518519` initial sweep (12) · `83baaf3` channel durability (5) · `d19b9f6` upload/IO hardening (10) · `fceb678` OBD2 correctness (3). Regression tests: `426e266` (tests/native/test_hardening_fixes.c, 15 assertions).

## Remaining (6) — need owner decision or careful HW-testable work

### [9] HIGH — `main/net/web_server_ota.c:83`
- POST /api/ota/check and POST /api/ota/start have no authentication, so anyone who can reach the dash's HTTP server (home LAN, or the forced fallback hotspot) can start a firmware download and force an automatic reboot of the instrument display.
- **Status:** POLICY — the entire web API is unauthenticated on the LAN by design; adding auth to only OTA is inconsistent. Product decision: add a session token / shared secret across state-changing POSTs, or accept the LAN trust model.

### [13] MEDIUM — `main/include/can_upload_secret.h:20`
- The CAN-upload HMAC shared secret is hard-coded in firmware that ships with flash encryption and secure boot disabled, so any attacker who dumps a unit's flash recovers it and can attack the whole fleet's cloud bucket.
- **Status:** POLICY — needs a real secret rotation + wrangler worker redeploy, not just a code edit. Your CLAUDE.md already says rotate before customer rollout; consider per-device keys derived from device_id.

### [18] HIGH — `main/can/can_manager.c:896`
- can_suspend (bus-scan task, prio 5, core 1) checks nothing about an in-flight reconfigure: reconfigure_can_filter/can_set_promiscuous_mode/can_change_bitrate test s_suspended only at entry, then sleep in multiple vTaskDelay windows (~150-750 ms) during which the scan task can preempt, suspend, and take ownership — after which the resuming reconfigure reinstalls the driver and respawns the RX task underneath the scan.
- **Status:** DEFERRED (risky) — the bus-scan task can preempt an in-flight CAN reconfigure across its vTaskDelay windows and take ownership. A correct fix (re-check s_suspended after each delay, or a peripheral mutex) needs on-bus HW testing to verify it doesn't introduce a worse deadlock.

### [40] HIGH — `main/net/wifi_manager.c:658`
- wifi_manager_start() wraps esp_wifi_init (line 658), esp_wifi_set_mode (691), esp_wifi_set_config (721) and esp_wifi_start (727) in ESP_ERROR_CHECK, so a recoverable ESP_ERR_NO_MEM aborts the whole dash, contradicting the module's own soft-fail policy ('never abort() while driving') applied at lines 807/1012.
- **Status:** DEFERRED (boot-critical) — replacing the 4 ESP_ERROR_CHECK wrappers with graceful rollback is aligned with the module's own soft-fail policy, but the failure (ENOMEM at wifi init) is rare and the rollback/deinit semantics need the failure path exercised before shipping.

### [24] MEDIUM — `main/can/obd2.c:1774`
- A DTC read completes and clears s_dtc_req.active on the FIRST ECU response to the 0x7DF functional broadcast, silently discarding codes from every other ECU that answers the same request (later 0x43 responses fall through to the return at line 1861).
- **Status:** DEFERRED (UX tradeoff) — accumulating DTCs from all ECUs until timeout would make every read (incl. the common single-ECU case) wait the full timeout instead of returning on first response. Owner's call.

### [12] MEDIUM — `main/net/ota_handler.c:264`
- ota_free_internal_ram() permanently drops the SoftAP (APSTA → STA) at the start of every OTA attempt and nothing restores AP mode when the update fails — restore_wifi_settings() only restores the power-save mode — so a failed OTA leaves hotspot users cut off until reboot.
- **Status:** DEFERRED (niche) — a failed OTA drops APSTA->STA and never restores AP, cutting off hotspot users until reboot. Only bites if someone is on the dash's AP while an OTA runs over STA; moderate wifi-mode-restore risk for low value.

## Refuted by verification (5) — no action

- **[11] `main/net/ota_handler.c:612`** — The arithmetic premise is right (sdkconfig: CONFIG_FREERTOS_HZ=500, so pdMS_TO_TICKS(1)==0), but the claimed consequence is false. IDF v5.3.1 FreeRTOS source refutes "the
- **[19] `main/can/can_manager.c:231`** — Two-part claim; the substantive part (sustained drops from too-low drain ceiling) is refuted by incorrect arithmetic and a false premise. PART 1 — "drops newest, contradi
- **[20] `main/can/can_bus_test.c:293`** — The three load-bearing sub-claims fail against the actual code: (1) Spinlock ownership CANNOT be stranded. All TWAI spinlock critical sections are tiny and run with inter
- **[39] `main/main.c:586`** — The arithmetic half of the claim is correct but the asserted failure path (core-1 idle starvation) does not hold. main/main.c:583-586: uint32_t elapsed = (esp_timer_get_t
- **[41] `main/main.c:565`** — The claim's mechanism (LVGL task pets TWDT only after lv_timer_handler returns, so a heavy build inside one handler invocation runs un-pet) is structurally real, but the 
