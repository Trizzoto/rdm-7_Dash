"""Cluster design registry.

Each design module exposes a uniform interface consumed by gen.py / deploy.py:
  NAME           str               CLI key + dist subdir
  IMAGE_NAME     str               baked-bg name (layout image_name + upload name)
  build_background() -> PIL.Image  supersampled RGBA art (gen.py downscales+bakes)
  build_layout() -> dict           layout JSON (overlays the live widgets)
  STATES         dict[str, dict]   named inject states for screenshot verification
  DEFAULT_STATE  str               which STATES key deploy.py uses by default
Optional:
  render_mock(big, state, path)    standalone composited preview PNG
  emit_preview()                   write a browser-preview bundle (Altezza WASM harness)
"""
import importlib

REGISTRY = {
    "altezza": "designs.altezza",
    "ktm": "designs.ktm",
    "mercedes": "designs.mercedes",
    "race": "designs.race",
}


def names():
    return list(REGISTRY)


def get(name):
    if name not in REGISTRY:
        raise KeyError(f"unknown cluster '{name}'. known: {', '.join(REGISTRY)}")
    return importlib.import_module(REGISTRY[name])
