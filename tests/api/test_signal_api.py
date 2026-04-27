"""
Contract tests for /api/signals/* endpoints.

Signals are the firmware's CAN decode layer; these endpoints expose the
current decoded snapshot and let Studio inject test values that bypass
CAN entirely.
"""

from __future__ import annotations

import pytest

from conftest import assert_keys, assert_types


def test_signals_values_shape(api):
    """GET /api/signals/values -> {"signals": [{name, value, stale, can_id}]}.

    Each signal is the live decoded snapshot. `stale` flips true when no
    frame for that signal has been received in the last 2 s (firmware-side
    timeout). `can_id` is the source CAN frame ID (0 for synthetic /
    internal signals like CALCULATED_GEAR).
    """
    resp = api.get("/api/signals/values")
    assert resp.status_code == 200
    body = resp.json()
    assert_keys(body, "signals")
    assert isinstance(body["signals"], list)

    for sig in body["signals"]:
        assert_keys(sig, "name", "value", "stale", "can_id")
        assert isinstance(sig["name"], str)
        assert sig["name"] != ""
        assert isinstance(sig["value"], (int, float))
        assert isinstance(sig["stale"], bool)
        # CAN ID is uint32; Python sees plain int. Standard 11-bit IDs go up
        # to 0x7FF; 29-bit extended IDs up to 0x1FFFFFFF. 0 is valid for
        # internally-driven synthetic signals.
        assert isinstance(sig["can_id"], int) and not isinstance(sig["can_id"], bool)
        assert sig["can_id"] >= 0


def test_signal_inject_single_value(api):
    """POST /api/signal/inject {"signal":"NAME","value":N} -> {"status":"ok"}.

    Single-signal form. Locks the named signal until /api/signal/clear so
    incoming CAN frames don't overwrite the test value.
    """
    resp = api.post(
        "/api/signal/inject",
        json={"signal": "RPM", "value": 3500.0},
    )
    assert resp.status_code == 200
    body = resp.json()
    assert body == {"status": "ok"}


def test_signal_inject_batch(api):
    """POST /api/signal/inject {"values":[...]} -> {"status":"ok"}.

    Batch form is undocumented in the user-facing handover but is what
    Studio's sim mode uses. Up to 16 signals per call. Same response.
    """
    resp = api.post(
        "/api/signal/inject",
        json={
            "values": [
                {"signal": "RPM", "value": 1200.0},
                {"signal": "VEHICLE_SPEED", "value": 55.0},
                {"signal": "COOLANT_TEMP", "value": 92.0},
            ]
        },
    )
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}
