# Security Posture & Release Checklist

This document records the security-relevant decisions in the firmware as it stands today, and the items that should be addressed before broad customer release.

It is intentionally honest about dev-phase shortcuts. If you're cutting a customer-facing release, walk the [pre-release checklist](#pre-release-hardening-checklist) and either close each item or accept the residual risk explicitly.

## What the firmware exposes

| Surface | Reachable from | Authentication | Threat model |
|---|---|---|---|
| HTTP web editor on port 80 | LAN (STA) + device hotspot (AP) | None | Local network. Hotspot password protects the AP itself. |
| OTA updates | HTTPS to `github.com/Trizzoto/RDM-7_Dash/releases/...` | TLS cert chain via ESP-IDF mbedTLS bundle | Server-authenticated. Manifest signature **not currently verified** — see below. |
| CAN raw-log cloud upload | HTTPS to a Cloudflare Worker on a fixed origin | HMAC-SHA256 over a static device-shared secret | Replay-limited by `±10 min` timestamp window and per-file naming. |
| USB Serial JTAG console | Physical USB port | None (physical access) | Trusted-physical-access only. Default ESP-IDF console; not hardened. |
| GPIO inputs (wire indicators) | Physical wiring | None | Trusted-physical-access only. |
| SD card (FAT mounted from SPI) | Physical insertion | None | Trusted-physical-access only. |

## Known shortcuts (dev phase)

### 1. CAN upload HMAC secret is in the source tree

**File**: `main/include/can_upload_secret.h`
**Status**: Acceptable dev-phase only.

The shared secret used to sign CAN raw-log uploads to the R2 bucket is hard-coded into the firmware binary. The header comment explicitly flags this:

> "Anyone with a flashed binary can extract it. Rotate before shipping to customers."

The matching `wrangler secret put CAN_UPLOAD_HMAC_SECRET` on the Cloudflare Worker side must remain consistent.

**Pre-customer fix**:
- Derive the per-device key from `device_id` (which is unique per chip) using HKDF or a similar KDF with a single root secret on the worker.
- Worker verifies by re-deriving the expected key from `device_id` (sent in the upload header) before validating the HMAC.
- Root secret never ships to firmware.

### 2. OTA manifest signature unverified

**File**: `main/net/ota_handler.c`
**Status**: TLS-only authentication today.

OTA update binaries are fetched over HTTPS, so the connection is authenticated against the GitHub Releases endpoint. The manifest JSON itself is not signed.

This means: an attacker with control of the GitHub repo (or a successful TLS MITM with a forged cert chain) could push malicious firmware. The first is mitigated by GitHub's account security; the second by mbedTLS root cert bundle integrity.

**Pre-customer fix** (optional but recommended):
- Sign the manifest with an ED25519 private key off-device.
- Bake the public key into firmware.
- Verify the manifest signature before fetching the binary.
- The signed-manifest path is what Tauri itself does for its auto-updater — pattern is well-trodden.

### 3. WiFi credentials at rest in NVS

**Status**: ESP-IDF default — credentials are stored unencrypted in the NVS partition.

Flash encryption is not enabled (`CONFIG_SECURE_BOOT` / `CONFIG_FLASH_ENCRYPTION` are off in `sdkconfig`). Anyone with physical access to the chip and a soldered SPI-flash reader can read user WiFi passwords.

**Pre-customer fix** (depending on threat model):
- Enable NVS encryption (`CONFIG_NVS_ENCRYPTION`) — straightforward, derives keys from the eFuse.
- Or accept the residual risk and document it. Most consumer dashboards (Haltech, AIM) make the same trade-off.

### 4. No rate limiting on the HTTP server

**Status**: Acceptable for the local-network threat model.

The web editor accepts unlimited connections, unlimited request rate. A malicious LAN user could load-attack the device, but the only consequence is service unavailability — there's no persistent state corruption path that a fast loop unlocks. The 32 KB layout-size cap protects against the most plausible accidental DoS (oversized layout JSON).

**Pre-customer fix**: not needed unless threat model expands to public-internet exposure.

### 5. `RDM7_DEBUG_KEEP_CONSOLE=1` default

**File**: `main/main.c`
**Status**: Acceptable for shipping. Console-on-USB stays available for crash diagnosis.

When this flag is set (currently the default), `uart_protocol_init()` is skipped so ESP_LOGx output stays routable to the USB Serial JTAG console. The desktop app's USB transport is therefore unavailable — users on USB must connect over WiFi instead.

Trade-off is between (a) shipping with logs visible for support diagnosis, (b) enabling the USB-serial transport that the desktop app's offline mode needs. Today we ship (a). The flag can be flipped per-build or via OTA.

## Pre-release hardening checklist

Walk this list before any "1.0 / GA / customer" release. Each item is independent; close them in any order.

**Status legend**: `[ ]` open · `[x]` done · `[~]` audited and deferred to a later release · `[-]` waived (documented decision)

### Walked 2026-05-19 (pre-v1.1.11 stabilisation window)

