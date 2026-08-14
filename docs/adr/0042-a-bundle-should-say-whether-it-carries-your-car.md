# ADR-0042: A bundle should say whether it carries your car

Date: 2026-08-14
Status: Accepted
Repos: RDM-7_Dash (`main/web/index.html`), rdm7-desktop (resynced base)
Supersedes the sharing half of the 2026-08-08 decision to put `channels.json`
inside `.rdm`. Prompted by the owner: *"the RDM file where you export your
whole dashboard including the channels, isn't that what we used to name the
layouts? so what are we doing there"*.

## The conflation

`.rdm` began as the layout format. In August it gained entry type 3,
`channels.json`, so a bundle saved at the car could carry the CAN decode and
thresholds for offline editing. Good feature, and the container was built for
it — the reader skips unknown types, so old importers cope.

But one verb now produced two different things:

- **a layout** — widgets, images, fonts; portable, says nothing about a car
- **a full dashboard** — the above plus the CAN ids, bit offsets, scale
  factors, thresholds and units this particular dash is running

Both were called `Name.rdm`. You could not tell which you had until you
imported it and were asked to replace your channel setup and reboot. Two
files, same extension, wildly different blast radius.

That matters most where the files travel. Nothing stopped a bundle
containing someone's whole bus configuration being published to the
marketplace, and nothing told the person downloading it.

## Decision

**One container, two flavours, and the file says which.**

Byte 8 of the header — previously part of the reserved run — is a flavour
field: 0 unstated, 1 layout, 2 dashboard. The export writes it, the importer
reads it and states what arrived before touching anything.

Filenames carry the same fact so the answer survives outside the app:
`Fuel_Cluster.layout.rdm`, `Fuel_Cluster.dashboard.rdm`. Both still end in
`.rdm`, so file pickers, drag-and-drop and the `accept` filter are unchanged.

`Save layout` genuinely omits the channels entry — it does not fetch the
registry at all. That is the safety property: a layout bundle cannot leak a
bus configuration because it never contains one, rather than because a
sanitiser remembered to strip it.

Channels keep their own file. `channels_<serial>.json` already existed, and
JSON is right for it: small, readable, diffable, and the thing you hand
someone debugging a decode. Wrapping it in a binary container to make it a
third `.rdm` flavour would be a downgrade.

## Why not three file types

The obvious alternative — `.rdm` layout, something else for a dashboard,
something else again for channels — was considered and rejected. It means
three parsers and three validators across firmware, the web editor, the
desktop app and the marketplace, to express a distinction the container
already carries in a type byte. And a user holding a mystery file is worse
off than one reading `.layout.rdm` in their downloads folder.

The problem was never that one format described two things. It was that the
file didn't admit it.

## Consequences

- Every existing `.rdm` still opens. Flavour 0 means unstated, and those are
  classified the way readers already did — by whether a type-3 entry is
  actually present.
- `exportRdm()` with no argument still writes a full dashboard, which is what
  the single old button did; the desktop command binding calls it that way.
- Anything published to the marketplace should be the layout flavour. That is
  now something a person can see rather than something they must be told.
- Import/export also lands in the Design sidebar. It was reachable from the
  app menu and the Setup page, neither of which is where you are when you
  want it.

## Verification

Exported both flavours from the live dash and read the bytes back:

| | flavour byte | entries | channels | size |
|---|---|---|---|---|
| `default.layout.rdm` | 1 | 2 (layout, image) | no | 23,260 B |
| `default.dashboard.rdm` | 2 | 3 (layout, image, channels.json) | yes | 26,547 B |

The 3,287-byte difference is the channel registry, absent by construction
from the layout flavour rather than removed after the fact.
