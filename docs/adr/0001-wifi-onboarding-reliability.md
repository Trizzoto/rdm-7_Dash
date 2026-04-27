# ADR 0001 — Wi-Fi Onboarding Reliability

**Status**: Accepted (in production since 2026-04-19)
**Context**: Phones must be able to join the device's `RDM7-XXXX` AP and reach the embedded web editor without specialised tooling. Several layered fixes were needed to make this reliable across iOS, Android, Windows, and Firefox. This ADR consolidates the rationale so future contributors don't reintroduce regressions.

## The problem we were solving

A user-facing onboarding flow:

1. Power on the dash.
2. On their phone, see the `RDM7-XXXX` access point.
3. Connect.
4. Phone OS auto-pops the captive-portal sheet → tap → web editor loads.

Empirically, on a stock ESP-IDF setup, this flow fails silently at multiple points. Phones either fail to associate, mark the AP "no internet" and refuse to route browser traffic, or the editor times out fetching its first asset. Each fix below addresses one of those failure modes.

## Decision

Ship all of the following in the default firmware build. Do not remove any of them without first reproducing and re-fixing the failure mode they address.

### 1. AP-only mode when no saved STA

**File**: [main/net/wifi_manager.c](../../main/net/wifi_manager.c)

If `wifi_cfg/count == 0` (no saved STA networks), skip starting STA entirely and run in `WIFI_MODE_AP`. The default `WIFI_MODE_APSTA` makes ESP-IDF retry connecting to a non-existent STA constantly, which logs `Haven't to connect` repeatedly and starves the single radio so the AP becomes unreliable.

### 2. AP channel 11 (not channel 1)

**File**: [main/net/wifi_manager.c](../../main/net/wifi_manager.c) — set in both `wifi_manager_start` and `wifi_manager_enable_ap`.

In the user's RF environment, channel 1 was congested enough that 802.11 association timed out (reason=15, 4-way handshake failed) on phones. Switching to channel 11 fixed it consistently. **Future improvement**: scan at boot, pick the cleanest channel automatically. Until then, channel 11 is the empirical safe default.

### 3. Force HT20 bandwidth

**File**: [main/net/wifi_manager.c](../../main/net/wifi_manager.c) — `esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20)` after `esp_wifi_start`.

HT40 negotiation fails on weak phone clients and on routers that don't have a clear secondary channel. HT20 throughput is plenty for the editor (a few KB/s once `index.html` is cached) and connection success is what we optimise for here.

### 4. Captive-portal HTTP probe handlers

**File**: [main/net/web_server.c](../../main/net/web_server.c) — 9 URIs registered.

iOS, Android, Windows, and Firefox each probe a different URL after joining a Wi-Fi network to detect captive portals. We answer all of them with **HTTP 302 + non-empty body** pointing at `/`:

| OS | Path |
|---|---|
| iOS | `/hotspot-detect.html`, `/library/test/success.html` |
| Android | `/generate_204`, `/gen_204`, `/generate204` |
| Windows | `/connecttest.txt`, `/ncsi.txt`, `/redirect` |
| Firefox | `/success.txt` |

The non-empty body is required for iOS — a bare 302 with no body still leaves iOS marking the AP "no internet" silently.

### 5. DNS hijack on UDP:53

**File**: [main/net/dns_hijack.c](../../main/net/dns_hijack.c). Started from `main.c` after `web_server_start()`.

Captive-portal probes happen via DNS first. Without a DNS responder on the AP, Android's probe to `connectivitycheck.gstatic.com` times out → OS marks the AP "no internet" → refuses to route browser traffic. Our DNS server answers all A queries with the AP IP (`192.168.4.1` by default, queried live from `esp_netif_get_ip_info(WIFI_AP_DEF)`). AAAA and unknown qtypes return NOERROR with zero answers (suppresses retry loops).

**Memory note**: the task stack lives in PSRAM (`xTaskCreateWithCaps(MALLOC_CAP_SPIRAM)`). Internal RAM is fragmented after WiFi init and the responder needs ~3 KB. Buffers are in BSS, not on the stack.

### 6. `CONFIG_HTTPD_MAX_REQ_HDR_LEN = 1024`

**File**: [sdkconfig](../../sdkconfig).

Default 512 B was too small for Android webview headers; the request would be rejected with a 431 and a downstream `newlib` lock-init abort. **2048 B was tried and rolled back** — burned ~14 KB across 7 simultaneous sessions and starved heap for `fopen`. 1024 B is the sweet spot.

### 7. `CONFIG_LWIP_MAX_SOCKETS = 12`

**File**: [sdkconfig](../../sdkconfig).

Mild headroom for parallel webview connections (Chrome opens 6+ for one page) without blowing the DRAM cost.

## Things tried and reverted (do not reintroduce)

| Change | Why it was rejected |
|---|---|
| `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM` bumped from 16 → 24 | Crashed `esp_wifi_init` with `ESP_ERR_NO_MEM` on cold boot. |
| `WIFI_AUTH_OPEN` for the AP | Debug-only convenience. Restored to conditional WPA2_PSK; users want a password. |
| Switching mDNS to `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y` | The correct fix in principle, but the project chose to drop mDNS entirely (files and dependency removed 2026-04-27). QR code + IP fallback cover the user experience. |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN = 2048` | Too aggressive, see above. |
| Auto-start STA with empty creds in hopes of "just trying" | ESP-IDF logs error spam and starves the AP. |

## What we did NOT do (and why)

- **Channel scanning at boot**. Would reduce dependence on the channel 11 default. Deferred because it adds ~2 s to boot and the channel 11 default has been reliable for users we've heard from. If a user reports association failures and channel 11 is at fault, this is the first thing to add.
- **Captive-portal full HTML form**. A "click to continue" page would be marginally clearer to users than the immediate redirect, but it adds a step. The current 302 + redirect lands them in the editor without any extra tap.
- **Routing STA traffic through the AP** (acting as a true router). Out of scope; our deployment assumes either AP-only or STA-only at a time, with the AP as the configuration-and-fallback path.

## How to verify in the field

- iPhone: should pop the captive-portal "Sign in" sheet within ~5 s of joining the AP. The sheet should load the editor.
- Pixel/Samsung Android: same behaviour. If the sheet says "Internet may not be available," the DNS hijack isn't running — check the boot log for `dns_hijack: started`.
- Windows: connecting to the AP in the system tray should auto-open Edge to the editor.
- Firefox: needs to click "Open network login page" in the URL bar.

If any of those fail, the most common cause in our experience is mismatched build of `web_server.c` (captive handlers missing) or missing `dns_hijack.c` from `main/CMakeLists.txt` `SRCS`.

## Related

- [main/net/wifi_manager.c](../../main/net/wifi_manager.c) — items 1–3.
- [main/net/web_server.c](../../main/net/web_server.c) — item 4.
- [main/net/dns_hijack.c](../../main/net/dns_hijack.c) — item 5.
- [sdkconfig](../../sdkconfig) — items 6–7.
- [docs/handover/07-web-server-api.md](../handover/07-web-server-api.md) §captive-portal-probes for endpoint detail.
