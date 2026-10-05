#pragma once
// Astra (the Astra companion's 3D avatar) walking with Sam. DS owns where she is: she follows a spot beside and
// behind Sam on DS's real collision (rays down on the layer Sam's own feet use for the ground she stands on:
// buildings, structures, bridges, Minecraft blocks' boxes; sphere tests on what stops Sam for walls and ceilings),
// steps up kerbs and rocks, goes round what's too steep or too high, falls when the ground ends, and comes back to
// Sam when she falls too far behind, ends up a floor above or below him, or gets stuck. While Sam stands (or turns on
// the spot) she stays where she is near him and now and then strolls a little. When Sam drives, she rides
// along: in his truck's or car's passenger seat (his seat mirrored across the vehicle), or behind him on a trike.
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
		bool seated;       // riding with Sam: the passenger seat of his truck or car, behind him on a trike
		float fwd[3], up[3]; // her full orientation, DS world: the vehicle's while she rides (tilted on slopes), upright on foot
	};

	/// Once per presented frame (on the present thread: a game worker with physics thread resources).
	void frame(const game::Snapshot &snap);
	State state();

	void set_enabled(bool on);
	bool enabled();
	/// Put her next to Sam now (the overlay's "Call Astra").
	void call();
	/// How much of her is in direct light from `toward` (DS world, unit): rays from her knees, chest and head toward
	/// it through DS's collision (rocks, buildings, a truck's roof). 0..1. Present thread, under guard.
	float sunlit(const float toward[3], const void *ignore);
	/// Her size (1: the model as made); the engine scales her, her body here and her calm pace scale along.
	float scale();
	void set_scale(float s);
	/// Her hips' point on a vehicle seat relative to Sam's root mirrored (metres up, forward): read, or set.
	void seat_offset(float &lift, float &forward, bool set);

	struct Stats
	{
		unsigned long long steps, rays, spheres;
		int teleports, blocked, falls, refused, rides;
		float last_ms;
		const char *vehicle; // what Sam rides now ("" on foot)
	};
	Stats stats();
	const char *gait_name(Gait g);
}
