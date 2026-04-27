"""
Pytest fixtures for RDM-7 Dash HTTP API contract tests.

Two backends share a common surface (.get / .post) so individual tests don't
care whether they're running against a real device or a canned mock:

  * MockDeviceClient  — returns hand-rolled responses keyed off (method, path).
                         Used by default. CI-friendly, deterministic.
  * RealDeviceClient  — thin requests wrapper. Activated by --device-url.

Both return the same Response shape: a small object with `.status_code`
and `.json()`.  Tests assert on those — no `requests`-specific behaviour
leaks into the test bodies.
"""

from __future__ import annotations

import json
import re
from typing import Any, Dict, Tuple

import pytest


# ---------------------------------------------------------------------------
# Shared response object
# ---------------------------------------------------------------------------


class FakeResponse(object):
    """A minimal stand-in for requests.Response, used by the mock client."""

    def __init__(self, status_code, body):
        self.status_code = status_code
        # Body may be a str (for raw bytes), a dict, or None.
        self._body = body

    def json(self):
        if isinstance(self._body, (dict, list)):
            return self._body
        if isinstance(self._body, str):
            return json.loads(self._body)
        raise ValueError("response has no JSON body")

    @property
    def text(self):
        if isinstance(self._body, str):
            return self._body
        return json.dumps(self._body)

    @property
    def content(self):
        return self.text.encode("utf-8")


# ---------------------------------------------------------------------------
# Mock client
# ---------------------------------------------------------------------------


class MockDeviceClient(object):
    """
    Returns canned responses for known (method, path) pairs.

    Path is matched as the URL path without query string. Tests can
    override responses for a single endpoint via `.set_response`.
    """

    def __init__(self):
        # (method, path) -> (status_code, body)
        self._routes = {}  # type: Dict[Tuple[str, str], Tuple[int, Any]]
        # (method, path_pattern_compiled) -> handler(body) -> (status, body)
        self._dynamic = []
        self._install_defaults()

    # -- public surface used by tests ------------------------------------

    def get(self, path, params=None):
        clean = _strip_query(path)
        return self._dispatch("GET", clean, body=None)

    def post(self, path, json=None, data=None):  # noqa: A002 — matches requests
        clean = _strip_query(path)
        body = json if json is not None else data
        return self._dispatch("POST", clean, body=body)

    # -- mutation hooks --------------------------------------------------

    def set_response(self, method, path, status_code, body):
        self._routes[(method.upper(), path)] = (status_code, body)

    # -- internals -------------------------------------------------------

    def _dispatch(self, method, path, body):
        # Static routes first.
        key = (method, path)
        if key in self._routes:
            status, payload = self._routes[key]
            # Allow callable payload for dynamic responses (e.g. echo body).
            if callable(payload):
                resolved = payload(body)
                if isinstance(resolved, tuple):
                    status, payload = resolved
                else:
                    payload = resolved
            return FakeResponse(status, payload)

        # Dynamic patterns next (e.g. layout-too-large size check).
        for pattern_method, pattern, handler in self._dynamic:
            if pattern_method == method and pattern.match(path):
                status, payload = handler(body)
                return FakeResponse(status, payload)

        return FakeResponse(404, {"error": "no mock route", "path": path})

    def _add_dynamic(self, method, path_regex, handler):
        self._dynamic.append((method, re.compile(path_regex), handler))

    def _install_defaults(self):
        # ---- Layout endpoints ----
        self.set_response("GET", "/api/layout/version", 200, {"v": 42})

        self.set_response(
            "GET",
            "/api/layout/list",
            200,
            {
                "active": "default",
                "layouts": ["default", "track", "street"],
            },
        )

        self.set_response(
            "GET",
            "/api/layout/current",
            200,
            {
                "name": "default",
                "schema_version": 13,
                "widgets": [],
            },
        )

        self.set_response(
            "POST",
            "/api/layout/set",
            200,
            {"status": "ok"},
        )

        # /api/layout/save — dynamic so we can model the layout_too_large path.
        # Mock client uses _dispatch for static routes; the layout-save handler
        # is wired separately because it's the most contract-sensitive.
        def _layout_save(body):
            payload_str = body if isinstance(body, str) else json.dumps(body or {})
            actual = len(payload_str.encode("utf-8"))
            if actual > 32768:
                return (
                    413,
                    {
                        "ok": False,
                        "error": "layout_too_large",
                        "max": 32768,
                        "actual": actual,
                    },
                )
            return (200, {"status": "ok"})

        # Use a dedicated route slot so tests can still override it.
        self.set_response("POST", "/api/layout/save", 200, _layout_save)

        # ---- Signal endpoints ----
        self.set_response(
            "GET",
            "/api/signals/values",
            200,
            {
                "signals": [
                    {"name": "RPM", "value": 2400.0, "stale": False, "can_id": 0x360},
                    {"name": "VEHICLE_SPEED", "value": 0.0, "stale": True, "can_id": 0x361},
                ],
            },
        )

        self.set_response(
            "POST",
            "/api/signal/inject",
            200,
            {"status": "ok"},
        )

        # ---- System endpoints ----
        self.set_response(
            "GET",
            "/api/system/health",
            200,
            {
                "uptime_s": 1234,
                "heap_free": 250000,
                "heap_min_free": 200000,
                "psram_free": 2000000,
                "wifi_rssi": -55,
            },
        )

        self.set_response(
            "GET",
            "/api/device/info",
            200,
            {
                "serial": "RDM7-ABC123",
                "schema": 13,
                "display": {"width": 800, "height": 480, "shape": "rect"},
                "hardware": {
                    "chip": "esp32s3",
                    "cores": 2,
                    "psram_mb": 8.0,
                    "flash_mb": 16.0,
                },
                "system": {
                    "uptime_s": 1234,
                    "heap_free": 250000,
                    "heap_min_free": 200000,
                    "psram_free": 2000000,
                    "logger_active": False,
                    "replay_active": False,
                },
                "can": {
                    "state": "running",
                    "rx_pending": 0,
                    "tx_errors": 0,
                    "rx_errors": 0,
                    "bus_errors": 0,
                    "rx_missed": 0,
                },
                "wifi": {
                    "state": "ap_only",
                    "ssid": "",
                    "sta_ip": "",
                    "ap_enabled": True,
                    "ap_ssid": "RDM7-ABCD",
                    "ap_ip": "192.168.4.1",
                },
                "sd": {"mounted": False},
                "signals": {"total": 12, "fresh": 10, "stale": 2},
            },
        )

        # ---- Touch / CONTROL ----
        self.set_response("GET", "/api/touch", 200, {"enabled": False})
        self.set_response("POST", "/api/touch", 200, {"enabled": True})
        self.set_response(
            "POST",
            "/api/screen/switch",
            200,
            {"status": "ok"},
        )


