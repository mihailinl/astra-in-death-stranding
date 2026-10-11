#!/bin/bash
# Cross-build the Astra add-on on Linux: clang-cl + lld-link against MSVC CRT / Windows SDK from xwin.
#   ./build.sh    ->  build/astra.addon64
# Shared code (DS's camera, collision, fault containment, WebSocket, DS's UI) comes from the Minecraft mod, a git
# submodule: git submodule update --init; its tools/fetch_deps.sh fetches ReShade's headers and imgui.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
X=${XWIN:-$HOME/.xwin}
MC=${MC_DIR:-$HERE/extern/minecraft-in-death-stranding} # MC_DIR: a local checkout instead (unpushed shared changes)
TP="$MC/third_party"
[ -f "$TP/reshade-include/reshade.hpp" ] || { echo "run $MC/tools/fetch_deps.sh first"; exit 1; }
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
-I$MC/ds/src
FLAGS
cd "$HERE/build"
clang-cl --target=x86_64-pc-windows-msvc /nologo /std:c++20 /O2 /EHsc /MT /LD /W3 /DNOMINMAX -fuse-ld=lld-link \
	-Wno-unused-command-line-argument \
	/imsvc "$X/crt/include" /imsvc "$X/sdk/include/ucrt" /imsvc "$X/sdk/include/um" /imsvc "$X/sdk/include/shared" \
	/I "$TP/reshade-include" /I "$TP/imgui" /I "$HERE/src" /I "$MC/ds/src" \
	"$HERE"/src/*.cpp "$MC"/ds/src/{game,physics,guard,ws,ui}.cpp /Fe"astra.addon64" \
	/link /libpath:"$X/crt/lib/x86_64" /libpath:"$X/sdk/lib/um/x86_64" /libpath:"$X/sdk/lib/ucrt/x86_64"
rm -f ./*.obj ./*.lib ./*.exp
ls -la astra.addon64
