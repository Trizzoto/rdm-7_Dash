# v2 Channel Architecture — Implementation Plan

**Status**: Active. ADR-0006 accepted. Source-of-truth for implementation.
**Updated**: 2026-06-06.

This is the working implementation doc for the v2 channel architecture.
Pairs with:
- [ADR 0006](adr/0006-channel-architecture-v2.md) — the decision record
- [`schema/canonical_channels.md`](../schema/canonical_channels.md) — channel registry data
- [`docs/v2_cleanup_log.md`](v2_cleanup_log.md) — running log of stale code removed during implementation

---

## 1. Data Model

### 1.1 Canonical channel registry

Firmware-baked, compile-time constant array. Read-only at runtime.
~90 entries. Auto-generated from `schema/canonical_channels.md`.

```c
// main/data/canonical_channels.h
typedef struct {
    const char *id;            // "rpm", "coolant_temp", ...
    const char *label;         // "RPM", "Coolant Temp"
    channel_group_t group;     // CHGRP_ENGINE_CORE, CHGRP_DRIVETRAIN, ...
    uint8_t     tier;          // 1, 2, 3
    channel_cardinality_t card; // SCALAR / ENUM / BOOLEAN
    const char *units_native;
    const char *units_display_default;
    uint8_t     decimals;
    int32_t     min_default;
    int32_t     max_default;
    int32_t     low_critical;  // INT32_MIN if unset
    int32_t     low_warn;
    int32_t     high_warn;     // INT32_MAX if unset
    int32_t     high_critical;
    uint32_t    color_normal;  // 0xRRGGBB
} canonical_channel_def_t;

extern const canonical_channel_def_t CANONICAL_CHANNELS[];
extern const size_t CANONICAL_CHANNEL_COUNT;
```

### 1.2 Runtime channel state

Per-channel struct held in heap (PSRAM). Created when user activates a
canonical channel OR loads one from `/lfs/channels.json`.

```c
// main/data/channel.h
typedef struct channel {
    char     id[32];            // canonical id or "custom_..."
    char     label[32];         // user-overridable
    uint8_t  tier;
    channel_group_t group;
    channel_cardinality_t card;

    /* Source */
    char     signal_name[32];   // which signal feeds this channel
    int16_t  signal_index;      // runtime cache (-1 if unbound)

    /* Format */
    char     units_native[8];
    char     units_display[8];  // user-overridable
    uint8_t  decimals;

    /* Range — display + sanity */
    int32_t  min, max;
    int32_t  sanity_min, sanity_max;

    /* Threshold profile — sentinel values when unset */
    int32_t  low_critical;
    int32_t  low_warn;
    int32_t  high_warn;
    int32_t  high_critical;

    /* Zone colors — 0xFFFFFFFF = use convention default */
    uint32_t color_normal;
    uint32_t color_low_warn;
    uint32_t color_low_critical;
    uint32_t color_high_warn;
    uint32_t color_high_critical;

    /* Runtime live state */
    float    current_value;
    bool     is_stale;
    uint32_t last_update_ms;

    /* Listeners (each widget bound to this channel) */
    channel_listener_t *listeners;
    uint8_t              listener_count;

    /* Calculated-channel hook (NULL for plain signal-fed channels) */
    void (*calculate_fn)(struct channel *);
} channel_t;
```

### 1.3 Channel manager

Single-instance global. Owns the channel table.

```c
// main/data/channel_manager.h

// Lifecycle
void channel_manager_init(void);
void channel_manager_shutdown(void);

// Lookup
channel_t *channel_manager_get(const char *id);
size_t     channel_manager_count(void);
channel_t *channel_manager_at(size_t idx);  // for iteration

// Mutation
channel_t *channel_manager_activate(const char *canonical_id);
channel_t *channel_manager_create_custom(const char *id, ...);
bool       channel_manager_set_field(const char *id, const char *field, const widget_field_value_t *val);
bool       channel_manager_delete(const char *id);

// Persistence
esp_err_t channel_manager_load_from_lfs(void);
esp_err_t channel_manager_save_to_lfs(void);
void      channel_manager_mark_dirty(void);  // queues debounced save

// Listeners
void channel_manager_subscribe(channel_t *c, channel_changed_cb cb, void *user);
void channel_manager_unsubscribe(channel_t *c, channel_changed_cb cb, void *user);

// Signal pipeline
void channel_manager_dispatch_signal(uint16_t signal_index, float value, bool stale);
```

### 1.4 JSON file format

`/lfs/channels.json`:

