# Documentation map

Reading the codebase cold? Open the docs by the **question you have**, not by what they're called. This map is the index — every other doc is purposeful.

## "I'm new — where do I start?"

1. [`/README.md`](../README.md) — the marketing pitch + the build command.
2. [`docs/handover/README.md`](handover/README.md) — handover index. From there, read chapters 01–10 in order. Budget ~90 minutes; you'll come out understanding the whole firmware.
3. [`CLAUDE.md`](../CLAUDE.md) at the repo root — terse architectural reference. Skim it, then keep it in a tab.

## "I'm looking for the answer to a specific question"

| Question | Open |
|---|---|
| How does a CAN frame become a widget update? | [`docs/handover/04-signal-and-can.md`](handover/04-signal-and-can.md) |
| How do I add a new widget type? | [`docs/handover/03-widget-system.md`](handover/03-widget-system.md) + the existing widget files in `main/widgets/` |
| What's the LVGL mutex / threading rule? | [`CLAUDE.md`](../CLAUDE.md) § "Threading" |
| What does the JSON layout look like? | [`docs/handover/03-widget-system.md`](handover/03-widget-system.md) + a saved `.json` in `data/layouts/` |
| Why are there three HTML copies? | [`docs/adr/0005-html-source-of-truth.md`](adr/0005-html-source-of-truth.md) |
| Why does Wi-Fi onboarding have so much code? | [`docs/adr/0001-wifi-onboarding-reliability.md`](adr/0001-wifi-onboarding-reliability.md) |
| How is the web server file structured? | [`docs/adr/0002-web-server-split-roadmap.md`](adr/0002-web-server-split-roadmap.md) |
| What are the performance budgets? | [`docs/adr/0004-performance-budgets.md`](adr/0004-performance-budgets.md) |
| What needs to happen before customer release? | [`SECURITY.md`](../SECURITY.md) § "Pre-release hardening checklist" |
| What landed in the last release? | [`CHANGELOG.md`](../CHANGELOG.md) |
| How do I run the test suite? | [`tests/README.md`](../tests/README.md) |
| What's the WASM offline preview pipeline? | [`docs/WASM_PREVIEW_BUILD_GUIDE.md`](WASM_PREVIEW_BUILD_GUIDE.md) |

## The four doc families

```
RDM-7_Dash/
├── README.md            user-facing pitch + dev quickstart
├── CLAUDE.md            terse architectural reference (Claude + experienced devs)
├── CHANGELOG.md         what changed and when
├── SECURITY.md          security posture + pre-release checklist
├── RDM-7_User_Guide.md  end-user setup guide (for someone who bought a dash)
└── docs/
    ├── README.md        ← you are here
    ├── WASM_PREVIEW_BUILD_GUIDE.md   one-off: WASM offline preview build
    ├── handover/        deep developer onboarding (10 chapters)
    │   ├── README.md
    │   ├── 01-architecture.md
    │   ├── 02-build-and-flash.md
    │   ├── 03-widget-system.md
    │   ├── 04-signal-and-can.md
    │   ├── 05-storage-and-persistence.md
    │   ├── 06-ui-and-screens.md
    │   ├── 07-web-server-api.md
    │   ├── 08-aux-systems.md
    │   ├── 09-conventions-and-pitfalls.md
    │   └── 10-module-reference.md
    └── adr/             architecture decisions (immutable, sequentially numbered)
        ├── README.md
        ├── 0001-wifi-onboarding-reliability.md
        ├── 0002-web-server-split-roadmap.md
        ├── 0003-desktop-index-sync-plan.md
        ├── 0004-performance-budgets.md
        └── 0005-html-source-of-truth.md
```

### Who each family is for

| Family | Reader | When updated |
|---|---|---|
| `README.md` | A first-time visitor | When the project pitch or build commands change. Rarely. |
| `CLAUDE.md` | An experienced dev (or Claude) who needs a refresher | Every architectural shift. ~Weekly. Targets ~180 lines — if it grows past 250 something belongs in handover. |
| `RDM-7_User_Guide.md` | A user who bought a dashboard | When user-visible features change. Per release. |
| `docs/handover/` | A new contributor reading the codebase cold | Schema and module changes. Per major feature. |
| `docs/adr/` | Anyone about to change an architectural area | Per significant decision. New ADRs are append-only. |
| `CHANGELOG.md` | Anyone deciding whether to upgrade | Per merged change worth a note. |
| `SECURITY.md` | Anyone preparing a release or auditing | Per security-relevant change. |
| `tests/README.md` | Anyone adding a test | Per new test file or test pattern. |

### What's *not* in any of these

- **Per-module reference docs** — those live as header-comment blocks in `main/*/*.h`. Doxygen would generate browsable HTML from them; we don't ship a generator today.
- **Per-API endpoint specs** — `docs/handover/07-web-server-api.md` is the closest. The Python tests in `tests/api/` double as executable spec.
- **JSON schemas** — `schema/widgets.schema.json` is the canonical layout schema. It's read by the codegen pipeline, not by humans directly.
- **Session memory** — `.claude/projects/.../memory/MEMORY.md` is Claude's session notes. It's not authoritative — anything important should graduate into one of the above before the session ends.

## Adding a doc — which family?

- **Are you answering "what does this code do?"** → header comment in the `.h`. Don't create a doc.
- **Are you answering "how is this thing built and why?"** → `docs/handover/`, in the relevant numbered chapter. If it doesn't fit any chapter, that's a signal — maybe it deserves a chapter, maybe it's an ADR.
- **Are you recording a decision you'll be questioned about later?** → new ADR in `docs/adr/`. Pick the next sequential number. Don't edit accepted ADRs — supersede them with a new one.
- **Are you writing a one-shot build / setup guide?** → top-level `docs/SOMETHING-GUIDE.md`. Keep it standalone.
- **Are you announcing a change to people who use the firmware?** → `CHANGELOG.md` bullet.
- **Are you flagging something a release needs to handle?** → `SECURITY.md` checklist item or a release-blocker note in `CHANGELOG.md`'s `[Unreleased]` section.
- **Are you writing for the end user (someone who bought a dash)?** → `RDM-7_User_Guide.md`.

## Maintenance signals — when docs are going stale

Watch for these. They mean a doc-refresh pass would pay off:

- A CLAUDE.md section talks about "current count is N" or a recent commit — these are honest snapshots, not specs. They drift. Refresh them when you spot one that's >3 months out.
- A handover chapter mentions a deleted file or a renamed function.
- An ADR says "Proposed" for >6 months. Either accept it, supersede it, or delete it.
- `CHANGELOG.md`'s `[Unreleased]` section is empty but `git log master ^last-tag` shows real changes. The version is overdue for a cut.
- `tests/README.md` lists a test file that doesn't exist, or omits one that does. Pattern: stale.

This file (`docs/README.md`) is the index and should never go stale — if you add a new doc family, add a row here and a new tree entry above.