- [~] **Rotate the CAN upload HMAC secret** to a per-device-derived key. Documented as [shortcut #1](#1-can-upload-hmac-secret-is-in-the-source-tree). Deferred to **v1.2** — proper per-device keying requires a flashing-pipeline change (eFuse provisioning or NVS first-boot key generation + worker-side device-id mapping). Acceptable for v1.1.11 customer trials with the documented release-note. The secret in the firmware repo is meant to deter casual abuse, not protect anything sensitive.
- [~] **Sign OTA manifests** with ED25519 and verify on-device. Deferred to **v1.2** — implementation is straightforward (mimic Tauri auto-updater's signed-manifest flow) but too risky 2 weeks before release without dev/test cycles for the signing key generation, manifest publishing, and rollback story. Today's authentication is TLS to GitHub Releases, which is acceptable for the current threat model.
- [-] **Enable Supabase Leaked Password Protection** (Marketplace) — **Waived until Supabase Pro upgrade.** Free tier doesn't expose the toggle. Documented in MEMORY.md.
- [-] **Decide on flash encryption** posture — **Waived for v1.1.11.** Current decision: ship without `CONFIG_FLASH_ENCRYPTION`. Trade-off: faster OTA, simpler factory provisioning, no risk of bricked devices from lost eFuse keys. Residual risk: someone with physical access to the chip + a SPI-flash reader can dump WiFi credentials. Matches the trade-off most aftermarket dash brands (Haltech, AIM, MoTeC) make. Re-evaluate at v2.0.
- [x] **Audit the dependency tree** — Done 2026-05-19:
  - **Firmware managed components** (`main/idf_component.yml` + `managed_components/`): `esp_new_jpeg` 0.6.1 (latest), `littlefs` 1.20.4 (latest), `LVGL` 8.3.11 (latest 8.x — v9 deliberately skipped). No outstanding security advisories on any.
  - **Desktop Cargo** (`rdm7-desktop/src-tauri/Cargo.lock`): `cargo audit` walked the 566-crate dependency tree. Found **3 actual vulnerabilities** in `rustls-webpki 0.103.10`:
    - RUSTSEC-2026-0098 — name constraints for URI names incorrectly accepted (TLS cert validation flaw)
    - RUSTSEC-2026-0099 — name constraints accepted for wildcard cert names (TLS cert validation flaw)
    - RUSTSEC-2026-0104 — reachable panic in CRL parsing (DoS on malformed CRL)

    All three pull in via `reqwest → tauri-plugin-updater`, so they sit directly on the auto-updater TLS path. **Fixed same-day** by `cargo update -p rustls-webpki` → 0.103.13. Verified `cargo audit --deny warnings` returns clean exit 0 post-bump.

    Plus 20 "no longer maintained" advisories (GTK3 transitives + build-time parsers + narrow-path unsoundness). All triaged and explicitly ignored in `src-tauri/.cargo/audit.toml` with per-entry rationale — none are exploitable in our deployment.
- [x] **Verify the firmware build is reproducible** from a clean checkout — Done 2026-05-19: `idf.py fullclean && idf.py build` reproduces firmware size 0x27aa40 (2,599,488 bytes) consistent with the v1.1.11 build. Binary SHA-256 differs run-to-run because ESP-IDF embeds build-time UUIDs/timestamps; **size + partition layout are stable**, which is what matters for OTA upgrade size budgets.
- [x] **Pre-shipped factory layout** — Done 2026-05-19: factory default lives in code (`main/layout/default_layout.c`), not as a JSON file. Widget positions only — no WiFi creds, no debug signal sources, no API keys. `RDM7_DEBUG_KEEP_CONSOLE=1` ships by default so console logs stay routable to USB for support diagnostics (documented trade-off: USB transport in the desktop app is therefore unavailable; users connect via WiFi).
- [x] **Boot-time secrets** — Done 2026-05-19: grep'd all `ESP_LOG[IWED]` calls in `main/` for sensitive strings. Findings:
  - WiFi credentials logged as metadata only (`"WiFi credentials saved for '%s'"` logs SSID, not password)
  - AP password updates log auth-mode + "updated/cleared" status, not the password itself
  - CAN upload HMAC secret is never logged
  - OTA endpoint URL is in code at `main/net/ota_handler.c:37` but doesn't ship as part of any runtime log

### New (post-v1.1.11)

- [x] **Fix portability bug in `web_server_name_is_safe`** — Done 2026-05-19, same-day: cast to `(unsigned char)` before the `< 0x20` check in both `web_server_name_is_safe` and `web_server_filename_is_safe`. Test `test_name_utf8_is_accepted` flipped to assert TRUE for `éclair` and `日本` and locks the cast in. Will ship in v1.1.12 (currently `[Unreleased]`).
- [x] **Wire `cargo audit` into desktop CI** — Done 2026-05-19 on rdm7-desktop@`84b4c47`. GitHub Actions workflow runs `cargo audit --deny warnings` on every push/PR touching `src-tauri/Cargo.{toml,lock}` plus a weekly Mon 08:00 UTC schedule. Prebuilt cargo-audit binary cached, sub-30 s runs after first cache hit.

## Reporting a vulnerability

If you find a security issue:

- **Privately**: open a draft security advisory on the GitHub repo (`Security` tab → `Report a vulnerability`).
- Or email Tommy directly: tommyrosato@gmail.com.

Do not file public issues for unpatched vulnerabilities. Coordinated disclosure preferred; we'll ack within 72 hours.

## Out of scope

- Side-channel attacks against the ESP32-S3 (power analysis, RF emission, fault injection). Threat model is consumer-class theft of dash-stored data, not nation-state.
- Physical attacks against the chip (decapping, microprobing). Same.
- Compromised developer workstations. The repo's CI signing keys, if/when introduced, are in-scope; the firmware authors' laptops are not.

## References

- ADR 0001 — Wi-Fi onboarding reliability (relevant to AP security stance)
- `main/include/can_upload_secret.h` (the dev-phase secret)
- `CLAUDE.md` § "CAN Cloud Upload" (the cloud upload pipeline)
- ESP-IDF security documentation: https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/security/
