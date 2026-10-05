#pragma once
// DS's sun: the world's SunLight (a Light: colour +0x16C, intensity +0x178, its WorldNode transform +0x20), found once
// by scanning DS's memory for its vtable on a thread of its own (no global points at it), then read every frame.
// At night DS's SunLight is the moon (dimmer, bluer): that is the light there is.

namespace sun
{
	/// Starts the search (on the first present; again by itself when the sun it has stops looking like one).
	void start();
	struct State
	{
		bool valid;
		float toward[3]; // unit, DS world, toward the light
		float color[3];  // as DS has it (linear)
		float intensity;
		float columns[3][3]; // its transform's columns (logged: which one is its direction)
	};
	/// Present thread; SEH-guarded reads.
	State read();
	/// Which of its transform's columns it shines along (1 or 2; the overlay can switch, to check against DS's shadows).
	int &axis();
	/// What the search found so far, for the overlay.
	const char *status();
}
