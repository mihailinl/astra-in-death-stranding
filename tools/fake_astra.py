#!/usr/bin/env python3
"""A stand-in for the Astra avatar engine's game bridge, to test the DS side without the engine.

Listens where the engine will (ws://127.0.0.1:25600), takes DS's "cam" and "avatar" messages (glTF coordinates:
right-handed, Y up, metres, relative to the add-on's floating origin) and renders a capsule where Astra stands, the
way the engine renders her: from DS's camera, into the shared memory the add-on composites ("MCPT" layout, the
Minecraft mod's FrameExporter; flags 8 | 16 like the engine: depth in linear metres, overlay plane unused,
rows top-down). Protocol: the engine's game bridge spec v1 (astra-avatar-engine notes/game-bridge.md).
The front of the capsule (her facing) is pink, the back blue; a band shows her animator parameters (standing grey,
"speed" over 0.15 green, over 2.2 orange, "airborne" red).

    uv run --with numpy --with websockets tools/fake_astra.py
"""
import asyncio
import json
import mmap
import os
import struct
import sys
import time

import numpy as np
import websockets

PORT = 25600
NAME = "/dev/shm/astra-frame"
MAGIC = 0x5450434D
HEADER, SLOT_DESC, SLOT_DESC_BYTES, SLOTS = 4096, 256, 128, 3
MAX_W, MAX_H = 1920, 1080
STRIDE = MAX_W * MAX_H * 4 * 3  # colour, depth, overlay (flag 16: the overlay plane is unused)
FLAGS = 8 | 16
RADIUS, FULL_HEIGHT = 0.28, 1.62

def band_colour(params):
    if params.get("seated"):
        return (0.7, 0.3, 1.0)
    if params.get("airborne"):
        return (1.0, 0.15, 0.15)
    speed = params.get("speed", 0.0)
    return (1.0, 0.55, 0.1) if speed > 2.2 else (0.2, 0.85, 0.3) if speed > 0.15 else (0.6, 0.6, 0.6)


class Shm:
    def __init__(self):
        size = HEADER + STRIDE * SLOTS
        fd = os.open(NAME, os.O_RDWR | os.O_CREAT, 0o644)
        os.ftruncate(fd, size)
        self.m = mmap.mmap(fd, size)
        os.close(fd)
        self.m[0:HEADER] = bytes(HEADER)
        struct.pack_into("<iiiiqii", self.m, 0, MAGIC, 1, HEADER, SLOTS, STRIDE, MAX_W, MAX_H)
        struct.pack_into("<qii", self.m, 32, 0, -1, os.getpid())
        self.slot, self.publish, self.frame = 0, 0, 0

    def write(self, colour, depth, cam):
        h, w = depth.shape
        slot = self.slot
        self.slot = (slot + 1) % SLOTS
        desc = SLOT_DESC + SLOT_DESC_BYTES * slot
        seq = struct.unpack_from("<q", self.m, desc)[0]
        if seq & 1:
            seq += 1
        struct.pack_into("<q", self.m, desc, seq + 1)
        base = HEADER + STRIDE * slot
        n = w * h * 4
        self.m[base:base + n] = colour.tobytes()
        self.m[base + n:base + 2 * n] = depth.astype("<f4").tobytes()
        self.frame += 1
        p, ypr = cam["pos"], cam["echo"]
        struct.pack_into("<qqiifffi", self.m, desc + 8, self.frame, cam["id"], w, h, cam["near"], cam["far"], cam["fovY"], FLAGS)
        struct.pack_into("<dddfffi", self.m, desc + 48, p[0], p[1], p[2], ypr[0], ypr[1], ypr[2], 0)
        struct.pack_into("<qq", self.m, desc + 88, time.monotonic_ns(), time.monotonic_ns())
        struct.pack_into("<q", self.m, desc, seq + 2)
        struct.pack_into("<i", self.m, 40, slot)
        self.publish += 1
        struct.pack_into("<q", self.m, 32, self.publish)


