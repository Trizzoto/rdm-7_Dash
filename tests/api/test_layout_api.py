"""
Contract tests for /api/layout/* endpoints.

These document the shape Studio expects from the firmware. If a test fails
after a firmware change, either the firmware response shape drifted (bug)
or the contract changed intentionally (update the test + the doc in
docs/handover/07-web-server-api.md).
"""

from __future__ import annotations

import pytest

from conftest import assert_keys, assert_types


def test_layout_version_returns_v_field(api):
    """GET /api/layout/version -> {"v": <int>}.

    Studio polls this every 3 s to detect external edits without paying
    the cost of fetching the full layout JSON. Must stay tiny.
    """
    resp = api.get("/api/layout/version")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "v")
    # `v` is a uint32 cast to long in firmware; Python sees a plain int.
    assert isinstance(body["v"], int), "v must be an integer (got %r)" % type(body["v"])
    assert body["v"] >= 0


def test_layout_list_shape(api):
    """GET /api/layout/list -> {"active": str, "layouts": [str, ...]}.

    `active` is the currently-loaded layout name (defaults to "default" if
    NVS read fails). System layouts prefixed with `_` (e.g. `_splash_*`)
    are deliberately filtered out of `layouts`.
    """
    resp = api.get("/api/layout/list")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "active", "layouts")
    assert isinstance(body["active"], str)
    assert isinstance(body["layouts"], list)
    for name in body["layouts"]:
        assert isinstance(name, str)
        assert not name.startswith("_"), (
            "system layouts (prefix `_`) must not appear in /api/layout/list"
        )


def test_layout_current_has_name_and_widgets(api):
    """GET /api/layout/current -> active layout JSON.

    Always contains `name` and `widgets`. `schema_version` may be absent on
    very old saved layouts; firmware backfills it on next save.
    """
    resp = api.get("/api/layout/current")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "name", "widgets")
    assert isinstance(body["name"], str)
    assert isinstance(body["widgets"], list)


def test_layout_save_returns_status_ok(api):
    """POST /api/layout/save -> {"status": "ok"} (200) on a valid payload.

    Note: firmware uses `{"status":"ok"}` here, not the more common
    `{"ok":true}`. Both shapes exist in different endpoints — Studio
    treats either as success but tests must match the actual handler.
    """
    payload = {
        "name": "default",
        "schema_version": 13,
        "widgets": [],
    }
    resp = api.post("/api/layout/save", json=payload)
    assert resp.status_code == 200, "save failed: %s" % resp.text
    body = resp.json()
    assert body == {"status": "ok"}, "unexpected save body: %r" % body


def test_layout_save_apply_zero_skips_reload(api):
    """POST /api/layout/save?apply=0 -> auto-save (no hot reload).

    The same handler is hit; only the in-memory side-effect differs.
    The wire response should be identical to apply=1.
    """
    payload = {
        "name": "default",
        "schema_version": 13,
        "widgets": [],
    }
    resp = api.post("/api/layout/save?apply=0", json=payload)
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}


def test_layout_set_returns_status_ok(api):
    """POST /api/layout/set with {"name":"<n>"} -> {"status":"ok"}.

    Triggers async reload via lv_async_call. The HTTP response returns
    immediately — Studio polls /api/layout/version to confirm the swap.
    """
    resp = api.post("/api/layout/set", json={"name": "default"})
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "status")
    assert body["status"] == "ok"
