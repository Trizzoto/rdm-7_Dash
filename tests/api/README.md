# RDM-7 Dash — HTTP API Contract Tests

A Python pytest suite that pins the request/response shapes of the firmware's
`/api/*` endpoints. The goal is regression detection: if a firmware change
inadvertently alters a JSON shape Studio depends on, these tests catch it
before the desktop app does.

## What's covered

This is a **focused subset** — ~12-15 high-value endpoints, not all 86. The
priority list was driven by what Studio polls in its hot path plus the most
recent contract change (the structured 413 for oversized layouts).

| Area | Endpoint | File |
|---|---|---|
| Layout | `GET /api/layout/version` | `test_layout_api.py` |
| Layout | `GET /api/layout/list` | `test_layout_api.py` |
| Layout | `GET /api/layout/current` | `test_layout_api.py` |
| Layout | `POST /api/layout/save` (apply=1 + apply=0) | `test_layout_api.py` |
| Layout | `POST /api/layout/set` | `test_layout_api.py` |
| Layout | `POST /api/layout/save` (oversized) | `test_layout_too_large.py` |
| Signal | `GET /api/signals/values` | `test_signal_api.py` |
| Signal | `POST /api/signal/inject` (single + batch) | `test_signal_api.py` |
| System | `GET /api/system/health` | `test_system_api.py` |
| System | `GET /api/device/info` | `test_system_api.py` |
| Touch | `GET /api/touch` | `test_touch_api.py` |
| Touch | `POST /api/touch` | `test_touch_api.py` |
| Touch | `POST /api/screen/switch` | `test_touch_api.py` |

## Not yet covered (TODO)

The other ~70 endpoints are intentionally deferred. Add as needed when:
- A new endpoint becomes load-bearing for Studio.
- A bug surfaces that a contract test would have caught.

Endpoints worth adding next: `/api/storage/info`, `/api/sd/status`,
`/api/log/status`, `/api/replay/status`, `/api/can/config`,
`/api/wifi/config`, `/api/ota/status`, `/api/image/list`, `/api/font/list`.

## Running

### Mock-only (CI, fast iteration)

```bash
cd tests/api
pip install -r requirements.txt
python -m pytest -v
```

The default backend is `MockDeviceClient` — hand-rolled responses keyed off
`(method, path)`. No network. Deterministic. ~15 tests, runs in <1 s.

### Against a real device

```bash
python -m pytest tests/api/ --device-url=http://192.168.4.1
```

The same tests now hit a live RDM-7 over its hotspot (`192.168.4.1`) or
station IP. Some assertions are loose enough to pass on whatever the device
happens to be in the middle of (e.g. signal counts, heap free).

> **Caution**: `test_layout_save_*` and `test_signal_inject_*` mutate device
> state. They're safe on a bench unit — the layout save round-trips and
> the signal locks clear at next CAN frame — but don't run them mid-drive.

## How the fixtures work

`conftest.py` exposes one fixture, `api`. It returns either a `MockDeviceClient`
(default) or a `RealDeviceClient` (when `--device-url` is set). Both expose
the same surface:

```python
def test_something(api):
    resp = api.get("/api/some/path")
    resp = api.post("/api/some/path", json={...})
    assert resp.status_code == 200
    assert resp.json() == {...}
```

`resp` is either a `requests.Response` or a `FakeResponse` with the same
`.status_code` / `.json()` / `.text` interface. Tests don't need to know which.

## Adding a new endpoint test

1. Add a default mock response in `conftest.py::MockDeviceClient._install_defaults`:

   ```python
   self.set_response("GET", "/api/my/thing", 200, {"ok": True, "data": ...})
   ```

   For dynamic behaviour (request-dependent responses), pass a callable:

   ```python
   def _handler(body):
       if not body:
           return (400, {"error": "missing body"})
       return (200, {"ok": True, "echo": body})
   self.set_response("POST", "/api/my/thing", 200, _handler)
   ```

2. Write the test against the `api` fixture:

   ```python
   def test_my_thing_shape(api):
       """GET /api/my/thing -> {"ok": True, "data": ...}."""
       resp = api.get("/api/my/thing")
       assert resp.status_code == 200
       body = resp.json()
       assert_keys(body, "ok", "data")
   ```

3. Add an entry to the table above.

4. Run `python -m pytest -v` before committing.

## Maintenance

- **Mock fixtures must be updated in lockstep with firmware response shapes.**
  If you change a handler in `main/net/web_server.c`, update the matching
  default response in `conftest.py`. The tests assert against the firmware's
  truth — a stale mock just means the test will pass against the mock and
  fail against the device, which is exactly the symptom we want.
- The truth source is the C handler in `main/net/web_server.c`, NOT
  `docs/handover/07-web-server-api.md` — the doc has known drift (e.g.
  `/api/system/health` lists `temp_c`, but the handler returns `wifi_rssi`).
  Read the handler when in doubt.
- This suite is **not** wired into the firmware's ESP-IDF/pytest-embedded
  test harness (`pytest_rgb_panel_lvgl.py`). It's a separate Python project,
  intentionally — it doesn't need a flashed device to run.
