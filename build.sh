#!/bin/bash
# Cross-build on Linux: clang-cl + lld-link against the MSVC CRT / Windows SDK from xwin (xwin splat --output ~/.xwin).
#   ./build.sh   ->  build/astra-ds.addon64                    this add-on: Death Stranding's part (the provider)
#                    extern/astra-reshade/build/astra.addon64  the Astra foundation it runs on (its own build.sh)
# Two submodules (git submodule update --init --recursive), each with ReShade's headers and imgui fetched once:
#   extern/minecraft-in-death-stranding/tools/fetch_deps.sh                       DS's camera, Sam, collision, UI
#   extern/astra-reshade/extern/minecraft-in-death-stranding/tools/fetch_deps.sh  (the foundation's own pin)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
X=${XWIN:-$HOME/.xwin}
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding} # MC_DIR: a local checkout instead (unpushed shared changes)
AR=${ASTRA_RESHADE_DIR:-$HERE/extern/astra-reshade}     # ASTRA_RESHADE_DIR: a local checkout of the foundation instead
TP="$MC/third_party"
[ -f "$MC/ds/src/game.cpp" ] && [ -f "$AR/include/astra_provider.h" ] || { echo "the submodules are missing: git submodule update --init --recursive"; exit 1; }
[ -f "$TP/reshade-include/reshade.hpp" ] || { echo "run $MC/tools/fetch_deps.sh first"; exit 1; }
[ -d "$X/crt/include" ] || { echo "no MSVC CRT / Windows SDK at $X (xwin splat --output $X, or set XWIN)"; exit 1; }
mkdir -p "$HERE/build"
cat > "$HERE/compile_flags.txt" <<FLAGS
--driver-mode=cl
--target=x86_64-pc-windows-msvc
/std:c++20
/EHsc
/DNOMINMAX
/TP
-imsvc$X/crt/include
-imsvc$X/sdk/include/ucrt
-imsvc$X/sdk/include/um
-imsvc$X/sdk/include/shared
-I$(cd "$TP" && pwd)/reshade-include
-I$(cd "$TP" && pwd)/imgui
-I$AR/include
-I$MC/ds/src
FLAGS
cd "$HERE/build"
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc /MT /LD /W3 /DNOMINMAX -fuse-ld=lld-link \
	-Wno-unused-command-line-argument -Wno-missing-braces \
	/imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$TP/reshade-include" /I "$TP/imgui" /I "$AR/include" /I "$HERE/src" /I "$MC/ds/src" \
	"$HERE"/src/*.cpp "$MC"/ds/src/{game,physics,guard,ui}.cpp /Fe"astra-ds.addon64" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64"
rm -f ./*.obj ./*.lib ./*.exp
ls -la astra-ds.addon64
# the foundation, from its pinned commit (its own build.sh, its own submodule)
"$AR/build.sh"
