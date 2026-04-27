"""
Regression test for the layout_too_large structured 413 response.

Background: a layout JSON > LAYOUT_MAX_FILE_BYTES (32 KiB) used to be
rejected with an opaque HTTPD_400. The editor had no way to surface a
useful inline message — auto-save would just silently fail.

The fix (web_server.c::_send_layout_too_large) returns a 413 with a body
the editor can parse:

    { "ok": false, "error": "layout_too_large", "max": 32768, "actual": N }

This test pins that contract. If it fails, either the firmware regressed
or the cap moved — update LAYOUT_MAX_FILE_BYTES in:
  - main/layout/layout_manager.h
  - main/web/index.html (RDM_LAYOUT_MAX_BYTES)
  - tests/api/conftest.py (mock handler)
  - this test
"""

from __future__ import annotations

import json

import pytest


# Match LAYOUT_MAX_FILE_BYTES in main/layout/layout_manager.h.
LAYOUT_MAX_FILE_BYTES = 32768


def _build_oversized_payload(target_bytes):
    """Construct a syntactically-valid layout JSON whose serialised size
    is just over `target_bytes` bytes when written by `requests`.

    We pad a single widget's `label` field with filler text, since the
    label is a free-form string and won't trip any other validation
    before the size check runs.
    """
    base = {
        "name": "default",
        "schema_version": 13,
        "widgets": [
            {
                "type": "panel",
                "slot": 0,
                "x": 0,
                "y": 0,
                "w": 200,
                "h": 100,
                "label": "",
            }
        ],
    }
    # Approximate non-label overhead; pad the rest.
    overhead = len(json.dumps(base).encode("utf-8"))
    pad_needed = (target_bytes - overhead) + 32  # extra to guarantee strictly >
    base["widgets"][0]["label"] = "X" * pad_needed
    return base


def test_layout_save_rejects_oversized_payload(api):
    """POST /api/layout/save with body > 32 KiB -> 413 with structured error.

    Asserts:
      1. HTTP status is 413 Payload Too Large.
      2. Body is exactly {ok:false, error:"layout_too_large", max:32768, actual:N}.
      3. `actual` is reported as a positive integer (server's view of the
         received content length, NOT zero or -1).
    """
    payload = _build_oversized_payload(LAYOUT_MAX_FILE_BYTES + 1024)
    serialised = json.dumps(payload).encode("utf-8")
    assert len(serialised) > LAYOUT_MAX_FILE_BYTES, (
        "test setup error: built payload is only %d bytes" % len(serialised)
    )

    resp = api.post("/api/layout/save", json=payload)
    assert resp.status_code == 413, (
        "expected 413 for oversized layout, got %d (body=%r)"
        % (resp.status_code, resp.text)
    )

    body = resp.json()
    assert body.get("ok") is False, "ok must be false (got %r)" % body.get("ok")
    assert body.get("error") == "layout_too_large", (
        "unexpected error code: %r" % body.get("error")
    )
    assert body.get("max") == LAYOUT_MAX_FILE_BYTES, (
        "max must equal %d (got %r)" % (LAYOUT_MAX_FILE_BYTES, body.get("max"))
    )
    actual = body.get("actual")
    assert isinstance(actual, int) and actual > LAYOUT_MAX_FILE_BYTES, (
        "actual must be an int > max; got %r" % actual
    )


def test_layout_save_accepts_payload_below_cap(api):
    """Sanity inverse: a small valid payload still returns 200.

    Without this counter-check the oversized test could silently pass if
    the handler always returned 413.
    """
    payload = {
        "name": "default",
        "schema_version": 13,
        "widgets": [],
    }
    resp = api.post("/api/layout/save", json=payload)
    assert resp.status_code == 200, (
        "small payload must be accepted; got %d (body=%r)"
        % (resp.status_code, resp.text)
    )