# ---------------------------------------------------------------------------
# Real device client
# ---------------------------------------------------------------------------


class RealDeviceClient(object):
    """Thin wrapper around `requests` targeting a live device URL."""

    def __init__(self, base_url, timeout=10):
        # Lazy import so mock-only runs don't pay the requests import cost
        # if the dependency is somehow missing.
        import requests  # noqa: WPS433

        self._requests = requests
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def get(self, path, params=None):
        url = self.base_url + path
        return self._requests.get(url, params=params, timeout=self.timeout)

    def post(self, path, json=None, data=None):  # noqa: A002 — matches requests
        url = self.base_url + path
        return self._requests.post(
            url, json=json, data=data, timeout=self.timeout
        )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _strip_query(path):
    idx = path.find("?")
    return path if idx < 0 else path[:idx]


# ---------------------------------------------------------------------------
# Pytest plumbing
# ---------------------------------------------------------------------------


def pytest_addoption(parser):
    parser.addoption(
        "--device-url",
        action="store",
        default=None,
        help=(
            "If set, run API tests against a real device (e.g. "
            "'http://192.168.4.1'). Otherwise the mock client is used."
        ),
    )


@pytest.fixture
def api(request):
    """
    Returns an HTTP client whose .get/.post calls hit either a mock
    or a real device, depending on --device-url.
    """
    url = request.config.getoption("--device-url")
    if url:
        return RealDeviceClient(url)
    return MockDeviceClient()


@pytest.fixture
def is_real_device(request):
    """True when --device-url was supplied. Tests can branch on this for
    mutation/destructive cases that should be skipped against the mock."""
    return request.config.getoption("--device-url") is not None


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------


def assert_keys(obj, *keys):
    """Assert the dict has every key in `keys`. Pretty error on failure."""
    assert isinstance(obj, dict), "expected dict, got %r" % type(obj)
    missing = [k for k in keys if k not in obj]
    assert not missing, "missing keys %r in %r" % (missing, sorted(obj.keys()))


def assert_types(obj, **expected):
    """Assert obj[key] is of the given type for each kwarg.

    Use bool checks before int (Python's bool is an int subclass).
    """
    for key, typ in expected.items():
        assert key in obj, "missing key %r" % key
        if typ is bool:
            assert isinstance(obj[key], bool), (
                "key %r expected bool, got %r" % (key, type(obj[key]))
            )
        elif typ is int:
            assert isinstance(obj[key], int) and not isinstance(obj[key], bool), (
                "key %r expected int, got %r" % (key, type(obj[key]))
            )
        else:
            assert isinstance(obj[key], typ), (
                "key %r expected %r, got %r" % (key, typ, type(obj[key]))
            )