```json
{
  "schema_version": 1,
  "default_unit_system": "metric",
  "channels": [
    {
      "id": "rpm",
      "signal": "RPM",
      "max": 8000,
      "high_warn": 6800,
      "high_critical": 7200
    },
    {
      "id": "coolant_temp",
      "signal": "WaterTemp",
      "low_warn": 70,
      "high_warn": 105,
      "high_critical": 115
    }
  ]
}
```

**Defaults-only emit**: only fields that differ from the canonical
default get serialized. Keeps JSON small (~3–5 KB typical) and the
diff with future canonical defaults is visible.

### 1.5 Layout JSON (v14)

```json
{
  "schema_version": 14,
  "name": "Race Pack Pro",
  "screens": [
    {
      "widgets": [
        {
          "type": "meter",
          "channel": "rpm",
          "x": 0, "y": 0, "w": 200, "h": 200,
          "config": {
            "needle_color": 16777215,
            "needle_tip_style": 2,
            "shadow_enabled": true
          }
        }
      ]
    }
  ],
  "embedded_custom_channels": [
    {
      "id": "custom_meth_dc",
      "label": "Meth Inj Duty",
      "group": "engine",
      "units_native": "%",
      "high_warn": 85
    }
  ]
}
```

The widget's `config` block holds ONLY visual style. `min`, `max`,
`signal_name`, threshold values are GONE — they come from the bound
channel.

Backwards compat: if `channel` is missing AND old fields are present,
the v13 migrator handles it on load.

---

## 2. Storage Architecture

```
/lfs/
├── channels.json          # User's channel config (v1+ persistent)
├── settings.json          # Global dash settings (unit system, theme)
├── layouts/
│   ├── default.json       # Firmware default mirror; regenerable
│   ├── <user-saved>.json
├── templates/             # Layout templates downloaded or shipped
│   └── race_pack.json     # Loaded read-only at template-pick time
├── images/                # existing
├── fonts/                 # existing
└── logs/                  # existing

NVS (key/value):
- wifi_*                   # WiFi credentials (existing)
- ecu_preset               # Currently selected ECU preset
- active_layout            # Currently active layout filename
- first_run_done           # bool — has wizard completed
- peak_persistence_*       # existing
- ch_unit_system           # global metric/imperial toggle
```

Firmware-baked (read-only, compile-time):
- Canonical channel registry (~9 KB, generated from markdown)
- Default layout (`default_layout.c` — current path)
- ECU signal+channel presets (`ecu_presets.c` extended to seed channels)
- Layout templates (5–8 ship in firmware as `const` data)

---

## 3. Studio UI Architecture

### 3.1 Three modes

Top bar always visible:

```
☰  RDM-7 Studio    [SETUP] [DESIGN] [LIVE]      ⓘ Connected ⚙
```

**Setup** — car-level configuration
- Car / ECU
- Channels  ← new big addition
- Signals (CAN config — advanced)
- WiFi
- Device

**Design** — layout authoring
- Layouts (your saved layouts)
- Templates (firmware + marketplace)
- Widgets (canvas + inspector)
- Themes (Phase 4)

**Live** — runtime observation
- Dashboard (live mirror)
- Peaks
- Logger / Replay
- Diagnostics

### 3.2 Channels editor (Setup → Channels)

Two-pane layout:

- **Left**: channels list grouped by `channel_group_t`. Tier filter +
  search at top. Configured channels show `●` (live status: green/yellow/red).
  Unconfigured show `○` — click to activate.
- **Right**: detail panel for selected channel.
  - Live value + staleness at top (immediate feedback)
  - Identity (id, label, group)
  - Source (signal dropdown)
  - Units & format (native / display / decimals)
  - Display range (min, max) + sanity bounds
  - Thresholds — 4 inputs with color pickers per zone
  - Live preview bar showing zones with current value pointer
  - "Used in N widgets" footer with deep-link
  - Reset to canonical defaults / Delete (custom only)

Debounced auto-save: 500ms after last edit → `channel_manager_save_to_lfs()`.

### 3.3 Widget inspector (Design → selected widget)

Inspector groups fields by source:

- **Channel** section: dropdown for binding, "Edit Channel" deep-link.
- **Data (from channel)** section: read-only display of min, max,
  thresholds. "Customize" link per field → flips to widget-local
  override.
- **Visual** section: per-widget style (needle, font, shadow, etc.).
- **Position & Layout** section.
- **Rules / Night Mode** sections (Pro mode only).

### 3.4 First-boot wizard

