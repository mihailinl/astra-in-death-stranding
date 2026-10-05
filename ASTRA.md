# Astra in Death Stranding (private)

Astra, the Astra companion's 3D avatar, walks next to Sam in **Death Stranding Director's Cut**. This is the public
[minecraft-in-death-stranding](https://github.com/mihailinl/minecraft-in-death-stranding) mod plus the Astra
integration. It stays private: everything about Astra lives only here.

## How it works

```
Death Stranding (Proton)                                    Astra avatar engine (native, game bridge mode)
  dsmc.addon64
    astra.cpp      where she walks: follows a spot beside and behind Sam on DS's real collision
    astralink.cpp  ---- WebSocket 127.0.0.1:25600 ---->     cam (DS's camera) + avatar (pose, animator params)
                   <-----------------------------------     hello (frames' location, states, params), events
    compositor     <--- shared memory "MCPT" ----------     her picture + linear depth, rendered from DS's camera
                        /dev/shm/astra-frame (Wine: Z:\dev\shm\astra-frame), Windows Local\AstraFrame
    MCPassthrough.fx: DS, Minecraft and Astra ordered per pixel by depth
```

**`astra.cpp`: her walking (DS decides where she is).**
- Rays down on the layer Sam's own feet use (96, Sam ignored) find the ground she stands on: terrain, building
  floors, structures, chiral bridges, Minecraft blocks' boxes.
- Spheres on what stops Sam (34) test for walls and ceilings.
- She steps up to 0.6 m and won't walk up slopes over ~49°. She slides along walls and falls with gravity.
- She comes back to Sam when she is 28 m behind, a floor above or below him for 2.5 s, stuck, or falling.

**`astralink.cpp`: the engine's game bridge, spec v1.** The spec is the engine's `notes/game-bridge.md`, and later
its `BRIDGE.md`.
- Coordinates are glTF (right-handed, Y up, metres), measured from a floating origin near Sam.
- `cam` carries `echo`: DS's camera angles in Minecraft's convention. The engine copies them into the frame's slot,
  so the compositor knows which camera a frame was rendered from and re-projects it to DS's current camera.
- `avatar` carries her placement (`pos`, `fwd`, `vel`) and raw animator parameters (`params`: `speed`,
  `airborne`, `vspeed`). What a speed means (walk, run) is the animation set's decision.

**Compositor.** Astra is a second frame source next to Minecraft, with her own textures and pose.
- Frame flags are honoured: 8 means linear depth, and rows are top-down when flag 2 is unset.
- Her light and grade matching are separate and weaker, because the engine already lights her with MToon.
- There is no glow on her.

## Running

1. Build and install as in the README: `ds/build.sh`, then `./install.sh`. This repo's add-on includes Astra.
   `third_party/` comes from `tools/fetch_deps.sh`; locally it can be a link to the public checkout's.
2. Start the engine in bridge mode (phase 1; later the Astra daemon will start it):
   `WGPU_PROTO_WINDOW=bridge cargo run --release …` in the engine's repo (see its notes). Without the engine,
   `uv run --with numpy --with websockets tools/fake_astra.py` stands in for it: a capsule in her place (pink
   front, blue back; the band is grey when standing, green when walking, orange when running, red in the air).
3. Start DS. The overlay's DSMC tab shows `Astra engine: linked`, has the "Astra follows Sam" switch, a "Call
   Astra" button and the debug marker.

Minecraft isn't needed for Astra.

## Status

- **Working:** following Sam on DS's collision, the link, compositing with the stand-in engine. User:
  "now its good".
- **Engine side, done by the engine's own agent:**
  - phase 1: picture, camera, pose;
  - phase 2: commands (dance, sit, mood, look-at) and events;
  - phase 3: locomotion clips (until then she glides in idle);
  - phase 4: foot IK, the game's light, a cropped readback.

## Keeping up with the public mod

```bash
git pull upstream main     # the public Minecraft mod; Astra's changes are on top
```

The files that differ from upstream are `astra.*`, `astralink.*`, the compositor, `MCPassthrough.fx`,
`DSMC_Debug.fx` (her marker), `dsmc.cpp` (her overlay and frame calls), `host.*` (`camera_angles`) and
`tools/fake_astra.py`.
