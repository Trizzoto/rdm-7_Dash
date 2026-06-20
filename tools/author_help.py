"""Author per-field `help` text in widgets.schema.json. The editor shows it as a
hover tooltip via the small i icon next to each setting (and, later, the
on-device inspector). Keyed widget -> field -> text. Idempotent; preserves the
schema's CRLF + inline-option formatting for a clean diff. Run both codegens
afterward. Start with arc; extend the HELP map per widget over time.
"""
import json, io, re

HELP = {
    "arc": {
        # Value / scale
        "start_angle_user": "Where the arc begins, in degrees (0° = 3 o'clock, increasing clockwise). The fill grows from here.",
        "sweep_degrees": "How far the arc travels from the start angle. 270 = a classic three-quarter gauge, 360 = full circle.",
        "signal_min": "Channel value mapped to the empty (start) end of the arc.",
        "signal_max": "Channel value mapped to the full (end) end of the arc.",
        "tick_min": "First value the tick scale covers. Usually matches Signal Min; set differently to label only part of the range.",
        "tick_max": "Last value the tick scale covers. Usually matches Signal Max.",
        "anchor_enabled": "Grow the fill outward from a fixed anchor value instead of from the start - ideal for centre-zero gauges like boost/vacuum.",
        "anchor_value": "The value the fill grows out from when Anchor Curve is on.",
        "anchor_position": "Where the anchor sits along the arc, as a percent of its length (0 = start, 100 = end).",
        "reverse": "Flip the fill so it grows from the high end toward the low end.",
        # Behaviour
        "smoothing_ms": "Damping time for fill/needle movement. Higher = smoother but laggier; 0 = instant, raw response.",
        # Fill
        "arc_width": "Thickness of the lit (value) arc, in pixels.",
        "arc_offset": "Shifts the whole ring in/out from the widget centre, in pixels. Use to nest concentric arcs.",
        "arc_color": "Colour of the lit (value) portion of the arc.",
        "bg_arc_color": "Colour of the unlit track behind the fill.",
        "bg_arc_width": "Thickness of the background track. Make it wider or narrower than the fill for a recessed or raised look.",
        "rounded_ends": "Round the ends of the fill and track instead of flat-cut.",
        "lead_edge_color": "Colour of the leading-edge marker.",
        "lead_edge_width": "Width of the leading-edge marker, in pixels.",
        "arc_image": "Use an image as the track instead of a solid colour.",
        "arc_image_full": "Use an image as the lit fill instead of a solid colour - revealed progressively as the value rises.",
        # Redline (threshold comes from the bound channel, not a widget field)
        "redline_color": "Colour of the redline band. Its position comes from the bound channel's high-warning threshold.",
        "redline_arc_width": "Thickness of the redline band arc.",
        "redline_recolor_fill": "When the value enters the redline zone, recolour the whole fill (not just the band) to the redline colour.",
        # Ticks
        "show_ticks": "Draw tick marks around the arc.",
        "major_tick_step": "Value distance between major (largest) ticks. e.g. 1000 on a 0-8000 tach = a major tick every 1000.",
        "mid_tick_step": "Value distance between medium ticks. 0 = no medium tier.",
        "minor_tick_step": "Value distance between minor (smallest) ticks. 0 = no minor tier.",
        "major_tick_width": "Thickness of the major ticks, in pixels.",
        "mid_tick_width": "Thickness of the medium ticks, in pixels.",
        "minor_tick_width": "Thickness of the minor ticks, in pixels.",
        "major_tick_length": "Length of the major ticks, in pixels.",
        "mid_tick_length": "Length of the medium ticks, in pixels.",
        "minor_tick_length": "Length of the minor ticks, in pixels.",
        "major_tick_color": "Colour of the major ticks.",
        "mid_tick_color": "Colour of the medium ticks.",
        "minor_tick_color": "Colour of the minor ticks.",
        # Numbers
        "show_tick_labels": "Draw numeric labels at the major ticks.",
        "tick_label_font": "Font (family:size) for the tick number labels.",
        "tick_label_color": "Colour of the tick number labels.",
        "label_gap": "Gap between the tick ring and the number labels, in pixels. Larger pushes labels toward the centre.",
        "tick_label_divisor": "Divide label values before display. 1000 -> a 7000 tick reads '7' (e.g. RPM x1000).",
        "ticks_on_top": "Draw ticks and labels above the fill instead of behind it.",
        # Needle / value line
        "show_value_line": "Draw a thin radial line pointing at the current value.",
        "value_line_width": "Thickness of the value line, in pixels.",
        "value_line_color": "Colour of the value line.",
        "value_line_r_mod": "Trim the value line's length inward from the outer edge, in pixels.",
        # Alerts
        "arc_alerts_enabled": "Recolour the fill when the value crosses the thresholds below - independent of the channel redline.",
        "arc_low": "At or below this value the fill uses the Low Color.",
        "arc_low_color": "Fill colour when the value is at or below the Low Threshold.",
        "arc_high": "At or above this value the fill uses the High Color.",
        "arc_high_color": "Fill colour when the value is at or above the High Threshold.",
    },
}

PATH = "schema/widgets.schema.json"
S = json.load(io.open(PATH, encoding="utf-8"))
widgets = S.get("widgets") or S.get("widget_types")
wmap = {w["name"]: w for w in widgets}

applied = 0
missing = []
for wname, fields in HELP.items():
    w = wmap.get(wname)
    if not w:
        missing.append(wname); continue
    fmap = {f["name"]: f for f in w["fields"]}
    for fn, text in fields.items():
        f = fmap.get(fn)
        if not f:
            missing.append(wname + "." + fn); continue
        if f.get("help") != text:
            f["help"] = text; applied += 1

txt = json.dumps(S, ensure_ascii=False, indent=2)
txt = re.sub(r'\{\s*\n\s*"value": ([^\n]+?),\s*\n\s*"label": ([^\n]+?)\s*\n\s*\}',
             r'{ "value": \1, "label": \2 }', txt)
with io.open(PATH, "w", encoding="utf-8", newline="\r\n") as fp:
    fp.write(txt + "\n")

print("applied/updated %d help string(s)" % applied)
if missing:
    print("MISSING:", missing)
for wn in HELP:
    w = wmap[wn]
    has = sum(1 for f in w["fields"] if f.get("help"))
    print("  %-8s coverage: %d/%d fields have help" % (wn, has, len(w["fields"])))
