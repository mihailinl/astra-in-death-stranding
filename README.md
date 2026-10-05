# Astra in Death Stranding (private)

A ReShade add-on that puts Astra, the Astra companion's 3D avatar, next to Sam in **Death Stranding Director's
Cut**. She walks with him on DS's real collision and rides in his truck, roadster or trike. The Astra avatar engine
draws her from DS's camera, and the add-on composites her into DS's picture with depth: DS's rocks and Minecraft's
blocks hide her, DS's UI stays on top.

It is a separate add-on from [minecraft-in-death-stranding](https://github.com/mihailinl/minecraft-in-death-stranding)
(`dsmc.addon64`). It works alone, or next to the Minecraft add-on.

```
Death Stranding (Proton)                                  Astra avatar engine (native, game bridge mode)
  astra.addon64
    astra.cpp      where she walks: a spot beside and behind Sam, on DS's collision; his vehicle's seat
    astralink.cpp  ---- WebSocket 127.0.0.1:25600 ---->   cam (DS's camera) + avatar (pose, orientation, animator params)
                   <-----------------------------------   hello (where its frames are), events
    layer.cpp      <--- shared memory ("MCPT") --------   her picture + linear depth, and the camera it was drawn from
                        /dev/shm/astra-frame (Wine: Z:\dev\shm\astra-frame), Windows: Local\AstraFrame
  AstraPassthrough.fx  re-projects her picture onto DS's camera, depth-tests it against DS and Minecraft, DS's UI over
```

Shared with the Minecraft mod through the submodule `extern/minecraft-in-death-stranding`, not copied:
- `game` reads DS's camera and Sam;
- `physics` makes DS's collision queries;
- `guard` contains faults;
- `ws` is the WebSocket client;
- `ui` finds DS's UI target and lays it over what the add-ons draw (one add-on per process does it).

## How she moves

- **Walking:**
  - Rays down use the layer of Sam's own feet (96). They find terrain, building floors, structures, chiral bridges and
    Minecraft blocks' boxes.
  - Spheres test walls and ceilings on what stops Sam (34).
  - She steps up to 0.6 m, goes round slopes over ~49° and falls with gravity.
  - She comes back to Sam when she is 28 m behind, a floor apart for 2.5 s, stuck, or falling.
- **Riding:**
  - Sam is found sitting in a vehicle by rays through him: truck `DSVehicleTruck`, trike `DSVehicleMotorbike`,
    roadster `DSVehicleSpeedCar`, by their vtables.
  - She takes the passenger seat: Sam's seat mirrored across the vehicle, or behind him on the trike. She gets the
    vehicle's orientation.
  - The engine plays its `seat` state, and her hips are pinned to the seat (`anchor: hips`) after its sit-down clip.
    `unseat` stands her up.
  - The seat point can be tuned in the overlay.
- **Latency:** an older picture of her is moved by how far she went since it was drawn, so running and driving don't
  drag her.

## Build and install

```bash
git clone --recursive <this repo> && cd astra-in-death-stranding
extern/minecraft-in-death-stranding/tools/fetch_deps.sh   # ReShade headers, imgui (into the submodule's third_party/)
./build.sh                                                # -> build/astra.addon64 (clang-cl + xwin, like the Minecraft mod)
./install.sh                                              # astra.addon64 + AstraPassthrough.fx into DS's folder
```

ReShade must already be in DS's folder. The Minecraft mod's `install.sh` puts it there; on Linux that also needs the
launch option `WINEDLLOVERRIDES="dxgi=n,b" %command%`. `./install.sh --remove` takes Astra out again. Her technique
enables itself (`enabled = true`), so no preset changes are needed.

## Running

1. Start the engine in bridge mode (`WGPU_PROTO_WINDOW=bridge`, see the engine's game-bridge notes). Without it,
   `uv run --with numpy --with websockets tools/fake_astra.py` stands in: a capsule in her place.
2. Start DS. ReShade's overlay has an **Astra** tab: the engine link, "Astra follows Sam", "Call Astra", the seat
   sliders, and DS's UI switch and frame trace (when this add-on is the one doing it).

The add-on logs to `astra.log` in DS's folder.

## Updating the shared code

```bash
git -C extern/minecraft-in-death-stranding pull origin main && git add extern/minecraft-in-death-stranding
```
