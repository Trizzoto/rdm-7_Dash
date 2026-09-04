# ADR-0060: A keypad is a device, not a model

Date: 2026-09-02
Status: Accepted — built 2026-09-02
Repos: rdm7-desktop (keypad workspace in `src/tauri-overlay.html`,
`tools/check_keypad.js`)
Follows: ADR-0055 (a device on the wrong bit rate is invisible, not broken),
ADR-0059 (the last frame is the one that stays)

## Context

Studio's keypad workspace stored its configuration in `rdm7_keypads_v1` — the
name plural, the shape singular. Inside, entries were keyed by **model id**:

```
{ "pkp2200": { keys: [...], node: 0x15, show: {...} },
  "pkp3500": { keys: [...], node: 0x15, show: {...} } }
```

So Studio could remember a 2200 *and* a 3500, and could not remember two 2200s.
Which is backwards. Nobody fits one of each; plenty of builds fit two of the
same — a four-key on the wheel and a fifteen-key on the console is the
canonical rally layout, and both are PKP-2xxx.

Worse, model was doing double duty as identity. Switching the model dropdown
silently loaded a different configuration, because the dropdown was the
primary key. There was one global `kp.node`, so both imaginary keypads claimed
node `0x15`, which on a real bus is not a conflict Studio warns about — it is
two devices answering the same SDO and a bench session that makes no sense.

## Decision

**Identity is a keypad, not a model. Storage is a list, the workspace has a
picker, and the address is checked because it is the one field that cannot be
wrong twice.**

```
{ v: 2,
  sel: "kp_a1b2c3",
  list: [ { id, name, model, node, sa, proto, baud, keys, show, ... } ] }
```

`model` becomes an ordinary field of a keypad — change it and you are changing
*this* keypad's part number, not switching to a different saved config. Adding
a keypad is a button in the black bar next to the section tabs; every section
then acts on the selected one, and the section tabs never move, so the shape
of the workspace does not change as keypads are added.

### Migration is a promotion, not a merge

The old store's model-keyed entries become the first keypads in the list, named
after their model, in the order the models are declared. A user with one saved
2200 gets one keypad called "PKP-2200" and notices nothing. A user with two
model entries gets two keypads and both survive. The old key is left in place
rather than deleted, so a downgrade still finds its data — cheap insurance
against a bad release, and the store is a few kilobytes.

### Addresses are allocated, not typed

Two keypads on one node ID is the failure that costs a bench afternoon,
because it presents as neither device working correctly and both answering.
So:

- a new keypad gets the **next free** node ID (and J1939 source address), not
  the factory default,
- an address already used by another keypad in the list is refused in the
  field with a plain sentence naming the other keypad,
- the setup wizard, which finds a keypad by whoever answers at the factory
  address, now says **connect them one at a time** before it starts, because
  two factory-fresh keypads on one bus are genuinely indistinguishable — this
  is the same "invisible, not broken" problem as ADR-0055 and has the same
  answer: say it before the bench does.

### Each keypad owns its own show

The alternative — one show spanning several keypads, a wipe travelling across
both panels — was rejected. The frame model is per-node (`0x200 + node`), the
panels are physically separated in the car by a driver, and the feature costs
a synchronisation problem across two independently rate-limited streams to buy
an effect nobody asked for. Instead, **copy to…** moves a finished show from
one keypad to another in one click, which is the actual want ("make the wheel
one match the console one") at none of the cost.

## Consequences

- `kp` stays a single live object — every render path reads it, and rewriting
  those to take a keypad argument would be a much larger change for no user
  benefit. Selecting a keypad loads it into `kp`; saving writes it back to its
  entry in the list. The list is the truth, `kp` is the cursor.
- Exports that describe a device (setup file, SavvyCAN CSV, DBC) describe the
  selected keypad and say which one in the filename. A single file describing
  every keypad was rejected for the same reason the wizard programs one at a
  time: the file is loaded against one device on one address.
- The home card and the workspace header show the keypad count, so "I have two"
  is visible from outside the section rather than discovered inside it.
- Deleting the last keypad is not offered. An empty workspace has nothing to
  render and no useful state; the floor is one.

## References
- Code: `rdm7-desktop/src/tauri-overlay.html` — `KP_LS`, `kpStore`,
  `kpLoadCfg` / `kpSaveCfg`, `kpAddKeypad`, `kpPickKeypad`, `kpRenderBar`
- Tests: `rdm7-desktop/tools/check_keypad.js`
- Related ADRs: 0055, 0059