Triggered when NVS `first_run_done` is false. Full-screen overlay.

Six steps:
1. Welcome + unit system (metric/imperial)
2. ECU picker — loads signal + channel presets for selection
3. Channels checklist — pre-checked common channels for that ECU
4. Tune key thresholds — one-screen-per-critical-channel slider review
5. Layout template gallery — pick + preview
6. Done — sets `first_run_done = true`, drops user into Design mode

Skippable per-step. Wizard re-runnable from Setup → "Re-run setup".

---

## 4. Layout Sharing & Import

### 4.1 Auto-create canonical channels

When a layout references a canonical channel the user hasn't activated:

1. Firmware detects unknown channel ID on load
2. Looks up canonical defaults from registry
3. Creates the channel in `/lfs/channels.json` with canonical defaults
4. Signal source field empty — widget renders "—" until user assigns
5. UI shows post-import banner: "3 channels need a signal source: EGT,
   AFR, Lap Time. [Configure]" with quick-config jump

No similarity-matching algorithm. No substitution suggestions. Cleaner
mental model: canonical channels are identities, they either have a
signal or they don't.

### 4.2 Custom channels in layouts

Layout file carries `embedded_custom_channels[]` block with full
channel definitions for any custom channels it uses.

On import:
- For each custom channel, brief modal:
  - "Recreate on my dash" (creates in `/lfs/channels.json`, prompts
    signal source)
  - "Map to existing channel" (pick from current configured channels)
  - "Hide widget" (skip)

### 4.3 Marketplace metadata

Layout listing auto-derived from layout file:
- Required canonical channels (manifest auto-generated)
- Optional widgets (per-widget `priority` field: required/optional/hidden)
- Custom channels by author (shown separately)
- Compatibility score for the viewing user's dash

User-facing filters:
- "100% compatible" / "Mostly compatible" / "Style"
- "Channels I have"

### 4.4 Quick-map for power-user oddities

Channels tab has a "Map signal to multiple channels" tool. For the
1-EGT-probe-but-layout-wants-8-EGTs case: pick one signal, multi-select
target channels, apply. All 8 widgets show the same value. Explicit,
not algorithmic.

---

## 5. Migration (v13 → v14)

Triggered on layout load when `schema_version < 14`.

Algorithm:
1. Parse v13 layout.
2. Walk widgets. For each widget with a `signal_name`:
   - Look up canonical channel that uses that signal name in the user's
     `/lfs/channels.json`.
   - If found → rewrite widget to use `channel: "..."` with the
     canonical ID; drop per-widget `min/max/threshold` (they're
     channel-derived now).
   - If not found → check canonical registry by signal-name hint
     (some signal names map to canonical IDs by convention — RPM,
     CoolantTemp, etc.). Auto-create canonical channel from defaults.
   - If still not found → keep widget signal binding as legacy; widget
     still works via fallback path.
3. Write back as v14 layout.
4. Show "Layout upgraded — review Channels tab" toast on first Studio
   load.

Backup: pre-migration version stored at `/lfs/layouts/<name>.v13.bak`
for one firmware release cycle.

---

## 6. Implementation Phases

### Phase 1: Foundation (3–4 days agent + 2 days verify)

- [ ] `main/data/canonical_channels.c/h` — registry generated from markdown
- [ ] `main/data/channel_manager.c/h` — module + lifecycle
- [ ] `/lfs/channels.json` I/O (load + debounced save)
- [ ] Generic OBD2 default channel set for blank-dash boot
- [ ] First widget bound: `widget_meter` — channel_id, channel snapshot
  cache, channel listener subscription, apply on event
- [ ] Schema bumped to v14 in `layout_manager.h`
- [ ] Hand-crafted v14 test layout for smoke test
- [ ] Unit tests for channel_manager (PC native build)

**Verify**: hand-edit a v14 layout that uses `rpm` and a meter widget.
Boot dash. Confirm meter renders with channel-derived range, responds
to live signal updates, shows correct thresholds.

### Phase 2: Widget rollout + Channels editor (4–5 days)

- [ ] Wire remaining widgets to channels:
  - widget_bar, widget_rpm_bar, widget_arc, widget_panel, widget_text,
    widget_indicator, widget_warning
- [ ] Per-widget per-field override mechanism (min_override etc.)
- [ ] Web UI: Setup mode + Channels tab CRUD interface
- [ ] Web UI: widget inspector "Channel" section + "from channel" badges
- [ ] Live channel-value broadcast to Studio (polling at 5 Hz)
- [ ] v13 → v14 migration on layout load

