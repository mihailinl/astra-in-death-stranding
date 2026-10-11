# Astra in Death Stranding

Astra, the Astra companion's 3D avatar, walks beside Sam in **Death Stranding
Director's Cut** and **Death Stranding 2: On the Beach** (PC, Steam). She follows him on the game's real collision,
strolls a little while he stands, and rides along in his truck, roadster or trike. Her own engine draws her from the
game's camera; she is composited into the game's picture with its depth, so rocks and buildings hide her and the
game's UI stays on top. She is lit by the game's sun and casts her shadow on its ground.

It is a [ReShade](https://reshade.me) add-on in two parts:

- **`astra.addon64`** — the [Astra foundation](https://github.com/mihailinl/astra-reshade), the same for every game:
  the link to her engine, her pictures, the depth test, the re-projection onto the game's camera;
- **`astra-ds.addon64`** — this repository: what only Death Stranding can say — where its camera is, where Sam is, its
  sun, and where she walks.

Supported builds: Death Stranding Director's Cut, Steam build 13300582; Death Stranding 2, Steam build 23923251. On
any other build the add-on finds the game's code does not match and stays idle (the overlay says so) — the game is
never touched.

## Install

**With Astra (recommended):** Astra's **Games** tab installs it and starts the game with her. Astra's engine must be
running (it is, when the companion is on).

**By hand**, from a release's `astra-gi.zip` (the `ds1-v…` release for Director's Cut, `ds2-v…` for Death
Stranding 2), into the game's folder (where `ds.exe` / `DS2.exe` is):

- `dxgi.dll` (ReShade 6.8.0 with full add-on support) and `ReShade.ini`;
- `astra.addon64` and `astra-ds.addon64`;
- `reshade-shaders/Shaders/AstraPassthrough.fx`, `AstraGame.fxh` and `ReShade.fxh`.

If ReShade with add-on support is already there (for example from Minecraft in Death Stranding), keep your
`dxgi.dll` and `ReShade.ini` and add the rest. Her engine's port is 25600 by default; Astra passes its own port and
token on the game's command line (`-AstraConfig=<file>`).

**Linux (Proton):** ReShade loads as `dxgi.dll` only with the launch option
`WINEDLLOVERRIDES="dxgi=n,b" %command%` (Astra's Games tab sets this override in the game's Proton prefix for you).

Remove: delete those files again (and `dxgi.dll` + `ReShade.ini` if nothing else uses ReShade).

## In the game

ReShade's overlay (Home key) has two tabs:

- **Astra** (the foundation): the link to her engine, her pictures, "Show Astra", her size, "wait for her picture".
- **Astra in Death Stranding**: "Astra follows Sam", **Call Astra** (she comes to Sam now), her size, the passenger
  seat's height and offset, the sun she is lit by, and DS's UI switch.

The log is `astra-reshade.log`, beside Astra's settings for the game (or beside the game's exe).

## How she moves

- **Walking:**
  - Rays down use the layer of Sam's own feet. They find terrain, building floors, structures, chiral bridges and
    Minecraft blocks' boxes (with Minecraft in Death Stranding).
  - Spheres test walls and ceilings on what stops Sam.
  - She steps up to 0.6 m, goes round slopes over ~49° and falls with gravity.
  - She comes back to Sam when she is 28 m behind, a floor apart for 2.5 s, stuck, or falling.
  - In Death Stranding 2 her walking runs on the game's own update thread (the only one its physics answers on).
- **Riding:** Sam is found sitting in a vehicle by rays through him (truck, trike, roadster). She takes the passenger
  seat — Sam's seat mirrored across the vehicle, or behind him on the trike — and the vehicle's orientation. Her
  engine plays its `seat` state and pins her hips to the seat after its sit-down clip; `unseat` stands her up.
- **What her engine is told** is raw facts — her speed, whether she is in the air, her vertical speed, whether she
  is seated — never "walk" or "run": what they mean is her animation set's business, which the owner can change.

## With Minecraft in Death Stranding

Both mods share one ReShade. Minecraft's blocks hide her where they are in front of her, and DS's UI is laid back over
both (whichever add-on loads first does that for every effect).

## Build from source (Linux)

```bash
git clone --recursive https://github.com/mihailinl/astra-in-death-stranding && cd astra-in-death-stranding
extern/minecraft-in-death-stranding/tools/fetch_deps.sh                         # ReShade's headers, imgui, ReShade 6.8.0
extern/astra-reshade/extern/minecraft-in-death-stranding/tools/fetch_deps.sh    # the same for the foundation's pin
./build.sh            # clang-cl + lld-link + xwin's MSVC CRT/SDK -> build/astra-ds.addon64, extern/astra-reshade/build/astra.addon64
tools/package-gi.sh   # -> build/gi/ds1/astra-gi.zip, build/gi/ds2/astra-gi.zip (the release assets)
./install.sh [--ds2]  # a developer's install into the game's folder (ReShade must be there already); --remove
```

`xwin splat --output ~/.xwin` provides the MSVC CRT and Windows SDK (`XWIN` points elsewhere). `MC_DIR` and
`ASTRA_RESHADE_DIR` build against local checkouts of the two submodules instead.

| path | what |
|---|---|
| `src/main.cpp` | the add-on: once per presented frame (the foundation's present callback) DS's camera, her placement and the sun go to the foundation |
| `src/astra.cpp` | where she walks and rides |
| `src/sun.cpp` | DS's sun (or moon), found in the game's memory |
| `src/foundation.cpp` | the foundation's provider API (`extern/astra-reshade/include/astra_provider.h`), looked up in `astra.addon64` |
| `shaders/AstraGame.fxh` | DS's hooks into the foundation's effect: DS's UI over her, Minecraft's blocks in front of her |
| `gi/` | the release manifests (`ds1/`, `ds2/`) and their `ReShade.ini` |
| `extern/minecraft-in-death-stranding` | DS's camera, Sam, collision and UI, shared with the Minecraft mod |
| `extern/astra-reshade` | the foundation |

## Licence

MIT (`LICENSE`), with the lineage it names. ReShade is BSD-3-Clause; everything else a release ships or compiles in is
listed in `THIRD_PARTY.md`. Death Stranding is KOJIMA PRODUCTIONS' game; this is an unofficial fan mod, not affiliated
with or endorsed by them.
