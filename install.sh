#!/bin/bash
# A developer's install into Death Stranding's folder, from ./build.sh's output (players install through Astra's
# Games tab, or by hand from the release zip: README.md). ReShade must already be there (ReShade's own installer with
# full add-on support, or the Minecraft mod's install.sh): this adds Astra's files beside it.
#   ./install.sh            the foundation (astra.addon64 + AstraPassthrough.fx), DS's part (astra-ds.addon64 +
#                           AstraGame.fxh); the technique enables itself: no preset changes
#   ./install.sh --remove   takes them out again
#   ./install.sh --ds2 ...  the same for Death Stranding 2
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding}
AR=${ASTRA_RESHADE_DIR:-$HERE/extern/astra-reshade}
GAME_ID=1; REMOVE=0
for a in "$@"; do
	case "$a" in
		--ds2) GAME_ID=2 ;;
		--remove) REMOVE=1 ;;
		*) echo "unknown option $a"; exit 1 ;;
	esac
done
if [ $GAME_ID = 2 ]; then
	GAME=${DS_DIR:-$("$MC/tools/find_ds.sh" 2)}; EXE=DS2.exe; MANIFEST="$HERE/installed-ds2.txt"
else
	GAME=${DS_DIR:-$("$MC/tools/find_ds.sh")}; EXE=ds.exe; MANIFEST="$HERE/installed.txt"
fi

[ -f "$GAME/$EXE" ] || { echo "$EXE not found in $GAME (set DS_DIR)"; exit 1; }

if [ $REMOVE = 1 ]; then
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
put "$AR/build/astra.addon64" astra.addon64
put "$HERE/build/astra-ds.addon64" astra-ds.addon64
put "$AR/shaders/AstraPassthrough.fx" astra-reshade/Shaders/AstraPassthrough.fx
put "$HERE/shaders/AstraGame.fxh" astra-reshade/Shaders/AstraGame.fxh

sort -u "$MANIFEST.new" > "$MANIFEST"; rm -f "$MANIFEST.new"
echo "installed into $GAME:"; sed 's/^/  /' "$MANIFEST"
