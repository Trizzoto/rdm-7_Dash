#!/bin/bash
# Run from the RDM-7_Dash root directory
# Usage: bash tools/setup_wasm_project.sh /path/to/new/folder

DEST="${1:-../rdm7-wasm-preview}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"

echo "Copying files from $SRC to $DEST"

mkdir -p "$DEST"/{docs,reference,firmware_src/{widgets,layout,can,ui},web}

# Build guide
cp "$SRC/docs/WASM_PREVIEW_BUILD_GUIDE.md" "$DEST/docs/"

# Reference (web server for API patterns)
cp "$SRC/main/net/web_server.c" "$DEST/reference/"
cp "$SRC/main/net/web_server.h" "$DEST/reference/" 2>/dev/null

# Widget sources
for f in widget_panel widget_bar widget_text widget_button widget_toggle \
         widget_arc widget_meter widget_image widget_warning widget_shift_light \
         widget_types widget_registry widget_rules signal signal_internal \
         font_manager; do
    cp "$SRC/main/widgets/${f}.c" "$DEST/firmware_src/widgets/" 2>/dev/null
    cp "$SRC/main/widgets/${f}.h" "$DEST/firmware_src/widgets/" 2>/dev/null
done

# Layout manager
cp "$SRC/main/layout/layout_manager.c" "$DEST/firmware_src/layout/"
cp "$SRC/main/layout/layout_manager.h" "$DEST/firmware_src/layout/"
cp "$SRC/main/layout/default_layout.c" "$DEST/firmware_src/layout/" 2>/dev/null

# CAN decode (pure math, no deps)
cp "$SRC/main/can/can_decode.c" "$DEST/firmware_src/can/"
cp "$SRC/main/can/can_decode.h" "$DEST/firmware_src/can/"

# Theme header
cp "$SRC/main/ui/theme.h" "$DEST/firmware_src/ui/"

# Web editor
cp "$SRC/main/web/index.html" "$DEST/web/"

# LVGL config (reference)
cp "$SRC/main/lv_conf.h" "$DEST/lv_conf_reference.h"

echo ""
echo "Done! Files copied to $DEST"
echo ""
echo "Next steps:"
echo "  cd $DEST"
echo "  git init"
echo "  Then paste the prompt into Claude Code"