**Verify**: existing real layouts auto-migrate cleanly. Edit a channel,
all bound widgets across all screens update live. No FPS regression.

### Phase 3: Studio modes + First-boot wizard (3–4 days)

- [ ] Studio top-bar 3-mode switcher
- [ ] First-boot wizard rewrite (6 steps)
- [ ] ECU preset extension to seed channels
- [ ] Layout template gallery (firmware ships 5–8 templates)
- [ ] Default layout uses Tier 1 canonical channels

**Verify**: brand-new dash boot → wizard appears → 5-minute setup
completes → useful dashboard running.

### Phase 4: Layout sharing + Marketplace plumbing (2–3 days)

- [ ] Auto-create canonical channels on layout load
- [ ] Embedded custom channel handling on import
- [ ] Quick-map tool (signal → multiple channels)
- [ ] Marketplace metadata auto-derivation from layouts
- [ ] Layout listing UI shows compatibility + required channels
- [ ] Post-import banner / configure prompt

**Verify**: a Link-authored layout downloaded on a Haltech dash
auto-binds usable channels, prompts only for missing signal sources.

### Phase 5: Polish + cleanup (2–3 days)

- [ ] Beginner / Pro mode toggle
- [ ] Inspector field filtering by mode
- [ ] Performance verification: synthetic CAN harness running full
  layouts, FPS comparison vs pre-channel build
- [ ] Stale code removal pass (track in `docs/v2_cleanup_log.md`)
- [ ] Documentation updates: `handover/03-widget-system.md` updated to
  reflect channel binding model
- [ ] Update CLAUDE.md and MEMORY.md

**Verify**: target FPS within 1–2 of pre-channel baseline. No legacy
code paths reachable.

---

## 7. Cleanup Tracking

As each piece lands, we remove what becomes stale. Logged in
`docs/v2_cleanup_log.md`. Initial expected removals:

- Per-widget redundant min/max defaults in factory functions
- Inspector inheritance-aware UI replaces flat property dumps
- Some widget rules become channel events (less code in widget_rules.c)
- ECU-specific signal naming on widgets disappears
- Legacy signal-name fallback path stays for one release, then deleted

Performance opportunities to track:
- Shared threshold evaluation across widgets bound to same channel
- Channel staleness as a single check vs per-widget signal lookups
- JSON budget freed by collapsing per-widget min/max/threshold (was
  eating non-trivial space on widget-heavy layouts)

---

## 8. Open Decisions Locked

Decisions made during planning. Locked unless evidence forces revisit.

| Topic | Decision |
|---|---|
| Channel location | Dash-level (`/lfs/channels.json`), not in layouts |
| Layout references | By canonical string ID |
| Auto-create on import | Yes, with canonical defaults |
| Similarity algorithm | None — drop in favor of explicit signal-source assignment |
| Custom channels | Embedded in layout file, opt-in recreate on import |
| Per-widget override | Yes, escape hatch for one-off custom ranges |
| Slot-based mode | Phase 4/5 — layered on top, not the primary model |
| Migration | Auto on load, with backup file kept one release |
| Wizard re-runnable | Yes, from Setup → "Re-run setup" |
| Default new-dash channel set | Generic OBD2 baked in firmware |
| Channel event vs widget rule | Channel events replace common widget rules; per-widget rules remain for niche cases |

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|---|---|
| Migration breaks customer layouts | Backup `.v13.bak` retained one release; auto-migrator tested against real customer layouts before rollout |
| Web UI complexity creep | Three-mode architecture forces clean separation; resist temptation to mix modes |
| Channel concept confuses users | Beginner mode hides Channels tab in Setup; defaults work without ever opening it |
| Performance regression | Phase 5 includes synthetic CAN harness for headless verification; FPS gate before ship |
| ECU preset accuracy | Each preset needs domain expertise — user to provide real-world reference values or sample CAN logs |
| Marketplace economy fails to grow | Architecture supports it; growth is a separate concern |

---

## 10. Working Agreements During Implementation

- Each phase ends with a verified milestone before next phase starts.
- Backwards compat for one release cycle minimum.
- All new C files include doc-comment headers explaining intent.
- ADR updated when locked decisions get revisited.
- Cleanup log updated with each removal so we can answer "what changed".
- MEMORY.md updated at phase boundaries with progress.
- CLAUDE.md updated when architecture details change (signal vs channel
  flow, storage layout, build steps).

---

*This document evolves as the implementation lands. PR descriptions cite
section numbers here so reviewers can map code to plan.*
