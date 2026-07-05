# RDM-7 Pre-Launch Hardening Sweep — Triage

_Generated 2026-07-05. 42 candidate defects from a 12-finder parallel audit, each adversarially verified by an independent high-effort agent (told to refute, confirm only on a traced failure path)._

**Result: 36 confirmed, 5 refuted.** 12 fixed this pass (build-clean + boot-verified); the rest are triaged below with the verifier's recommended fix.


## Fixed this pass (12)

| # | Sev | Location | Defect |
|---|---|---|---|
| 17 | high | `main/widgets/signal.c:367` | CAN ID 0x000 force-updated every OBD2/internal signal (can_id=0 sentinel collision) |
| 0 | high | `main/net/web_server_capture.c:294` | `/api/screenshot/hash` int-overflow in `x+w` clamp -> unauth OOB framebuffer read / crash |
| 7 | high | `main/net/ota_handler.c:272` | Dead OTA reentrancy guard -> two concurrent checks race shared response_buffer (double-free/UAF) |
| 8 | high | `main/net/ota_handler.c:231` | A check during a download reset status to AVAILABLE -> concurrent OTA installs on one partition |
| 10 | medium | `main/net/ota_handler.c:641` | `esp_https_ota_get_image_len_read` called after abort freed the handle (UAF) |
| 32 | high | `main/net/web_server_logger.c:325` | `/api/log/delete` could remove() the data logger's open file -> FS corruption |
| 36 | high | `main/net/web_server_logger.c:378` | `/api/log/upload` could open a 2nd handle on the active LFS log -> FS corruption |
| 34 | high | `main/storage/signal_replay.c:204` | `signal_replay_start` walked the whole CSV on the LVGL task -> dashboard freeze / TWDT |
| 25 | high | `main/can/obd2.c:395` | `obd2_stop()` froze an in-progress discovery scan (stuck 'in progress', bus on wrong bitrate) |
| 16 | medium | `main/can/obd2.c:1597` | OBD2 ISO-TP consecutive frame memcpy'd `dlc-1` (up to 14) bytes from an 8-byte frame -> OOB read |
| 15 | medium | `main/can/can_manager.c:335` | Extended 29-bit CAN IDs truncated to 11 bits -> extended-ID channels never decoded |
| 38 | medium | `main/main.c:1394` | Boot `lv_timer_create` ran without the LVGL lock while the LVGL task ran on the other core (race) |

## Confirmed, not yet fixed (24) — recommended backlog


### CAN reconfigure concurrency (needs careful design + HW test)

**[18] HIGH — `main/can/can_manager.c:896`**
- can_suspend (bus-scan task, prio 5, core 1) checks nothing about an in-flight reconfigure: reconfigure_can_filter/can_set_promiscuous_mode/can_change_bitrate test s_suspended only at entry, then sleep in multiple vTaskDelay windows (~150-750 ms) during which the scan task can preempt, suspend, and take ownership — after which the resuming reconfigure reinstalls the driver and respawns the RX task underneath the scan.
- _Fix:_ Re-check s_suspended after each vTaskDelay window in the three reconfigure functions and bail before reinstalling/respawning if a scan took ownership mid-flight. Minimal robust fix: guard the peripheral with a mutex that both can_suspend()/can_resume() and reconfigure_can_filter()/can_set_promiscuous_mode()/can_change_bitrate() acquire for the whole teardown/reinstall sequence, so a scan cannot in

### OTA / cloud policy decisions (product call)

**[9] HIGH — `main/net/web_server_ota.c:83`**
- POST /api/ota/check and POST /api/ota/start have no authentication, so anyone who can reach the dash's HTTP server (home LAN, or the forced fallback hotspot) can start a firmware download and force an automatic reboot of the instrument display.
- _Fix:_ Gate the two mutating OTA endpoints (and other state-changing POSTs) behind an auth check. Minimal: add a shared-secret/session-token check at the top of _ota_check_handler and _ota_start_handler (web_server_ota.c:60,83) that returns 401 when absent/mismatched; ideally centralize it as a helper called by all POST handlers. Longer-term, require the AP to always use WPA2 (reject WIFI_AUTH_OPEN) so t

**[13] MEDIUM — `main/include/can_upload_secret.h:20`**
- The CAN-upload HMAC shared secret is hard-coded in firmware that ships with flash encryption and secure boot disabled, so any attacker who dumps a unit's flash recovers it and can attack the whole fleet's cloud bucket.
- _Fix:_ Pre-customer: replace the shared static secret with a per-device key derived on-device (HKDF over device_id with a root secret that never ships to firmware); the worker re-derives the expected key from the X-Device-Id header before verifying the HMAC (already the documented plan in SECURITY.md:32-34). This limits a single dumped unit to its own device_id. Additionally, gate /can-list behind a sepa