def render(w, h, cam, av):
    """Ray-traced capsule: premultiplied RGBA8 (rows top-down) and linear depth in metres (0: empty)."""
    eye = np.array(cam["pos"], np.float64)
    fwd = np.array(cam["fwd"], np.float64)
    up = np.array(cam["up"], np.float64)
    right = np.cross(fwd, up)
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    ty = np.tan(np.radians(cam["fovY"]) * 0.5)
    tx = ty * w / h
    colour = np.zeros((h, w, 4), np.uint8)
    depth = np.zeros((h, w), np.float32)

    # only the pixels her box covers on screen get rays (she's a small part of the picture)
    feet = np.array(av["pos"], np.float64)
    HEIGHT = 1.05 if av.get("params", {}).get("seated") else FULL_HEIGHT  # sitting: shorter
    corners = feet + np.array([[sx * RADIUS, sy * HEIGHT, sz * RADIUS] for sx in (-1, 1) for sy in (0, 1) for sz in (-1, 1)])
    rel = corners - eye
    cz = rel @ fwd
    if (cz < 0.05).all():
        return colour, depth  # behind the camera
    x0, x1, y0, y1 = 0, w, 0, h
    if (cz > 0.05).all():
        u = (rel @ right) / cz / tx
        v = (rel @ up) / cz / ty
        x0 = int(np.clip(np.floor((u.min() * 0.5 + 0.5) * w) - 2, 0, w))
        x1 = int(np.clip(np.ceil((u.max() * 0.5 + 0.5) * w) + 2, 0, w))
        y0 = int(np.clip(np.floor((0.5 - v.max() * 0.5) * h) - 2, 0, h))
        y1 = int(np.clip(np.ceil((0.5 - v.min() * 0.5) * h) + 2, 0, h))
        if x1 <= x0 or y1 <= y0:
            return colour, depth  # off screen
    xs = ((np.arange(x0, x1) + 0.5) / w * 2 - 1) * tx
    ys = (1 - (np.arange(y0, y1) + 0.5) / h * 2) * ty
    X, Y = np.meshgrid(xs, ys)
    d = fwd[None, None, :] + X[..., None] * right + Y[..., None] * up
    d /= np.linalg.norm(d, axis=-1, keepdims=True)

    a = feet + np.array([0, RADIUS, 0])
    b = feet + np.array([0, HEIGHT - RADIUS, 0])
    # capsule intersection (Inigo Quilez)
    ba = b - a
    oa = eye - a
    baba = ba @ ba
    bard = d @ ba
    baoa = oa @ ba
    rdoa = d @ oa
    oaoa = oa @ oa
    A = baba - bard * bard
    B = baba * rdoa - baoa * bard
    C = baba * oaoa - baoa * baoa - RADIUS * RADIUS * baba
    disc = B * B - A * C
    t = np.full(X.shape, np.inf)
    ok = disc >= 0
    tc = np.where(ok, (-B - np.sqrt(np.maximum(disc, 0))) / np.maximum(A, 1e-9), np.inf)
    yb = baoa + tc * bard
    body = ok & (yb > 0) & (yb < baba) & (tc > 0)
    t = np.where(body, tc, t)
    for cap in (a, b):
        oc = eye - cap
        bb = d @ oc
        cc = oc @ oc - RADIUS * RADIUS
        hh = bb * bb - cc
        tcap = np.where(hh >= 0, -bb - np.sqrt(np.maximum(hh, 0)), np.inf)
        tcap = np.where(tcap > 0, tcap, np.inf)
        t = np.minimum(t, tcap)
    hit = np.isfinite(t)
    if not hit.any():
        return colour, depth
    P = eye + d * np.where(hit, t, 0)[..., None]
    axis = a + np.clip(((P - a) @ ba) / baba, 0, 1)[..., None] * ba
    N = P - axis
    N /= np.maximum(np.linalg.norm(N, axis=-1, keepdims=True), 1e-9)
    face = np.array(av["fwd"], np.float64)
    front = (N @ face) > 0
    base = np.where(front[..., None], np.array([1.0, 0.45, 0.7]), np.array([0.35, 0.5, 1.0]))
    height = P[..., 1] - feet[1]
    band = (height > 1.0) & (height < 1.15)
    base = np.where(band[..., None], np.array(band_colour(av.get("params", {}))), base)
    light = 0.45 + 0.55 * np.clip(N @ np.array([0.3, 0.9, 0.2]) / np.linalg.norm([0.3, 0.9, 0.2]), 0, 1)
    rgb = np.clip(base * light[..., None], 0, 1)
    sub_c = colour[y0:y1, x0:x1]
    sub_d = depth[y0:y1, x0:x1]
    sub_c[hit, :3] = (rgb[hit] * 255).astype(np.uint8)
    sub_c[hit, 3] = 255
    sub_d[hit] = (t * (d @ fwd))[hit]
    return colour, depth


async def main():
    shm = Shm()
    state = {"cam": None, "avatar": None}
    print(f"fake Astra engine on ws://127.0.0.1:{PORT}, frames -> {NAME}")

    async def client(ws):
        print("DS linked")
        await ws.send(json.dumps({"t": "hello", "v": 1, "shm": NAME, "maxW": MAX_W, "maxH": MAX_H, "states": ["idle"], "triggers": [], "moods": [],
                                  "params": {"speed": "float", "airborne": "bool"}}))
        fresh = asyncio.Event()

        async def renderer():
            # always the newest camera (older ones are dropped if it ever falls behind)
            frames, t0, last = 0, time.monotonic(), None
            while True:
                await fresh.wait()
                fresh.clear()
                cam, av = state["cam"], state["avatar"]
                if cam is None or av is None or cam is last:
                    continue
                last = cam
                # half of what DS asks for: numpy is slow (the real engine renders the full size)
                w, h = max(64, min(cam["w"], MAX_W) // 2), max(36, min(cam["h"], MAX_H) // 2)
                colour, depth = await asyncio.to_thread(render, w, h, cam, av)
                shm.write(colour, depth, cam)
                frames += 1
                if time.monotonic() - t0 > 5:
                    print(f"{frames / (time.monotonic() - t0):.1f} fps  astra {av.get('params')} at "
                          f"{av['pos'][0]:.1f} {av['pos'][1]:.1f} {av['pos'][2]:.1f}")
                    frames, t0 = 0, time.monotonic()

        task = asyncio.create_task(renderer())
        try:
            async for raw in ws:
                m = json.loads(raw)
                kind = m.get("t")
                if kind == "avatar":
                    state["avatar"] = m
                elif kind == "cam":
                    state["cam"] = m
                    fresh.set()
                elif kind == "param":
                    if state["avatar"] is not None:
                        state["avatar"].setdefault("params", {}).update(m.get("set", {}))
                elif kind == "hello":
                    print("hello from", m.get("client"), m.get("game"))
        finally:
            task.cancel()
        print("DS gone")

    async with websockets.serve(client, "127.0.0.1", PORT, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
