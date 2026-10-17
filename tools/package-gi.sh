#!/bin/bash
# Builds the release assets for Astra's Games tab (astra-bepinex/docs/GAME-INTEGRATION-MANIFEST.md), one per game:
#   build/gi/ds1/astra-gi.zip   Death Stranding Director's Cut   -> the release tagged ds1-v<version>, as astra-gi.zip
#   build/gi/ds2/astra-gi.zip   Death Stranding 2: On the Beach  -> the release tagged ds2-v<version>, as astra-gi.zip
# Each is gi/<game>/astra-gi.toml + every file it names (+ LICENSES/), from
#   ./build.sh's output (astra-ds.addon64, and the foundation's astra.addon64 from extern/astra-reshade),
#   shaders/AstraGame.fxh and the foundation's AstraPassthrough.fx (no ReShade.ini: the add-on sets up its own
#   ReShade.ini keys, astra-reshade/src/config_rules.h),
#   ReShade 6.8.0 Addon's ReShade64.dll and ReShade.fxh (the Minecraft submodule's tools/fetch_deps.sh).
# The foundation's tools/gi-pack.py fills in our digests, checks every pinned one and writes the zip (reproducible).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding}
AR=${ASTRA_RESHADE_DIR:-$HERE/extern/astra-reshade}
TP="$MC/third_party"
CACHE="$HERE/build/cache"
RESHADE_LICENSE_URL=https://raw.githubusercontent.com/crosire/reshade/v6.8.0/LICENSE.md

need() { [ -f "$1" ] || { echo "missing $1 ($2)"; exit 1; }; }
need "$HERE/build/astra-ds.addon64" "run ./build.sh"
need "$AR/build/astra.addon64" "run ./build.sh"
need "$AR/tools/gi-pack.py" "the foundation submodule is too old: it has no tools/gi-pack.py"
need "$TP/reshade-6.8.0-addon/ReShade64.dll" "run $MC/tools/fetch_deps.sh"
need "$TP/shaders/ReShade.fxh" "run $MC/tools/fetch_deps.sh"
mkdir -p "$CACHE"
[ -f "$CACHE/ReShade-LICENSE.md" ] || curl -fsSL -o "$CACHE/ReShade-LICENSE.md" "$RESHADE_LICENSE_URL" || { echo "ReShade's licence: download failed"; exit 1; }

for game in ds1 ds2; do
	OUT="$HERE/build/gi/$game/staged"
	rm -rf "$OUT" "$HERE/build/gi/$game/astra-gi.zip"
	mkdir -p "$OUT/astra-reshade/Shaders" "$OUT/LICENSES"
	cp "$TP/reshade-6.8.0-addon/ReShade64.dll" "$OUT/dxgi.dll"
	cp "$AR/build/astra.addon64" "$HERE/build/astra-ds.addon64" "$OUT/"
	cp "$AR/shaders/AstraPassthrough.fx" "$HERE/shaders/AstraGame.fxh" "$TP/shaders/ReShade.fxh" "$OUT/astra-reshade/Shaders/"
	cp "$HERE/LICENSE" "$OUT/LICENSES/astra-in-death-stranding-MIT.txt"
	cp "$HERE/THIRD_PARTY.md" "$OUT/LICENSES/THIRD_PARTY.md"
	cp "$AR/LICENSE" "$OUT/LICENSES/astra-reshade-MIT.txt"
	# DS's camera, Sam, collision, UI, the WebSocket client and the fault guard compiled into the add-ons are this MIT
	# repository's (and its lineage's)
	cp "$MC/LICENSE" "$OUT/LICENSES/minecraft-in-death-stranding-MIT.txt"
	cp "$MC/mc/LICENSE.universal-modder" "$OUT/LICENSES/universal-modder-MIT.txt"
	cp "$CACHE/ReShade-LICENSE.md" "$OUT/LICENSES/ReShade-BSD-3-Clause.md"
	python3 "$AR/tools/gi-pack.py" "$HERE/gi/$game/astra-gi.toml" "$OUT" "$HERE/build/gi/$game/astra-gi.zip"
done
