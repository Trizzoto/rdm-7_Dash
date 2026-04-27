"""
Contract tests for the CONTROL-mode endpoints: /api/touch and
/api/screen/switch.

These are how Studio's CONTROL mode forwards remote touch events to the
firmware's virtual LVGL indev (see system/remote_touch.c).
"""

from __future__ import annotations

from conftest import assert_keys


def test_touch_get_returns_enabled_state(api):
    """GET /api/touch -> {"enabled": <bool>}.

    Read-only state probe. Studio uses this to render the CONTROL toggle's
    initial state before issuing any touch events.
    """
    resp = api.get("/api/touch")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "enabled")
    assert isinstance(body["enabled"], bool)


def test_touch_post_event_returns_enabled_state(api):
    """POST /api/touch with {x, y, state} -> {"enabled": <bool>}.

    The body uses `state: "down"|"up"|"move"`, NOT `pressed: bool` (the
    handover doc is wrong — verified against api_touch_post_handler in
    main/net/web_server.c). Optional `enabled` flips the master switch.
    Response always echoes the post-call enabled state.
    """
    resp = api.post(
        "/api/touch",
        json={"x": 400, "y": 240, "state": "down", "enabled": True},
    )
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "enabled")
    assert isinstance(body["enabled"], bool)


def test_screen_switch_returns_status_ok(api):
    """POST /api/screen/switch?screen=dashboard -> {"status":"ok"}.

    Switches between the splash editor and the live dashboard. Note this
    handler reads from the URL query string, NOT a JSON body — calling
    `.post(path, json=...)` with a non-empty body is fine but the body is
    ignored. Valid values: "splash", "dashboard". Anything else -> 400.
    """
    resp = api.post("/api/screen/switch?screen=dashboard")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "status")
    assert body["status"] == "ok"
