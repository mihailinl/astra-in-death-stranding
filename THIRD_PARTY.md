# Third-party software

What a release of Astra in Death Stranding (`astra-gi.zip`) ships or compiles in besides this repository's own code
(MIT, `LICENSE`). The release zip carries each licence text in `LICENSES/`.

| what | where it is | licence | source |
|---|---|---|---|
| **ReShade** 6.8.0 with full add-on support: `ReShade64.dll`, shipped unmodified as `dxgi.dll` | the zip | BSD-3-Clause, Copyright 2014 Patrick Mours | <https://reshade.me>, <https://github.com/crosire/reshade> (tag v6.8.0) |
| ReShade's add-on API headers (`reshade*.hpp`) | compiled into both add-ons (fetched at build time, not in this repository) | BSD-3-Clause | <https://github.com/crosire/reshade/tree/v6.8.0/include> |
| `ReShade.fxh` (shipped unmodified) | the zip | CC0-1.0 | <https://github.com/crosire/reshade-shaders> (branch `slim`) |
| Dear ImGui headers `imgui.h`, `imconfig.h` 1.92.5 (docking; ReShade provides the implementation) | compiled into both add-ons (fetched at build time) | MIT, Copyright (c) Omar Cornut | <https://github.com/ocornut/imgui> |
| **astra-reshade**, the Astra foundation: `astra.addon64`, `AstraPassthrough.fx` | git submodule `extern/astra-reshade`; the zip | MIT, Copyright (c) 2026 mihailinl | <https://github.com/mihailinl/astra-reshade> |
| **minecraft-in-death-stranding**: Death Stranding's camera, Sam, collision, UI (`ds/src/game, physics, ui`), the fault guard and the WebSocket client | git submodule `extern/minecraft-in-death-stranding`; compiled into the add-ons | MIT, Copyright (c) 2026 mihailinl | <https://github.com/mihailinl/minecraft-in-death-stranding> |
| universal-modder's Minecraft x GTA V passthrough example: the lineage of the compositor and of the WebSocket client | (derived code, in the two repositories above) | MIT, Copyright (c) 2026 Rehan and universal-modder contributors | named in minecraft-in-death-stranding's `mc/LICENSE.universal-modder` |
| The Microsoft C/C++ runtime, linked statically (`/MT`) from the MSVC CRT and Windows SDK that `xwin` downloads | compiled into both add-ons | Microsoft's Visual Studio / Windows SDK licence terms (accepted by whoever runs `xwin`) | <https://github.com/Jake-Shadle/xwin> |

Death Stranding and Death Stranding 2 are KOJIMA PRODUCTIONS' games; nothing of theirs is in this repository or its
releases. The add-ons read the game's memory while it runs, at offsets this project found in the games' own exported
script symbols and run-time type information (see `extern/minecraft-in-death-stranding/ds/src/game.h`).
