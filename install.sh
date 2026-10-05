#!/bin/bash
# Put the Astra add-on into Death Stranding's folder (next to ReShade, which this doesn't install: the Minecraft mod's
# install.sh does, or ReShade's own installer with full add-on support).
#   ./install.sh            astra.addon64 + AstraPassthrough.fx (its technique enables itself: no preset changes)
#   ./install.sh --remove   takes them out again
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MC="$HERE/extern/minecraft-in-death-stranding"
GAME=${DS_DIR:-$("$MC/tools/find_ds.sh")}
MANIFEST="$HERE/installed.txt"

[ -f "$GAME/ds.exe" ] || { echo "ds.exe not found in $GAME (set DS_DIR)"; exit 1; }

if [ "$1" = --remove ]; then
	[ -f "$MANIFEST" ] || { echo "nothing recorded in $MANIFEST"; exit 0; }
	while IFS= read -r rel; do rm -f -- "$GAME/$rel"; done < "$MANIFEST"
	rm -f "$MANIFEST"
	echo "Astra removed"
	exit 0
fi

[ -f "$GAME/dxgi.dll" ] || { echo "ReShade isn't in $GAME: install it first ($MC/install.sh, or ReShade's installer with add-on support)"; exit 1; }

: > "$MANIFEST.new"
put() { # put <source> <path relative to the game folder>
	echo "$2" >> "$MANIFEST.new"
	cmp -s "$1" "$GAME/$2" && return
	mkdir -p "$(dirname "$GAME/$2")"
	cp -f "$1" "$GAME/$2.new" && mv -f "$GAME/$2.new" "$GAME/$2"
}
put "$HERE/build/astra.addon64" astra.addon64
put "$HERE/shaders/AstraPassthrough.fx" reshade-shaders/Shaders/AstraPassthrough.fx

sort -u "$MANIFEST.new" > "$MANIFEST"; rm -f "$MANIFEST.new"
echo "installed into $GAME:"; sed 's/^/  /' "$MANIFEST"
