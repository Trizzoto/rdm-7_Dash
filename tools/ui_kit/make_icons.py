#!/usr/bin/env python3
"""Bake the UI kit's line icons into LVGL alpha images.

    python tools/ui_kit/make_icons.py

Writes main/ui/kit/uk_icons.c and uk_icons.h. Each icon is drawn from the
24-unit SVG path below (the same set the menu mockups were drawn with) at
every size in SIZES, supersampled 8x and box-filtered down so a 1.8-unit
stroke keeps its real coverage instead of snapping to whole pixels.

The images are LV_IMG_CF_ALPHA_4BIT: shape only, no colour. LVGL paints them
with the object's img_recolor, so one icon serves every palette — a light
theme is a different recolor, not a second set of pictures.

Rasterises with headless Microsoft Edge (every icon of a size in one page,
one screenshot) and needs Pillow. Re-run after editing ICONS.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

EDGE = os.environ.get('EDGE', r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe')

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT_DIR = os.path.join(ROOT, 'main', 'ui', 'kit')
SIZES = (22, 32)
SS = 8          # supersample factor
STROKE = 1.8    # in 24-unit icon space

# name -> SVG body in a 24x24 box. Strokes use currentColor; a fill="currentColor"
# element is a solid dot. Keep names short: they become UK_ICON_<NAME>.
ICONS = {
    'layouts':  '<rect x="3" y="3" width="7.5" height="7.5" rx="1.5"/><rect x="13.5" y="3" width="7.5" height="7.5" rx="1.5"/><rect x="3" y="13.5" width="7.5" height="7.5" rx="1.5"/><rect x="13.5" y="13.5" width="7.5" height="7.5" rx="1.5"/>',
    'sun':      '<circle cx="12" cy="12" r="4"/><path d="M12 2.5v2.2M12 19.3v2.2M2.5 12h2.2M19.3 12h2.2M5.3 5.3l1.6 1.6M17.1 17.1l1.6 1.6M5.3 18.7l1.6-1.6M17.1 6.9l1.6-1.6"/>',
    'moon':     '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>',
    'rec':      '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="3.6" fill="currentColor" stroke="none"/>',
    'gear':     '<circle cx="12" cy="12" r="3.2"/><path d="M12 2.8l1.6 2.3 2.7-.6.8 2.6 2.6.8-.6 2.7 2.3 1.6-2.3 1.6.6 2.7-2.6.8-.8 2.6-2.7-.6L12 21.2l-1.6-2.3-2.7.6-.8-2.6-2.6-.8.6-2.7L2.6 12l2.3-1.6-.6-2.7 2.6-.8.8-2.6 2.7.6z"/>',
    'car':      '<path d="M4 15.5l1.6-5a2 2 0 0 1 1.9-1.4h9a2 2 0 0 1 1.9 1.4l1.6 5"/><rect x="2.5" y="15" width="19" height="4.5" rx="1.5"/><circle cx="7" cy="17.3" r=".9" fill="currentColor"/><circle cx="17" cy="17.3" r=".9" fill="currentColor"/>',
    'can':      '<circle cx="5" cy="12" r="2.2"/><circle cx="19" cy="6" r="2.2"/><circle cx="19" cy="18" r="2.2"/><path d="M7.2 12h4M11.2 6v12M11.2 6h5.6M11.2 18h5.6"/>',
    'obd':      '<path d="M4 7h16l-2 10H6z"/><path d="M8 11h.01M11 11h.01M14 11h.01M17 11h.01M9.5 14h.01M12.5 14h.01M15.5 14h.01"/>',
    'wifi':     '<path d="M2.5 9a14 14 0 0 1 19 0M5.8 12.4a9 9 0 0 1 12.4 0M9.1 15.7a4.3 4.3 0 0 1 5.8 0"/><circle cx="12" cy="19" r=".9" fill="currentColor"/>',
    'chart':    '<path d="M3 20h18"/><path d="M5 16l4-5 3.5 3 6.5-8"/>',
    'play':     '<path d="M7 4.5v15l12-7.5z"/>',
    'info':     '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5h.01"/>',
    'left':     '<path d="M15 5l-7 7 7 7"/>',
    'right':    '<path d="M9 5l7 7-7 7"/>',
    'close':    '<path d="M6 6l12 12M18 6L6 18"/>',
    'check':    '<path d="M4.5 12.5l5 5 10-11"/>',
    'image':    '<rect x="3" y="4.5" width="18" height="15" rx="2"/><circle cx="8.5" cy="9.5" r="1.8"/><path d="M4 18l5.5-5.5 4 4 2.5-2.5 4.5 4.5"/>',
    'gauge':    '<path d="M4 17a8 8 0 1 1 16 0"/><path d="M12 17l4.5-5.5"/>',
    'channels': '<path d="M4 6h16M4 12h16M4 18h10"/>',
    'web':      '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
    'peak':     '<path d="M3 20l6-9 4 5 3-4 5 8"/><path d="M16 4h4v4"/>',
    'wrench':   '<path d="M14.5 5.5a4 4 0 0 0 5 5L11 19a2.1 2.1 0 0 1-3-3l8.5-8.5a4 4 0 0 0-2-2z"/>',
    'back':     '<path d="M10 6l-6 6 6 6M4 12h16"/>',
    'qr':       '<rect x="3.5" y="3.5" width="6.5" height="6.5" rx="1"/><rect x="14" y="3.5" width="6.5" height="6.5" rx="1"/><rect x="3.5" y="14" width="6.5" height="6.5" rx="1"/><path d="M14 14h3v3h-3zM20.5 14v.01M17 20.5h3.5V17"/>',
    'chip':     '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9.5 2.5V6M14.5 2.5V6M9.5 18v3.5M14.5 18v3.5M2.5 9.5H6M2.5 14.5H6M18 9.5h3.5M18 14.5h3.5"/>',
    'route':    '<circle cx="6" cy="18" r="2"/><circle cx="18" cy="6" r="2"/><path d="M8 18h7.5a3 3 0 0 0 0-6h-7a3 3 0 0 1 0-6H16"/>',
    'shifter':  '<circle cx="5" cy="5" r="1.7"/><circle cx="12" cy="5" r="1.7"/><circle cx="19" cy="5" r="1.7"/><circle cx="5" cy="19" r="1.7"/><circle cx="12" cy="19" r="1.7"/><path d="M5 6.7v10.6M12 6.7v10.6M19 6.7V12H5"/>',
    'spark':    '<path d="M11 3.5l1.9 5.1 5.1 1.9-5.1 1.9L11 17.5l-1.9-5.1L4 10.5l5.1-1.9z"/><path d="M19 15.5v5M16.5 18h5"/>',
    'reset':    '<path d="M4.5 12a7.5 7.5 0 1 0 2.2-5.3"/><path d="M4.5 3.5v4.2h4.2"/>',
    'alert':    '<path d="M12 3.8l9 15.7H3z"/><path d="M12 10v4.2M12 17h.01"/>',
    'update':   '<path d="M12 3.5v11M7.5 10l4.5 4.5 4.5-4.5M4.5 20h15"/>',
    'card':     '<path d="M7 3h8.5L19 6.5V21H7z"/><path d="M10 3v3.5M13 3v3.5M16 4.5v2"/>',
    'dim':      '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/><path d="M17 3.5v3M15.5 5h3"/>',
    'stop':     '<rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor" stroke="none"/>',
    'plus':     '<path d="M12 5v14M5 12h14"/>',
    'list':     '<path d="M9 6h11M9 12h11M9 18h11"/><circle cx="4.5" cy="6" r=".9" fill="currentColor"/><circle cx="4.5" cy="12" r=".9" fill="currentColor"/><circle cx="4.5" cy="18" r=".9" fill="currentColor"/>',
    'clock':    '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3.2 2"/>',
    'hotspot':  '<circle cx="12" cy="12" r="2"/><path d="M7.8 16.2a6 6 0 0 1 0-8.4M16.2 7.8a6 6 0 0 1 0 8.4M5 19a10 10 0 0 1 0-14M19 5a10 10 0 0 1 0 14"/>',
    'trash':    '<path d="M4.5 6.5h15M9.5 6.5V4h5v2.5M6.5 6.5l1 13.5h9l1-13.5"/>',
    'keypad':   '<rect x="2.5" y="5" width="19" height="14" rx="2.5"/><circle cx="7.5" cy="9.7" r="1.7"/><circle cx="12" cy="9.7" r="1.7"/><circle cx="16.5" cy="9.7" r="1.7"/><circle cx="7.5" cy="14.3" r="1.7"/><circle cx="12" cy="14.3" r="1.7"/><circle cx="16.5" cy="14.3" r="1.7" fill="currentColor"/>',
    'bluetooth': '<path d="M6.5 7.5l11 9-5.5 4.5v-18l5.5 4.5-11 9"/>',
}


_cache = {}


def _sheet(size):
    """Draw every icon at size*SS in one Edge screenshot; return {name: L image}."""
    if size in _cache:
        return _cache[size]
    big = size * SS
    names = list(ICONS)
    cols = 8
    rows = (len(names) + cols - 1) // cols
    cells = ''.join(
        f'<svg viewBox="0 0 24 24" width="{big}" height="{big}">{ICONS[n]}</svg>' for n in names)
    html = (f'<html><body style="margin:0;background:#000;width:{cols*big}px;line-height:0;color:#fff">'
            f'<style>svg{{display:block;float:left;fill:none;stroke:#fff;stroke-width:{STROKE};'
            'stroke-linecap:round;stroke-linejoin:round}</style>' + cells + '</body></html>')
    tmp = tempfile.mkdtemp()
    page = os.path.join(tmp, 'icons.html')
    shot = os.path.join(tmp, 'icons.png')
    with open(page, 'w') as f:
        f.write(html)
    subprocess.run([EDGE, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                    '--force-device-scale-factor=1', f'--user-data-dir={os.path.join(tmp, "prof")}',
                    f'--window-size={cols*big},{rows*big}', f'--screenshot={shot}',
                    'file:///' + page.replace(os.sep, '/')], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    img = Image.open(shot).convert('L')
    out = {}
    for i, n in enumerate(names):
        x, y = (i % cols) * big, (i // cols) * big
        out[n] = img.crop((x, y, x + big, y + big)).resize((size, size), Image.BOX)
    _cache[size] = out
    return out


def render(name, size):
    return _sheet(size)[name]


def pack_a4(img):
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        row = [(px[x, y] * 15 + 127) // 255 for x in range(w)]
        if w % 2:
            row.append(0)
        for i in range(0, len(row), 2):
            out.append((row[i] << 4) | row[i + 1])
    return bytes(out)


def main():
    names = list(ICONS)
    c = ['/* GENERATED by tools/ui_kit/make_icons.py — do not edit by hand. */',
         '#include "uk_icons.h"', '']
    for size in SIZES:
        for name in names:
            data = pack_a4(render(name, size))
            sym = f'uk_icon_{name}_{size}'
            c.append(f'static const uint8_t {sym}_map[] = {{')
            for i in range(0, len(data), 24):
                c.append('    ' + ','.join(f'0x{b:02x}' for b in data[i:i + 24]) + ',')
            c.append('};')
            c.append(f'static const lv_img_dsc_t {sym} = {{')
            c.append(f'    .header.cf = LV_IMG_CF_ALPHA_4BIT, .header.always_zero = 0,')
            c.append(f'    .header.w = {size}, .header.h = {size},')
            c.append(f'    .data_size = sizeof({sym}_map), .data = {sym}_map,')
            c.append('};')
    c.append('')
    for size in SIZES:
        c.append(f'static const lv_img_dsc_t *const s_icons_{size}[UK_ICON__COUNT] = {{')
        for name in names:
            c.append(f'    [UK_ICON_{name.upper()}] = &uk_icon_{name}_{size},')
        c.append('};')
    c.append('')
    c.append('const lv_img_dsc_t *uk_icon_img(uk_icon_t icon, uk_icon_size_t size)')
    c.append('{')
    c.append('    if ((unsigned)icon >= UK_ICON__COUNT) return NULL;')
    c.append(f'    return size == UK_ICON_LG ? s_icons_{SIZES[1]}[icon] : s_icons_{SIZES[0]}[icon];')
    c.append('}')

    h = ['/* GENERATED by tools/ui_kit/make_icons.py — do not edit by hand. */',
         '#pragma once', '#include "lvgl.h"', '',
         '/* Line icons for the UI kit, as recolourable 4-bit alpha images. */',
         'typedef enum {']
    for name in names:
        h.append(f'    UK_ICON_{name.upper()},')
    h += ['    UK_ICON__COUNT,', '    UK_ICON_NONE = -1,', '} uk_icon_t;', '',
          'typedef enum {',
          f'    UK_ICON_MD = 0,   /* {SIZES[0]} px — buttons, rows, the dock */',
          f'    UK_ICON_LG = 1,   /* {SIZES[1]} px — tiles */',
          '} uk_icon_size_t;', '',
          f'#define UK_ICON_PX_MD {SIZES[0]}', f'#define UK_ICON_PX_LG {SIZES[1]}', '',
          'const lv_img_dsc_t *uk_icon_img(uk_icon_t icon, uk_icon_size_t size);', '']

    with open(os.path.join(OUT_DIR, 'uk_icons.c'), 'w', newline='\n') as f:
        f.write('\n'.join(c) + '\n')
    with open(os.path.join(OUT_DIR, 'uk_icons.h'), 'w', newline='\n') as f:
        f.write('\n'.join(h) + '\n')

    # A contact sheet so a person can eyeball every icon at its real size.
    if '--sheet' in sys.argv:
        sheet = Image.new('L', (len(names) * 40, 80), 0)
        for i, name in enumerate(names):
            sheet.paste(render(name, SIZES[0]), (i * 40 + 9, 9))
            sheet.paste(render(name, SIZES[1]), (i * 40 + 4, 44))
        path = sys.argv[sys.argv.index('--sheet') + 1]
        sheet.save(path)
        print('sheet ->', path)
    print(f'{len(names)} icons x {len(SIZES)} sizes')


if __name__ == '__main__':
    main()