**[12] MEDIUM — `main/net/ota_handler.c:264`**
- ota_free_internal_ram() permanently drops the SoftAP (APSTA → STA) at the start of every OTA attempt and nothing restores AP mode when the update fails — restore_wifi_settings() only restores the power-save mode — so a failed OTA leaves hotspot users cut off until reboot.
- _Fix:_ In restore_wifi_settings() (ota_handler.c:417), re-assert the correct mode via the wifi_manager instead of only restoring power-save: capture the mode at the top of ota_free_internal_ram() (or query s_ap_enabled) and, if the AP was dropped, restore it — e.g. call a wifi_manager helper that does `esp_wifi_set_mode(_has_saved_sta_creds()?WIFI_MODE_APSTA:WIFI_MODE_AP)` and re-applies the AP config. R

**[14] LOW — `main/storage/can_upload.c:239`**
- Untrusted make/model/notes strings from the web request are passed verbatim into esp_http_client_set_header without CRLF sanitization, allowing HTTP header injection into the outbound upload request.
- _Fix:_ Reject or strip CR/LF in the header values before use. Minimal: in `can_upload_start` (can_upload.c ~281), after the filename check, reject any of make/model/notes containing '\r' or '\n' (return ESP_ERR_INVALID_ARG), e.g. `if (strpbrk(make, "\r\n") || strpbrk(model, "\r\n") || (notes && strpbrk(notes, "\r\n"))) return ESP_ERR_INVALID_ARG;`. Alternatively sanitize at the web handler (web_server_lo

### OBD2 diagnostic correctness (bundle into OBD2 consolidation, Phase 3)

**[21] MEDIUM — `main/can/obd2.c:617`**
- All six one-shot state machines (test/dtc/clear/vin/ecuname/freeze) rely on _poll_timer_cb for timeout expiry, but none of them ensure s_poll_timer exists (only obd2_discovery_start does at line 2064), so with OBD2 PID polling idle a TX-ok/no-response one-shot leaves active=true forever and permanently bricks that request type until reboot.
- _Fix:_ Ensure the poll timer exists before arming any one-shot, or give the one-shots an independent timeout source. Minimal fix: add a helper `static void _ensure_poll_timer(void){ if(!s_poll_timer) s_poll_timer = lv_timer_create(_poll_timer_cb, OBD2_TICK_MS, NULL); }` and call it at the top of obd2_test_pid, _dtc_start, obd2_clear_dtcs, obd2_read_vin, obd2_read_ecu_name, and obd2_read_freeze_pid (right

**[22] MEDIUM — `main/can/obd2.c:1696`**
- The Mode 09 NRC handler attributes any service-0x09 rejection to the VIN request first, so when VIN and ECU-name requests are in flight together (they are fired back-to-back by device_settings.c:2758+2776 and web snapshot at web_server_obd2.c:434-437), an NRC for the unsupported ECU-name PID 0x0A cancels the VIN request instead.
- _Fix:_ Disambiguate the two Mode 09 requests in the NRC handler the same way the positive-response handlers do. Since an NRC does not echo the PID, the correct fix is to only attribute a service-0x09 NRC when exactly one of the two Mode 09 requests is active; if both are active, do not cancel either from the NRC (let them resolve via PID-matched positive response or per-request timeout). Concretely, repl

**[23] MEDIUM — `main/can/obd2.c:1544`**
- The single-frame guard `len_bytes < 2` drops a strict 1-byte positive response — specifically the bare Mode 04 clear ack (frame `01 44`) that some real ECUs send — so a successful DTC clear is reported to the user as a failure (this gap is even acknowledged in-file at lines 1371-1380).
- _Fix:_ Change the single-frame lower bound at obd2.c:1544 from `len_bytes < 2` to `len_bytes < 1` (i.e. `if (len_bytes < 1 || len_bytes > 7) return;`), matching ISO 15765-2's 1–7 valid SF length. `_process_full_message` already guards `if (len < 2) return;` at line 1650, so also relax that to `if (len < 1) return;` and confirm the Mode 04 handler at 1782 (which only checks `service == 0x04`) still fires 

**[24] MEDIUM — `main/can/obd2.c:1774`**
- A DTC read completes and clears s_dtc_req.active on the FIRST ECU response to the 0x7DF functional broadcast, silently discarding codes from every other ECU that answers the same request (later 0x43 responses fall through to the return at line 1861).
- _Fix:_ Do not tear down the DTC request on the first response. Instead accumulate into s_dtc_req.codes on each matching 0x43/0x47/0x4A frame (leave s_dtc_req.active true and return without firing the callback at obd2.c:1770-1776), and deliver the aggregated result once — from the timeout path at obd2.c:1009-1016 — changing that branch to fire cb(true, s_dtc_req.count>0?s_dtc_req.codes:NULL, s_dtc_req.cou

### channel_manager storage robustness (interrelated — do together)

**[27] HIGH — `main/data/channel_manager.c:1457`**
- channel_manager_load_from_lfs skips every 'custom_*' channel on boot (the Phase-2 skip), so custom channels that the first-run wizard, studio import, and the v2->v3 decode migration create AND persist to channels.json are silently dropped on the next reboot, and the very next save rewrites channels.json without them — permanent loss of their bindings, thresholds, and CAN decode.
- _Fix:_ In the load loop, when canonical_channel_exists(jid) is false, re-create the custom channel instead of skipping: read its stored metadata (label, group, cardinality, units_native/display, decimals, min, max) from the JSON object and call channel_manager_create_custom(...) to obtain c, then fall through to channel_from_json(c, jc) so decode/thresholds/colors/signal binding round-trip. Guard it on c

**[28] MEDIUM — `main/data/channel_manager.c:1464`**
- load_from_lfs subscribes the channel to its signal for EACH JSON entry with that id, and signal_subscribe does not de-dupe (signal.c:283-303) — a channels.json containing two entries with the same id leaves two subscriber slots pointing at one channel_t, but channel_free/signal_unsubscribe removes only one, so deleting that channel leaves a dangling subscriber that writes into freed memory on every subsequent CAN frame.
- _Fix:_ In the load loop, subscribe only once per channel: guard against re-subscribing a channel already bound to this signal. Simplest: before line 1465, skip if this channel is already subscribed to idx, e.g. track/compare `c->signal_index == idx` set on a prior iteration, or (better) de-dupe on the id itself — skip a JSON entry whose id was already processed this load. Alternatively make signal_subscr

**[29] HIGH — `main/data/channel_manager.c:1516`**
- save_to_lfs's lock-timeout fallback serializes s_channels UNGUARDED on the esp_timer task, but a 500 ms lock timeout does not imply LVGL is wedged — a long layout reload/wizard apply legitimately holds the recursive LVGL mutex for seconds while actively mutating s_channels (ensure_capacity realloc can move/free the pointer array; channel_free frees entries), so the unguarded iterator can dereference freed memory or persist torn garbage.
- _Fix:_ Do not serialize unguarded on lock-timeout. Replace the fall-through with an abort that keeps the save pending: at channel_manager.c:1514-1516, `if (!rdm_lvgl_lock(500)) { ESP_LOGW(TAG, "save: LVGL lock busy — deferring"); return ESP_ERR_TIMEOUT; }` and leave `s_dirty = true`. The debounce will re-fire, and end_bulk/flush on the LVGL task (which already holds the lock recursively) still commits du

**[30] HIGH — `main/data/channel_manager.c:1396`**
- The atomic-write recovery only runs when the live file exists-but-corrupt; power loss in the window between save_to_lfs's two renames (live->.bak at line 1577, then .tmp->live at line 1582) leaves NO live file, so the next boot returns ESP_ERR_NOT_FOUND and channel_manager_init reseeds factory defaults — destroying the fully-fsync'd new .tmp (truncated by the reseed save's fopen(CHM_TMP_PATH,"w")) and permanently stranding the previous-good .bak.
- _Fix:_ Make load_from_lfs attempt recovery even when the live file is ABSENT, not only when it exists-but-corrupt. Minimal change: in channel_manager_load_from_lfs, before the `!root && !existed` early-return at line 1396, check for a staged .tmp or .bak (e.g. stat(CHM_TMP_PATH)/stat(CHM_BAK_PATH)); if either exists, call chm_preserve_and_recover() (which already tries .tmp then .bak and republishes to t

**[31] MEDIUM — `main/data/channel_manager.c:1271`**
- chm_read_and_parse cannot distinguish 'file corrupt' from 'transient read failure' (fopen failure or malloc(st_size+1) OOM both return NULL with existed=true), so a perfectly good channels.json can be shunted aside to .corrupt and downgraded to the older .bak (or defaults) purely because of momentary heap pressure at boot.
- _Fix:_ Give chm_read_and_parse a third out-parameter (e.g. `bool *out_io_error`) set true on the fopen/malloc/fread failure branches (:1270, :1272, and a short-read at :1273) and left false only when the file was read but cJSON_Parse returned NULL. In channel_manager_load_from_lfs, when io_error is set, do NOT run chm_preserve_and_recover — instead return a distinct code (e.g. ESP_ERR_TIMEOUT/ESP_FAIL th

### Asset upload atomicity/validation (serial + font paths)

**[2] MEDIUM — `main/net/serial_commands_upload.c:157`**
- _handle_upload_finish() only LOGS 'Upload incomplete: %u/%u bytes' when received != total_size and then proceeds to write the truncated buffer to LittleFS as the live asset (for fonts with zero content validation, for images only a 4-byte magic check).
- _Fix:_ In _handle_upload_finish() (serial_commands_upload.c ~157), turn the incompleteness log into a hard failure for the image/font branch: if `s_upload.received != s_upload.total_size`, free the buffer, set `s_upload.active = false`, and `_send_error(id, "Upload incomplete")` with a return — do not write the file. (OTA already goes through esp_ota_end which validates the image, so this only needs to g

**[3] MEDIUM — `main/net/serial_commands_upload.c:188`**
- Serial upload.finish publishes with a non-atomic in-place fopen(path, "wb") (which truncates the existing good asset immediately) and remove(path) on short write, with no free-space pre-check and no tmp+rename staging — unlike the hardened atomic web image path in web_server_assets.c:122-210.
- _Fix:_ Mirror the web path's atomic staging in `_handle_upload_finish`: build `path.tmp`, `fopen(tmp,"wb")`, `fwrite`, `fflush`+`fsync`+`fclose`, and only on full success `rename(path, path.bak); rename(tmp, path); remove(path.bak);` — restoring the `.bak` on rename failure. Add an `esp_littlefs_info("littlefs", ...)` free-space pre-check in `_handle_upload_start` before allocating the buffer, and abort 

**[4] MEDIUM — `main/net/web_server_assets.c:531`**
- font_upload_handler writes the TTF in place with fopen(path, "wb") (truncating any existing font of that name) and remove(path) on partial write, with no fsync and no tmp+bak+rename staging — the atomicity fix applied to the image upload path 400 lines above was never applied to fonts.
- _Fix:_ Mirror the image path: fopen a "%s.tmp" staging file, `fflush`+`fsync(fileno(f))` before close, `rename(path, bak)` then `rename(tmp, path)` with `rename(bak, path)` restore on failure, and `remove(bak)` on success. Then register in font_manager only after the atomic publish succeeds.

**[5] LOW — `main/net/serial_commands_upload.c:67`**
- _handle_upload_start() enforces IMAGE_MAX_SIZE for images only — font uploads have no size cap (web path enforces FONT_MAX_FILE_SIZE = 512 KB at web_server_assets.c:475) and neither serial type has a LittleFS free-space check, so an oversized font both wastes flash permanently and can fill the partition.
- _Fix:_ In _handle_upload_start (serial_commands_upload.c ~line 67), mirror the web path: add `if (is_font && total_size > FONT_MAX_FILE_SIZE) { _send_error(id, "Font too large"); return; }`, and for both image and font add a LittleFS free-space pre-check via `esp_littlefs_info("littlefs", &total, &used)` rejecting when `total_size > (total-used)`, before allocating the PSRAM buffer.

**[6] MEDIUM — `main/net/serial_commands_upload.c:62`**
- Serial upload.start/finish never call boot_assets_is_protected_image()/boot_assets_is_protected_font(), so built-in protected assets that the web handlers refuse to touch (web_server_assets.c:80 and :468 return 403) can be overwritten with arbitrary data over serial.
- _Fix:_ In serial_commands_upload.c _handle_upload_start (or _handle_upload_finish before fopen), add the same guard the web handlers use: after the `_name_is_safe` check at :62, reject protected assets — `if (is_image && boot_assets_is_protected_image(name)) { _send_error(id, "Cannot overwrite built-in image"); return; }` and `if (is_font && boot_assets_is_protected_font(name)) { _send_error(id, "Cannot 

### Logging / replay / misc

**[1] MEDIUM — `main/net/web_server.c:192`**
- web_server_recv_body_raw() retries forever on HTTPD_SOCK_ERR_TIMEOUT ('if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;'), so a stalled or silently-dead client wedges the single httpd task permanently; reachable from the assets endpoints sd_copy_handler (web_server_assets.c:834) and sd_delete_handler (web_server_assets.c:957) and every other web_server_recv_body() caller.
- _Fix:_ Bound the timeout retries in web_server_recv_body_raw. Track consecutive HTTPD_SOCK_ERR_TIMEOUT results and abort after a small cap (e.g. a total deadline or N retries), returning -1: int timeouts = 0; while (total < want) { int r = httpd_req_recv(req, buf + total, want - total); if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++timeouts > 2) return -1; // ~60s max with 30s SO_RCVTIMEO continue; } timeouts

**[33] MEDIUM — `main/storage/data_logger.c:203`**
- Log filenames are derived from seconds-since-boot and opened with mode "w", so a session started N seconds after boot silently truncates any previous boot's log that was also started N seconds after boot.
- _Fix:_ Make the filename collision-resistant. Minimal: before fopen, if s_filename already exists (stat), append a disambiguator — a static monotonically-increasing session counter or a short retry loop bumping a suffix (log_%lu_%u.csv) until stat() fails. Alternatively open with a create-exclusive path (fopen mode "wx") and, on EEXIST, increment a suffix and retry. Either removes the same-second-since-b

**[37] MEDIUM — `main/net/serial_commands_logger.c:171`**
- Serial replay.start accepts any absolute path verbatim and does not run _filename_is_safe on relative names (snprintf into /sdcard/logs/%s at line 174 allows ../ traversal), unlike the web handler (web_server_logger.c:652-663) which enforces tier prefixes and rejects ".." — so a serial client can make signal_replay fopen and parse any file on either filesystem.
- _Fix:_ In _handle_replay_start (serial_commands_logger.c:171-175), mirror the web handler: for the fn[0]=='/' branch, require a tier prefix and reject "..": if (!((strncmp(fn,"/sdcard/",8)==0)||(strncmp(fn,"/lfs/",5)==0)) || strstr(fn,"..")) { _send_error(id,"Invalid log path"); free(a); return; }. For the relative branch, gate on _filename_is_safe(fn) before the snprintf into "/sdcard/logs/%s" (reject o

**[35] LOW — `main/storage/boot_assets.c:78`**
- _write_if_missing() treats any existing file with st_size > 0 as valid and never re-seeds it, while its own failure path leaves exactly such a file behind: on a short fwrite (LFS full) the partial file is not unlinked, and fclose()'s return (which commits buffered data on LittleFS and can fail on ENOSPC) is ignored at line 90, returning ESP_OK for a truncated file.
- _Fix:_ In _write_if_missing (main/storage/boot_assets.c ~line 84-97): capture and check fclose's return, and unlink the partial file on any failure so the next boot re-seeds. E.g.: size_t written = fwrite(data, 1, len, f); int close_rc = fclose(f); if (written != len || close_rc != 0) { ESP_LOGE(TAG, "seed write failed for %s (%u/%u, close=%d)", path, (unsigned)written, (unsigned)len, close_rc); unlink(p

## Refuted by verification (5) — no action

- **[11] `main/net/ota_handler.c:612`** — The arithmetic premise is right (sdkconfig: CONFIG_FREERTOS_HZ=500, so pdMS_TO_TICKS(1)==0), but the claimed consequence is false. IDF v5.3.1 FreeRTOS source refutes "the yield never happens": - tasks
- **[19] `main/can/can_manager.c:231`** — Two-part claim; the substantive part (sustained drops from too-low drain ceiling) is refuted by incorrect arithmetic and a false premise. PART 1 — "drops newest, contradicting 'drop oldest' comment" (
- **[20] `main/can/can_bus_test.c:293`** — The three load-bearing sub-claims fail against the actual code: (1) Spinlock ownership CANNOT be stranded. All TWAI spinlock critical sections are tiny and run with interrupts disabled: twai_receive d
- **[39] `main/main.c:586`** — The arithmetic half of the claim is correct but the asserted failure path (core-1 idle starvation) does not hold. main/main.c:583-586: uint32_t elapsed = (esp_timer_get_time()/1000) - start_time; uint
- **[41] `main/main.c:565`** — The claim's mechanism (LVGL task pets TWDT only after lv_timer_handler returns, so a heavy build inside one handler invocation runs un-pet) is structurally real, but the concrete 15s-panic path does n

## Note on #27 (custom_* channels dropped on reboot)
Confirmed HIGH and already FIXED + hardware-verified in a parallel session (see memory `custom-channels-reboot-fix`). #28 (duplicate-id double-subscribe) is an adjacent latent bug in the same load loop — fold into that area.