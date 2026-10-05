#pragma once
// The link to the Astra avatar engine (the Astra companion's 3D avatar, rendered natively next to DS like Minecraft):
// every frame DS's camera and Astra's pose (astra.h: DS decides where she walks) go to the engine over a WebSocket
// on 127.0.0.1; the engine renders her from that camera into shared memory, and compositor.cpp lays her into DS's
// picture against DS's depth, like Minecraft's world.
//
// Coordinates sent: glTF's (right-handed, Y up, metres): DS (x, y, z) -> (x, z, -y), relative to a floating origin
// near Sam (DS's coordinates reach ±10 km; the engine works in f32). Camera angles also in Minecraft's convention
// ("ypr"), echoed back in the frame's slot so the compositor knows which camera a frame was rendered from.
#include "game.h"

namespace astralink
{
	void start();
	void stop();
	/// Once per presented frame, after astra::frame.
	void frame(const game::Snapshot &snap, int backbuffer_w, int backbuffer_h);
	bool connected();

	/// Any message to the engine (animation commands and the like).
	void send_raw(const char *message);

	struct Stats
	{
		unsigned long long sent, received, rebases;
		unsigned long long frames; // Astra frames composited
	};
	Stats stats();
	/// The engine's last event or error, for the overlay.
	const char *last_event();
}
