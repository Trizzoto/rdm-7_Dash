"""
Contract tests for /api/system/* and /api/device/info — the diagnostics
surface that the on-device Diagnostics screen and Studio's status
strip both consume.
"""

from __future__ import annotations

from conftest import assert_keys, assert_types


def test_system_health_shape(api):
    """GET /api/system/health -> uptime + heap + psram + wifi snapshot.

    Note: the handover doc lists `temp_c`; the actual firmware does NOT
    return that field (no on-die temp sensor read in the handler). The
    truth is the C source — handler returns:
        uptime_s, heap_free, heap_min_free, psram_free, wifi_rssi.
    """
    resp = api.get("/api/system/health")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(
        body,
        "uptime_s",
        "heap_free",
        "heap_min_free",
        "psram_free",
        "wifi_rssi",
    )
    assert_types(
        body,
        uptime_s=int,
        heap_free=int,
        heap_min_free=int,
        psram_free=int,
        wifi_rssi=int,
    )
    assert body["uptime_s"] >= 0
    assert body["heap_free"] >= 0
    # heap_min_free is the low-water mark — must not exceed current free.
    assert body["heap_min_free"] <= body["heap_free"]


def test_device_info_top_level_shape(api):
    """GET /api/device/info -> nested device snapshot.

    Top-level keys: serial, schema, display, hardware, system, can,
    wifi, sd, signals. Firmware version is intentionally NOT exposed
    here (Studio reads it from elsewhere).
    """
    resp = api.get("/api/device/info")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(
        body,
        "serial",
        "schema",
        "display",
        "hardware",
        "system",
        "can",
        "wifi",
        "sd",
        "signals",
    )
    assert isinstance(body["serial"], str)
    assert isinstance(body["schema"], int) and not isinstance(body["schema"], bool)
    # Schema is on a monotonically increasing version. v13 is current; any
    # value < 1 means the firmware never set it (would be a bug).
    assert body["schema"] >= 1


def test_device_info_display_shape(api):
    """device.display has width, height, shape (rect|round)."""
    body = api.get("/api/device/info").json()
    display = body["display"]
    assert_keys(display, "width", "height", "shape")
    assert_types(display, width=int, height=int, shape=str)
    assert display["width"] > 0
    assert display["height"] > 0
    assert display["shape"] in ("rect", "round")


def test_device_info_signals_summary(api):
    """device.signals exposes total/fresh/stale counts."""
    body = api.get("/api/device/info").json()
    signals = body["signals"]
    assert_keys(signals, "total", "fresh", "stale")
    assert_types(signals, total=int, fresh=int, stale=int)
    # The arithmetic must be self-consistent — fresh + stale == total.
    assert signals["fresh"] + signals["stale"] == signals["total"], (
        "signals counts inconsistent: fresh=%d stale=%d total=%d"
        % (signals["fresh"], signals["stale"], signals["total"])
    )
