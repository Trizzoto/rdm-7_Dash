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

- [ ] **Rotate the CAN upload HMAC secret** to a per-device-derived key. Update both firmware and worker. Old key is now garbage. (See [shortcut #1](#1-can-upload-hmac-secret-is-in-the-source-tree).)
- [ ] **Sign OTA manifests** with ED25519 and verify on-device. Or document the accepted residual risk. (See [shortcut #2](#2-ota-manifest-signature-unverified).)
- [ ] **Enable Supabase Leaked Password Protection** (Marketplace) — Pro-plan gated; marked deferred in MEMORY.md.
- [ ] **Decide on flash encryption** posture. If yes: enable, regenerate keys, document the recovery path (lost eFuse keys = bricked device). If no: document the decision here.
- [ ] **Audit the dependency tree** — `idf_component.yml` declares `espressif/esp_new_jpeg ^0.6.0`. Walk the `managed_components/` tree for CVE exposure pre-release. `cargo audit` on the desktop side likewise.
- [ ] **Verify the firmware build is reproducible** from a clean checkout. Document any developer-machine-specific assumptions.
- [ ] **Pre-shipped factory layout**: confirm `default.json` ships safe defaults (no specific user WiFi creds, no specific HMAC, no `RDM7_DEBUG_KEEP_CONSOLE=0` for end users).
- [ ] **Boot-time secrets**: confirm nothing prints the HMAC secret, OTA endpoint, or any user PII at `ESP_LOGI` level in the default build.

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
