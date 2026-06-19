# RDM-7 Dash — Stress Test Findings (2026-06-19)

Target: live device **192.168.4.61** (also COM27), fw running, sim ON, layout `ford_cluster`.
Harness: `tools/_stress/stress.py` (HTTP API battery). Raw log: `run.log`, data: `results.json`.
~2000 requests across 11 phases (~9 min wall). Active layout backed up + restored.

## Verdict: STABLE

- **0 crashes, 0 new panics** (`panic_count` 270 → 270, NVS-persisted so a panic would have shown).
- **0 reboots** (uptime monotonic 12741 → 13283 s).
- **No heap leak** — net **+8956 bytes** over the whole run; heap returned to baseline (2.69 MB free).
- **URI registration: 0 failures** (138/160 handlers).
- **Input validation solid** — garbage JSON, empty body, missing fields, 5000-char signal name, OOB
  touch coords: all rejected cleanly (400) or no-op (200). Nothing crashed the device.

The handlers are defensively coded: under pressure they return an error status and bail rather than
faulting. That's *why* nothing panicked.

## Performance baseline (serial, ~idle)

| Endpoint | median | p95 | max | notes |
|---|---|---|---|---|
| `/api/touch` | 84 ms | 121 | 137 | lightest |
| `/api/signal/inject` (7 sigs) | 142 ms | 1393 | 2437 | big tail |
| `/api/widget/set` | 180 ms | 1079 | 1140 | |
| `/api/screenshot` (q80) | 259 ms | 345 | **2267** | live-preview poll; ~3.3 fps ceiling |
| `/api/channels` (67 ch) | 903 ms @c6 | 1061 | 1862 | large JSON build |
| `/api/layout/preview` (full rebuild) | 863 ms | 1050 | 1247 | whole-screen rebuild |
| `/api/layout/set` (switch) | 121 ms | 1604 | 1604 | light fast, Altezza ~1.6 s |

## Findings (by severity)

### 1. Concurrency collapses — single LVGL lock + few httpd workers serialize everything
Under concurrent load, heavy handlers hold the LVGL lock and everyone else queues to the 15 s client ceiling.
- `mixed_conc12`: **301/360 ok, 41 timeouts, 18× HTTP 500**, median 2381 ms, p95/max ~15 s.
- `screenshot_conc8`: median **1316 ms**, max **15018 ms**, 2 timeouts.
- `signal_inject_conc8`: p95 3640 ms, max 15005 ms.
This is the dominant performance issue. Most real clients are serial (editor poll), so it mainly bites
when Studio + mirror + a second browser hit the device at once.

### 2. `/api/widget/transform` drops ~25% under rapid drag (503)
- `widget_transform`: **50/200 → 503**.  `widget_set`: **15/200 → 503**.
- Root: `rdm_lvgl_lock(1000)` 1 s timeout — `web_server_touch.c:338` (transform) and `:410` (set).
  During a live drag the editor fires these rapidly; when the LVGL render task holds the lock, the
  acquire times out → 503 → that edit is silently dropped. The editor's 80 ms coalescing masks most of
  it, but mid-drag drops are possible and there's no client retry on 503.

### 3. 500s under load are transient PSRAM alloc failures, not crashes
The concurrent-mix 500s come from `/api/channels` OOM guards (`web_server_channels.c` — many) and
screenshot/JSON buffers all allocating from PSRAM at once. Handlers detect the failed `malloc` and
return 500 (no crash). But: screenshot already does the *nice* thing for its busy case
(`web_server_capture.c:144` → 503 + `Retry-After`), while channels/others escalate to 500. Inconsistent —
500 makes Studio fire error toasts + back off, 503+Retry-After is the intended "try again" signal.

### 4. Oversized POST body → connection reset (not a clean 4xx)
`preview_huge` (2000-panel layout) → `ConnectionResetError`; device closed the socket. No crash, heap
recovered, but the client gets a reset instead of a 413/400. Should enforce a max content-length up front.

### 5. Screenshot is the steady-state cost driver
259 ms median, tail to 2.3 s. This is the editor live-preview poll. The dedup cache + idle pacing
(already in place) are the right mitigations; concurrent screenshots are the pathological case.

