#pragma once
// Astra (the Astra companion's 3D avatar) walking with Sam. DS owns where she is: she follows a spot beside and
// behind Sam on DS's real collision (rays down on the layer Sam's own feet use for the ground she stands on:
// buildings, structures, bridges, Minecraft blocks' boxes; sphere tests on what stops Sam for walls and ceilings),
// steps up kerbs and rocks, goes round what's too steep or too high, falls when the ground ends, and comes back to
// Sam when she falls too far behind, ends up a floor above or below him, or gets stuck.
// Her pose and pace drive the avatar engine's picture (the engine bridge); until then a debug marker shows her.
#include "game.h"

namespace astra
{
	enum class Gait
	{
		idle,
		walk,
		run,
		fall
	};

	struct State
	{
		bool valid;
		double pos[3];    // DS world, her feet
		float heading;    // radians, DS: 0 along +x, counter-clockwise (+y) positive
		float speed;      // m/s along the ground
		float vel[3];     // m/s, DS world
		float ground_n[3]; // ground normal under her
		Gait gait;
	};

	/// Once per presented frame (on the present thread: a game worker with physics thread resources).
	void frame(const game::Snapshot &snap);
	State state();

	void set_enabled(bool on);
	bool enabled();
	/// Put her next to Sam now (the overlay's "Call Astra").
	void call();

	struct Stats
	{
		unsigned long long steps, rays, spheres;
		int teleports, blocked, falls, refused;
		float last_ms;
	};
	Stats stats();
	const char *gait_name(Gait g);
}
