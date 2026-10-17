#!/bin/bash
# tests/foundation_relink_tests.cpp - does the provider's link to the compositor (src/foundation.cpp's find()/
# api()) come back after astra.addon64 is unloaded and a fresh instance is loaded in its place (ReShade unloads
# and reloads its add-ons with every D3D device; DS2 does this once at start-up, DS1 on an alt-tab or a
# resolution change)? Drives the real src/foundation.cpp against two tiny stand-in DLLs
# (tests/stub_foundation.cpp, tagged A and B) - built for Windows (clang-cl, the xwin CRT/SDK as build.sh) and
# run under Wine, in the same prefix astra-reshade's own tools/winecheck.sh uses. Without wine or xwin it says
# so and passes.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
X=${XWIN:-$HOME/.xwin}
AR=${ASTRA_RESHADE_DIR:-$HERE/extern/astra-reshade}
command -v wine >/dev/null || { echo "foundationtest: no wine, skipped"; exit 0; }
command -v clang-cl >/dev/null || { echo "foundationtest: no clang-cl, skipped"; exit 0; }
[ -d "$X/crt/include" ] || { echo "foundationtest: no xwin CRT/SDK at $X, skipped"; exit 0; }
[ -f "$AR/include/astra_provider.h" ] || { echo "foundationtest: the astra-reshade submodule is missing, skipped"; exit 0; }

OUT="$HERE/build/foundationtest"
rm -rf "$OUT"
mkdir -p "$OUT/stubA" "$OUT/stubB"
CLFLAGS="--target=x86_64-pc-windows-msvc /nologo /O1 /MT /EHsc /std:c++20 /DNOMINMAX /DWIN32_LEAN_AND_MEAN -fuse-ld=lld-link \
	-Wno-unused-command-line-argument /imsvc $X/crt/include /imsvc $X/sdk/include/ucrt /imsvc $X/sdk/include/um /imsvc $X/sdk/include/shared"
LDFLAGS="/link /libpath:$X/crt/lib/x86_64 /libpath:$X/sdk/lib/um/x86_64 /libpath:$X/sdk/lib/ucrt/x86_64"

(cd "$OUT/stubA" && clang-cl $CLFLAGS "/DASTRA_STUB_TAG='A'" /I "$AR/include" /LD "$HERE/tests/stub_foundation.cpp" /Fe"astra.addon64" $LDFLAGS >/dev/null && rm -f ./*.obj ./*.lib ./*.exp)
(cd "$OUT/stubB" && clang-cl $CLFLAGS "/DASTRA_STUB_TAG='B'" /I "$AR/include" /LD "$HERE/tests/stub_foundation.cpp" /Fe"astra.addon64" $LDFLAGS >/dev/null && rm -f ./*.obj ./*.lib ./*.exp)
(cd "$OUT" && clang-cl $CLFLAGS /I "$HERE/src" /I "$AR/include" "$HERE/tests/foundation_relink_tests.cpp" "$HERE/src/foundation.cpp" /Fe"foundation_relink_tests.exe" $LDFLAGS >/dev/null && rm -f ./*.obj)

export WINEPREFIX="$HERE/build/foundationtest/wineprefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$WINEPREFIX"
[ -f "$WINEPREFIX/system.reg" ] || wineboot -i >/dev/null 2>&1
rm -f "$OUT/stub.log"
ASTRA_STUB_DIR_A="$(winepath -w "$OUT/stubA")" \
ASTRA_STUB_DIR_B="$(winepath -w "$OUT/stubB")" \
ASTRA_STUB_LOG="$(winepath -w "$OUT/stub.log")" \
	wine "$OUT/foundation_relink_tests.exe"
