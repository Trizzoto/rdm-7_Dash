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
