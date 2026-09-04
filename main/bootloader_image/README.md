# Embedded bootloader image

`bootloader.bin` here is the 2nd-stage bootloader the dash will write to itself
via `POST /api/bootloader/update` (see `main/storage/bootloader_selfupdate.c`).

It is **committed deliberately**, not generated at build time. Embedding
`build/bootloader/bootloader.bin` directly would mean shipping whatever the
current build happened to produce; this way the bootloader a dash flashes into
itself is an explicit artifact somebody chose and reviewed.

## Updating it

    cp build/bootloader/bootloader.bin main/bootloader_image/bootloader.bin

Then check the header still reads DIO / 16 MB / 80 m before committing:

    python -c "d=open('main/bootloader_image/bootloader.bin','rb').read(); \
               print(hex(d[0]), d[2], hex(d[3]))"

Expect `0xe9 2 0x4f`. Those header bytes are what esptool normally patches at
flash time; nothing patches them on the self-update path, so a mismatch here
produces a dash that will not boot until someone reflashes it over USB.

## The feature stamp

The bootloader carries `RDM-BL-FEAT:NNNN` (see
`bootloader_components/rdm_can_park/rdm_can_park.c`). The app reads that string
back out of flash and compares it with the one in the image here; the amber
"finish the update" banner appears when flash has a lower number, and nothing
else makes it appear.

That is on purpose. A byte comparison would fire on any rebuild — a changed
console pin, a toolchain bump — and this write bricks the dash if power drops
mid-way, so it must only ever be offered when the bootloader is genuinely
missing something.

So when you promote a bootloader here:

* **Behaviour changed** (something dashes in cars actually need): bump the
  version in `rdm_can_park.c` first, rebuild, then copy. Every dash on a lower
  number will offer the update.
* **Nothing behavioural changed**: leave the version alone. The bytes will
  differ and no one will be prompted, which is the intended outcome.

A bootloader with no stamp at all reads as version 0, so any dash still running
one from before the stamp existed will be offered the update once. Verify it is
there before committing:

    python -c "print(b'RDM-BL-FEAT:' in open('main/bootloader_image/bootloader.bin','rb').read())"

It must print `True`. The stamp is kept through `--gc-sections` by
`__attribute__((used, retain))` plus a reference in `bootloader_hooks_include()`
— the first attempt used `used` alone and the linker collected it, producing a
bootloader that silently never advertised itself.
