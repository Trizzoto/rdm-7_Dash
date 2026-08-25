# ADR-0047: The bus must be quiet before the dash is ready

Date: 2026-08-25
Status: Accepted
Repos: RDM-7_Dash (firmware + 2nd-stage bootloader)
Evidence: boot capture from `RDM-DCB4-D926` over COM40, 2026-08-25 (before/after).

## Context

A customer's Link G4 stopped transmitting whenever the car and the dash powered
up together. Plugging the dash in *after* the engine was running worked; an
inline switch on the dash's power feed was being used as a workaround. The
symptom read as an ECU fault, and the standing suspicion in this repo was the
`rdm_bus` 0x4FF discovery announce storming a bus with nobody to ACK it.

It is not that. It is earlier and simpler.

TWAI TX is **GPIO20, which on the ESP32-S3 is also USB D+**. Out of reset the
USB Serial/JTAG PHY owns that pad, and it leaves it with nothing driving it.
Measured at the top of `app_main`, before any of our code touched the pin:

```
usb_pad_en=1  dp_pullup=0  iomux20 func=0 IE=0 PU=0 PD=0  out_en=0  level=0
```

No pull-up (the S3's default D+ pull-up, which would have held the line
recessive, is off), no pull-down, no output driver. The pin floats. A floating
CMOS input at the transceiver's TXD reads as **0**, and TXD low means *drive
the bus dominant*. So the transceiver holds the entire bus dominant for the
whole of startup. Nothing else can transmit through that. An ECU that powers up
inside that window sees a jammed bus, errors out, and gives up — while one that
is already healthy rides it out and recovers, which is exactly the boot-order
asymmetry the customer found.

The pad is only claimed when `twai_driver_install()` runs, and that is late:

| phase | ends at |
|---|---|
| ROM + 2nd-stage bootloader start | ~114 ms |
| app image map (1.46 MB + 1.59 MB segments) | ~723 ms |
| PSRAM init + XIP copy of rodata and instructions | ~1076 ms |
| PSRAM memory test | ~1264 ms |
| `app_main` reached | ~1396 ms |
| `twai_driver_install()` claims GPIO20 | **~1540 ms** |

Roughly **1.5 seconds of jammed bus, every single boot.** None of the phases
above are meaningfully shrinkable — they are image mapping and PSRAM bring-up.

## Decision

**Park the CAN transmit line recessive before anything else runs, and do it in
the second-stage bootloader.**

`bootloader_components/rdm_can_park` hooks `bootloader_before_init()`: it
clears `USB_SERIAL_JTAG_USB_PAD_ENABLE` to take the pad off the USB PHY, sets
the IO-mux pull-up, selects plain GPIO function, writes the output level high
and *then* enables the output driver (that order matters — enabling the driver
first would pulse the line dominant, which is the thing we are here to avoid).
This lands at ~25 ms.

Three supporting decisions:

- **`can_park_tx_line()` repeats the park as the first statement of
  `app_main`.** It costs microseconds and it covers an app OTA'd onto an older
  bootloader, which is the common case in the field (see Consequences).
- **`CONFIG_ESP_CONSOLE_SECONDARY_NONE`.** The USB Serial/JTAG console owns
  GPIO19/20 — the CAN pins. The primary console is UART0 via the CH343, so
  nothing is lost by stopping the PHY from ever re-claiming them.
- **`can_init()` stays where it is**, after the channel registry loads. The bus
  only needs to be *quiet* early, not *live* early. Moving TWAI init earlier
  would build the acceptance filter before the channels exist, trading a real
  bug for a cosmetic one.

## Consequences

**The bootloader is not updated by OTA.** `esp_ota_*` writes the app partition
only. An existing dash that takes this release over the air gets the
`app_main` park — the line goes quiet at ~1.40 s instead of ~1.54 s, which is
not the fix. **Getting the real fix onto a dash in the field requires a USB
reflash.** Any customer presenting the Link boot-order symptom needs the cable,
not a fleet OTA.

**The permanent fix is a resistor.** Transceiver datasheets generally require
an external pull-up on TXD precisely because MCU pins float through reset. This
firmware change is a mitigation that closes the window to ~25 ms; a pull-up at
the transceiver closes it to zero, needs no bootloader, and cannot be undone by
a future refactor of the boot path. It belongs on the next board revision, and
until it exists this ADR is what stops someone "tidying up" the hook.

**GPIO19/20 are now spoken for in two ways.** Anything that wants USB
Serial/JTAG back — a native-USB console, USB CDC — takes the CAN bus down with
it. `dev-console-keep-flag` already notes the UART side of this; this is the
other half.

**The rdm_bus announce storm is still real, just not this.** Commit `2708802`
backed the announce off on a bus that has never received a frame. That remains
worth having; it was simply never the cause of the Link fault.