### 6. `bad-id` widget/set returns 503 (1 s) rather than a fast 404
A nonexistent widget id during a busy moment waits on the lock and 503s instead of failing fast.

## Recommended improvements
1. **Unify transient-failure responses on 503 + `Retry-After`** (not 500) for OOM / lock-busy, and have
   the editor retry-on-503 with backoff. Biggest UX win for the least risk.
2. **Make widget transform/set tolerant of the busy lock** — either queue via `lv_async_call` (coalesced)
   so a drag never drops, or raise the 1 s timeout, or client-side retry on 503.
3. **Cap concurrent heavy GETs server-side** (channels could reuse a static build buffer behind a mutex,
   like screenshot caps concurrent JPEG encodes) to stop PSRAM alloc storms → fewer 500s.
4. **Enforce a max request body size** with a clean 413 before reading, instead of socket reset.
5. Optional: a tiny **per-client request serialization** in the editor when several panels are open.

## Fixes applied + verified (2026-06-19)

All four implemented and re-tested with the same harness (`run_after.log`). Firmware built
clean (19% app free) + flashed COM27; device rebooted into it (uptime reset, panic 270→270).

Error-rate before → after (same battery):

| Phase | Before | After |
|---|---|---|
| `screenshot_conc8` | 46/48, 2 timeouts, max 15.0 s | **48/48, 0 err**, max 7.2 s |
| `signal_inject_conc8` | 198/200, 2 timeouts | **200/200, 0 err** |
| `widget_set` | 185/200, **15× 503** | **200/200, 0 err** |
| `widget_transform` | 150/200, **50× 503** | **200/200, 0 err** |
| `mixed_conc12` | 301/360, 41 timeouts + **18× 500** | **354/360, 6 timeouts, 0× 500** |
| `preview_huge` | `ConnectionResetError` | **HTTP 413** clean |
| `huge_signal_name` | 400 | **413** clean |
| `widget_set_badid` | 503 (1.07 s) | **200 found:false** (0.11 s) |

- **#1 503-unify**: the LVGL-busy 500s in channels mutations now return 503+Retry-After
  (`web_server_send_busy`).
- **#2 transform/set**: 2 s lock acquire + 503 helper (firmware) and one-shot 503 retry in
  `_doWidgetTransform`/`_doFieldSet` (editor). 503 rate 50/200 + 15/200 → **0**.
- **#3 channels gate**: `s_channels_list_mux` serializes the heavy list build; concurrent
  builds shed as 503 instead of OOM-500. The mixed-load 500s → **0**.
- **#4 clean 413**: `web_server_drain_body()` drains the rejected body (bounded 384 KB) before
  `web_server_send_layout_too_large` / oversize `recv_body`, so the client reads the 413
  instead of an RST.

Stability held: 0 crashes, 0 new panics, no leak (the −320 KB net is the one-time screenshot
dedup cache, not a leak — every post-screenshot phase delta is ~0).

Residual (NOT addressed — would need an architecture change, out of scope for these fixes):
- Extreme concurrency (12) still has a few requests hit the 15 s client ceiling
  (`mixed_conc12`: 6 timeouts) — fundamental to the single LVGL lock + small httpd worker pool.
- `/api/selftest` can take >20 s right after a 30× full-rebuild preview burst (each preview
  queues an async rebuild on the LVGL task; selftest waits behind them). Inherent to the
  deferred-preview design.
- Latency also improved across the board, but the after-run started from a fresh boot (healthy
  PSRAM, no fragmentation) vs the before-run on a 3.5 h-up device — so treat the *error-rate*
  eliminations as the definitive win, not the raw latency deltas.

## NOT covered (future passes)
- On-device touch-UI navigation stress (skipped deliberately to protect the live `ford_cluster` setup).
- CAN bus flooding (no physical bus on the bench; `bus_errors` 248 M is bench error-state spam, not traffic).
- OTA / erase / flash-write storms (need explicit OK; flash wear).
- Multi-hour soak (this was ~9 min; a long soak would better surface slow fragmentation).
- Font/image upload storms (risk of filling LittleFS).
