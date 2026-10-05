#include "astra.h"
#include "log.h"
#include "physics.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <windows.h>

namespace astra
{
	namespace
	{
		// where she walks: beside and a little behind Sam (his right, metres)
		constexpr float kSide = 1.2f, kBack = 1.5f;
		constexpr float kArrive = 0.35f;          // close enough to her spot: stop
		constexpr float kWalkTop = 2.2f;          // m/s: faster than this is a run
		constexpr float kMaxSpeed = 8.5f;         // her sprint (Sam's run is about 5-6)
		constexpr float kAccel = 7.0f, kBrake = 10.0f;
		constexpr float kTurnRate = 7.0f;         // rad/s while moving
		constexpr float kIdleTurnRate = 2.0f;     // rad/s turning to face Sam when standing
		// her body against DS's static collision (layer 47: terrain, rocks, buildings, Minecraft blocks' boxes)
		constexpr float kStepUp = 0.6f;           // kerbs, rocks, stairs, one Minecraft block (no: a block is 1 m)
		constexpr float kStepDown = 1.0f;         // walks down this much; more is a fall
		constexpr float kMaxSlopeCos = 0.66f;     // ~49 degrees: steeper ground is a wall going up
		constexpr float kBodyR = 0.28f, kChest = 1.0f, kHeadR = 0.22f, kHead = 1.45f;
		constexpr float kSubstep = 0.22f;         // longest move between collision checks (thin posts)
		constexpr float kGravity = 9.81f;
		// coming back to Sam
		constexpr float kTooFar = 28.0f, kTooHigh = 10.0f;
		constexpr float kStuckSeconds = 1.6f, kStuckDistance = 3.0f;
		constexpr float kFallLimit = 3.0f;        // seconds falling: she's lost
		constexpr float kFloorApart = 2.2f, kApartSeconds = 2.5f; // metres above or below Sam, for this long

		bool g_enabled = true;
		bool g_callRequested = false;
		State g_state{};
		float g_vz = 0.0f, g_fallTime = 0.0f, g_drawZ = 0.0f;
		float g_samHeading = 0.0f;
		double g_samPrev[3] = {};
		bool g_samPrevValid = false;
		float g_samSpeed = 0.0f;
		float g_stuckTime = 0.0f, g_bestDist = 1e9f, g_apartTime = 0.0f;
		LARGE_INTEGER g_prevTick{}, g_freq{};
		const void *g_ignore = nullptr; // Sam (and his cargo): the ground rays see characters
		Stats g_stats{};

		const float kTwoPi = 6.2831853f;

		float wrap(float a)
		{
			while (a > 3.14159265f)
				a -= kTwoPi;
			while (a < -3.14159265f)
				a += kTwoPi;
			return a;
		}

		float turn_toward(float from, float to, float max_step)
		{
			const float d = wrap(to - from);
			return wrap(from + std::clamp(d, -max_step, max_step));
		}

		struct Ground
		{
			bool called, hit;
			double z;
			float n[3];
		};

		Ground ground_at(double x, double y, double z_from, double z_to)
		{
			Ground g{};
			const double from[3] = {x, y, z_from}, to[3] = {x, y, z_to};
			physics::Hit h;
			++g_stats.rays;
			g.called = physics::intersect_line(from, to, physics::layers().ground, g_ignore, h);
			if (!g.called || !h.hit)
				return g;
			g.hit = true;
			g.z = h.pos[2];
			const float len = std::sqrt(h.normal[0] * h.normal[0] + h.normal[1] * h.normal[1] + h.normal[2] * h.normal[2]);
			for (int i = 0; i < 3; ++i)
				g.n[i] = len > 1e-4f ? h.normal[i] / len : (i == 2 ? 1.0f : 0.0f);
			return g;
		}

		/// Is there room for her body standing with her feet at (x, y, z)? (chest and head spheres)
		bool body_free(double x, double y, double z)
		{
			bool called = false;
			const double chest[3] = {x, y, z + kChest};
			++g_stats.spheres;
			if (physics::intersect_sphere(chest, kBodyR, physics::layers().solid, called) || !called)
				return false;
			const double head[3] = {x, y, z + kHead};
			++g_stats.spheres;
			return !physics::intersect_sphere(head, kHeadR, physics::layers().solid, called) && called;
		}

		/// One short step from her position: where she ends up (ground under the new spot, or the same height in the
		/// air when the ground ends: a fall), or false when a wall, a low ceiling or too steep a slope is in the way.
		bool try_step(const double from[3], float dx, float dy, double out[3], float n[3], bool &airborne)
		{
			const double x = from[0] + dx, y = from[1] + dy;
			const Ground g = ground_at(x, y, from[2] + kStepUp + 0.05, from[2] - kStepDown);
			if (!g.called)
				return false;
			if (!g.hit)
			{
				if (!body_free(x, y, from[2]))
					return false;
				out[0] = x, out[1] = y, out[2] = from[2];
				n[0] = 0, n[1] = 0, n[2] = 1;
				airborne = true;
				return true;
			}
			if (g.n[2] < kMaxSlopeCos && g.z > from[2] + 0.05)
				return false; // too steep to walk up
			if (!body_free(x, y, g.z))
				return false;
			out[0] = x, out[1] = y, out[2] = g.z;
			std::copy(g.n, g.n + 3, n);
			airborne = false;
			return true;
		}

		float sam_heading_from(const game::Snapshot &s)
		{
			const float fx = s.sam.col[1][0], fy = s.sam.col[1][1];
			return (fx * fx + fy * fy) > 1e-6f ? std::atan2(fy, fx) : g_samHeading;
		}

		/// Her spot next to Sam (DS world, Sam's height).
		void spot(const game::Snapshot &s, double out[3])
		{
			const float c = std::cos(g_samHeading), sn = std::sin(g_samHeading);
			// forward (c, sn), right (sn, -c) in DS's right-handed, Z-up world
			out[0] = s.sam.pos[0] + sn * kSide - c * kBack;
			out[1] = s.sam.pos[1] - c * kSide - sn * kBack;
			out[2] = s.sam.pos[2];
		}

		/// Puts her at her spot next to Sam (or another free place around him), on the ground.
		void teleport(const game::Snapshot &s, const char *why)
		{
			const float offsets[][2] = {{kSide, -kBack}, {-kSide, -kBack}, {0.0f, -2.0f}, {kSide * 1.5f, 0.0f}, {-kSide * 1.5f, 0.0f}, {0.0f, 1.8f}};
			const float c = std::cos(g_samHeading), sn = std::sin(g_samHeading);
			double best[3] = {s.sam.pos[0] - c * 0.8, s.sam.pos[1] - sn * 0.8, s.sam.pos[2]};
			float n[3] = {0, 0, 1};
			for (const auto &o : offsets)
			{
				const double x = s.sam.pos[0] + sn * o[0] + c * o[1], y = s.sam.pos[1] - c * o[0] + sn * o[1];
				const Ground g = ground_at(x, y, s.sam.pos[2] + 1.5, s.sam.pos[2] - 3.0);
				if (!g.called)
					return; // no physics on this thread: next frame
				if (g.hit && std::fabs(g.z - s.sam.pos[2]) < 1.2 && body_free(x, y, g.z))
				{
					best[0] = x, best[1] = y, best[2] = g.z;
					std::copy(g.n, g.n + 3, n);
					break;
				}
			}
			const bool first = !g_state.valid;
			g_state.valid = true;
			std::copy(best, best + 3, g_state.pos);
			std::copy(n, n + 3, g_state.ground_n);
			g_state.heading = g_samHeading;
			g_state.speed = 0.0f;
			g_state.vel[0] = g_state.vel[1] = g_state.vel[2] = 0.0f;
			g_state.gait = Gait::idle;
			g_drawZ = float(best[2]);
			g_vz = 0.0f;
			g_fallTime = 0.0f;
			g_stuckTime = 0.0f;
			g_bestDist = 1e9f;
			if (!first)
				++g_stats.teleports;
			logf("ASTRA %s: at %.2f %.2f %.2f (Sam %.2f %.2f %.2f)", why, best[0], best[1], best[2], s.sam.pos[0], s.sam.pos[1], s.sam.pos[2]);
		}
	}

	namespace
	{
		// ---- riding with Sam ----
		// her hips' point from Sam's root mirrored: measured in the truck, Sam's root is on the seat (1.55 m up)
		float g_seatLift = 0.12f, g_seatForward = 0.12f;
		// DS's vehicles (vtable rvas, from their RTTI: DSVehicleTruck, DSVehicleMotorbike, DSVehicleSpeedCar)
		enum class Ride
		{
			none,
			truck,
			trike,
			roadster
		};
		struct Vehicle
		{
			void *entity;
			Ride kind;
			game::WorldTransform t;
		};
		constexpr uintptr_t kTruckVtbl = 0x3e26790, kTrikeVtbl = 0x3e25db8, kRoadsterVtbl = 0x3e274d8;
		constexpr int kRideConfirm = 3, kRideLose = 6; // probes in a row (every third frame)

		Ride ride_kind(void *entity)
		{
			// (inside guard::run: a stale pointer is contained)
			const uintptr_t vt = *reinterpret_cast<const uintptr_t *>(entity) - uintptr_t(GetModuleHandleW(nullptr));
			return vt == kTruckVtbl ? Ride::truck : vt == kTrikeVtbl ? Ride::trike : vt == kRoadsterVtbl ? Ride::roadster : Ride::none;
		}

		const char *ride_name(Ride r)
		{
			return r == Ride::truck ? "truck" : r == Ride::trike ? "trike" : r == Ride::roadster ? "roadster" : "";
		}

		/// The vehicle Sam sits in: short rays down through him (Sam himself skipped) that hit a vehicle, with Sam
		/// inside it (under a truck's roof, on a trike's or a roadster's seat), not standing on it or next to it.
		bool vehicle_under_sam(const game::Snapshot &s, Vehicle &out)
		{
			const void *sam = reinterpret_cast<const void *>(s.sam_ptr);
			for (const uint32_t layer : {physics::kLayerHumanoidRaycast, 12u, physics::kLayerPlayerLegIK})
				for (const double above : {1.0, 2.0, 3.0})
				{
					const double from[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] + above}, to[3] = {s.sam.pos[0], s.sam.pos[1], s.sam.pos[2] - 1.5};
					physics::Hit h;
					++g_stats.rays;
					if (!physics::intersect_line(from, to, layer, sam, h, false) || !h.hit || h.entity == nullptr)
						continue;
					const Ride kind = ride_kind(h.entity);
					if (kind == Ride::none)
						continue;
					const double dz = h.pos[2] - s.sam.pos[2];
					// a truck's roof over him, or the seat he sits on; standing on the truck's bed is not riding
					const bool inside = kind == Ride::truck ? dz > 0.6 : dz > -1.2 && dz < 1.2;
					if (!inside)
						continue;
					out.entity = h.entity;
					out.kind = kind;
					std::memcpy(&out.t, static_cast<const char *>(h.entity) + 200, sizeof(out.t)); // Entity::Orientation
					if (!std::isfinite(out.t.pos[0]) || !std::isfinite(out.t.pos[2]))
						return false;
					// Sam inside it, in its seat, not climbing in beside it (a truck's side overhangs him then)
					const double d[3] = {s.sam.pos[0] - out.t.pos[0], s.sam.pos[1] - out.t.pos[1], s.sam.pos[2] - out.t.pos[2]};
					const double right = d[0] * out.t.col[0][0] + d[1] * out.t.col[0][1] + d[2] * out.t.col[0][2];
					const double up = d[0] * out.t.col[2][0] + d[1] * out.t.col[2][1] + d[2] * out.t.col[2][2];
					const double half_width = kind == Ride::truck ? 1.25 : kind == Ride::roadster ? 1.0 : 0.5;
					const double min_up = kind == Ride::truck ? 0.8 : 0.2;
					return std::fabs(right) < half_width && up > min_up;
				}
			return false;
		}

		/// Sam's place in the vehicle -> hers: his seat mirrored across the vehicle (the passenger seat), or behind
		/// him on a trike. In the vehicle's frame (its columns: right, forward, up), DS world out.
		void seat_for_astra(const Vehicle &v, const double sam[3], double out[3])
		{
			const double d[3] = {sam[0] - v.t.pos[0], sam[1] - v.t.pos[1], sam[2] - v.t.pos[2]};
			double local[3];
			for (int i = 0; i < 3; ++i)
				local[i] = d[0] * v.t.col[i][0] + d[1] * v.t.col[i][1] + d[2] * v.t.col[i][2];
			if (v.kind == Ride::trike)
			{
				local[1] -= 0.55; // the pillion, behind Sam
				local[2] += 0.05;
			}
			else
				local[0] = -local[0];
			// her hips' point on the seat, from Sam's root there (tunable in the overlay)
			local[1] += g_seatForward;
			local[2] += g_seatLift;
			for (int i = 0; i < 3; ++i)
				out[i] = v.t.pos[i] + local[0] * v.t.col[0][i] + local[1] * v.t.col[1][i] + local[2] * v.t.col[2][i];
		}

		Vehicle g_vehicle{};
		Ride g_riding = Ride::none;
		int g_rideHits = 0, g_rideMisses = 0;
		unsigned g_rideFrame = 0;
	}

	void frame(const game::Snapshot &s)
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (g_freq.QuadPart == 0)
			QueryPerformanceFrequency(&g_freq);
		const float dt = g_prevTick.QuadPart ? std::clamp(float(now.QuadPart - g_prevTick.QuadPart) / float(g_freq.QuadPart), 0.0f, 0.1f) : 0.0f;
		g_prevTick = now;
		if (!g_enabled || !s.ok)
			return;
		if (physics::thread_slot() == 0)
		{
			++g_stats.refused; // this present ran on a thread without physics resources: next frame
			return;
		}
		const auto t0 = std::chrono::steady_clock::now();
		g_ignore = reinterpret_cast<const void *>(s.sam_ptr);

		// Sam: his pace and facing (his movement direction while he moves, his body's facing when he stands)
		if (g_samPrevValid && dt > 0.0f)
		{
			const double dx = s.sam.pos[0] - g_samPrev[0], dy = s.sam.pos[1] - g_samPrev[1];
			const float v = float(std::sqrt(dx * dx + dy * dy)) / dt;
			g_samSpeed += (std::min(v, 30.0f) - g_samSpeed) * std::min(1.0f, dt * 6.0f);
			g_samHeading = g_samSpeed > 1.0f && v > 0.5f ? turn_toward(g_samHeading, std::atan2(float(dy), float(dx)), dt * 4.0f)
														   : turn_toward(g_samHeading, sam_heading_from(s), dt * 2.0f);
		}
		else
			g_samHeading = sam_heading_from(s);
		std::copy(s.sam.pos, s.sam.pos + 3, g_samPrev);
		g_samPrevValid = true;

		State &a = g_state;

		// riding with Sam: look for his vehicle every third frame; a few probes in a row decide
		if ((++g_rideFrame % 3) == 0)
		{
			Vehicle v{};
			if (vehicle_under_sam(s, v))
			{
				g_rideMisses = 0;
				if (g_riding == Ride::none && ++g_rideHits >= kRideConfirm)
				{
					g_riding = v.kind;
					++g_stats.rides;
					// where Sam's root sits in the vehicle (its frame: right, forward, up) and the ground under it: to
					// place her hips on the seat
					const double d[3] = {s.sam.pos[0] - v.t.pos[0], s.sam.pos[1] - v.t.pos[1], s.sam.pos[2] - v.t.pos[2]};
					double l[3];
					for (int i = 0; i < 3; ++i)
						l[i] = d[0] * v.t.col[i][0] + d[1] * v.t.col[i][1] + d[2] * v.t.col[i][2];
					const double from[3] = {v.t.pos[0], v.t.pos[1], v.t.pos[2] + 0.5}, to[3] = {v.t.pos[0], v.t.pos[1], v.t.pos[2] - 4.0};
					physics::Hit gh;
					const bool ground = physics::intersect_line(from, to, physics::kLayerRayVsStatic, nullptr, gh) && gh.hit;
					logf("ASTRA rides with Sam: %s %p; Sam in it at right %+.2f forward %+.2f up %+.2f; vehicle origin %.2f above the ground",
						ride_name(v.kind), v.entity, l[0], l[1], l[2], ground ? v.t.pos[2] - gh.pos[2] : -1.0);
				}
				g_vehicle = v;
			}
			else
			{
				g_rideHits = 0;
				if (g_riding != Ride::none && ++g_rideMisses >= kRideLose)
				{
					// Sam got out: she gets out on her side and walks again
					logf("ASTRA gets out of the %s", ride_name(g_riding));
					g_riding = Ride::none;
					a.seated = false;
					teleport(s, "gets out");
					return;
				}
			}
		}
		if (g_riding != Ride::none && a.valid)
		{
			// the vehicle moves every frame: its transform now, her seat in it
			game::WorldTransform t;
			std::memcpy(&t, static_cast<const char *>(g_vehicle.entity) + 200, sizeof(t));
			if (std::isfinite(t.pos[0]))
				g_vehicle.t = t;
			double seat[3];
			seat_for_astra(g_vehicle, s.sam.pos, seat);
			if (dt > 0.0f)
				for (int i = 0; i < 3; ++i)
					a.vel[i] = float(seat[i] - a.pos[i]) / dt;
			std::copy(seat, seat + 3, a.pos);
			a.heading = std::atan2(g_vehicle.t.col[1][1], g_vehicle.t.col[1][0]);
			for (int i = 0; i < 3; ++i)
			{
				a.fwd[i] = g_vehicle.t.col[1][i];
				a.up[i] = g_vehicle.t.col[2][i];
			}
			a.speed = 0.0f;
			a.gait = Gait::idle;
			a.seated = true;
			a.ground_n[0] = g_vehicle.t.col[2][0], a.ground_n[1] = g_vehicle.t.col[2][1], a.ground_n[2] = g_vehicle.t.col[2][2];
			g_drawZ = float(a.pos[2]);
			g_stuckTime = 0.0f;
			g_apartTime = 0.0f;
			g_bestDist = 1e9f;
			g_stats.last_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
			return;
		}

		const double toSamX = s.sam.pos[0] - a.pos[0], toSamY = s.sam.pos[1] - a.pos[1];
		const float samDist = float(std::sqrt(toSamX * toSamX + toSamY * toSamY));
		if (g_callRequested || !a.valid)
		{
			teleport(s, g_callRequested ? "called" : "appears");
			g_callRequested = false;
			return;
		}
		if (samDist > kTooFar || std::fabs(s.sam.pos[2] - a.pos[2]) > kTooHigh)
		{
			teleport(s, "catches up");
			return;
		}
		// a floor apart (Sam up on a structure, she below it, or the other way round) and not getting closer
		if (a.gait != Gait::fall && std::fabs(s.sam.pos[2] - a.pos[2]) > kFloorApart)
		{
			g_apartTime += dt;
			if (g_apartTime > kApartSeconds)
			{
				g_apartTime = 0.0f;
				teleport(s, "was a floor apart");
				return;
			}
		}
		else
			g_apartTime = 0.0f;

		double target[3];
		spot(s, target);
		float tx = float(target[0] - a.pos[0]), ty = float(target[1] - a.pos[1]);
		const float dist = std::sqrt(tx * tx + ty * ty);

		// stuck: no progress toward her spot for a while although it's far
		if (dist < g_bestDist - 0.3f)
		{
			g_bestDist = dist;
			g_stuckTime = 0.0f;
		}
		else if (dist > kStuckDistance)
		{
			g_stuckTime += dt;
			if (g_stuckTime > kStuckSeconds)
			{
				teleport(s, "was stuck");
				return;
			}
		}
		else
			g_bestDist = dist, g_stuckTime = 0.0f;

		// falling: gravity until she lands on something
		if (a.gait == Gait::fall)
		{
			g_vz -= kGravity * dt;
			g_fallTime += dt;
			const double z_next = a.pos[2] + g_vz * dt;
			const Ground g = ground_at(a.pos[0] + a.vel[0] * dt, a.pos[1] + a.vel[1] * dt, a.pos[2] + 0.3, z_next - 0.05);
			if (!g.called)
				return;
			a.pos[0] += a.vel[0] * dt;
			a.pos[1] += a.vel[1] * dt;
			if (g.hit)
			{
				a.pos[2] = g.z;
				std::copy(g.n, g.n + 3, a.ground_n);
				a.gait = Gait::idle;
				a.speed *= 0.3f;
				g_vz = 0.0f;
				g_fallTime = 0.0f;
				logf("ASTRA lands at %.2f %.2f %.2f", a.pos[0], a.pos[1], a.pos[2]);
			}
			else
				a.pos[2] = z_next;
			a.vel[2] = g_vz;
			g_drawZ = float(a.pos[2]);
			if (g_fallTime > kFallLimit)
				teleport(s, "fell too far");
			return;
		}

		// where to head: her spot, kept clear of Sam himself (layer 47 doesn't see characters)
		float dirX = dist > 1e-3f ? tx / dist : 0.0f, dirY = dist > 1e-3f ? ty / dist : 0.0f;
		if (samDist < 1.0f && samDist > 1e-3f)
		{
			const float push = (1.0f - samDist) * 1.5f;
			dirX -= float(toSamX) / samDist * push;
			dirY -= float(toSamY) / samDist * push;
			const float l = std::sqrt(dirX * dirX + dirY * dirY);
			if (l > 1e-3f)
				dirX /= l, dirY /= l;
		}

		// pace: Sam's plus what closes the gap, braking into her spot
		float want = dist < kArrive ? 0.0f : std::min(kMaxSpeed, g_samSpeed * 0.9f + (dist - kArrive) * 1.4f);
		if (want > 0.0f)
			want = std::max(want, 0.7f);
		// turn first: she walks the way she faces, slower while she's still turning round
		if (want > 0.0f)
		{
			a.heading = turn_toward(a.heading, std::atan2(dirY, dirX), kTurnRate * dt);
			const float off = std::fabs(wrap(std::atan2(dirY, dirX) - a.heading));
			want *= std::clamp(std::cos(off), 0.0f, 1.0f);
		}
		else if (samDist > 0.5f)
			a.heading = turn_toward(a.heading, std::atan2(float(toSamY), float(toSamX)), kIdleTurnRate * dt);
		a.speed = want > a.speed ? std::min(want, a.speed + kAccel * dt) : std::max(want, a.speed - kBrake * dt);

		// move along her facing in short steps, sliding along walls
		float left = a.speed * dt;
		const float hx = std::cos(a.heading), hy = std::sin(a.heading);
		double pos[3] = {a.pos[0], a.pos[1], a.pos[2]};
		bool airborne = false, blocked = false;
		while (left > 1e-4f && !airborne)
		{
			const float step = std::min(left, kSubstep);
			double next[3];
			float n[3];
			bool ok = try_step(pos, hx * step, hy * step, next, n, airborne);
			if (!ok)
			{
				// slide: the nearest turn either side that is free, preferring the side toward her spot
				const float side = (hx * dirY - hy * dirX) >= 0.0f ? 1.0f : -1.0f;
				for (float ang : {0.6f, -0.6f, 1.2f, -1.2f, 1.6f, -1.6f})
				{
					const float r = ang * side, c = std::cos(r), sn = std::sin(r);
					const float sx = hx * c - hy * sn, sy = hx * sn + hy * c;
					if ((ok = try_step(pos, sx * step * 0.7f, sy * step * 0.7f, next, n, airborne)))
						break;
				}
			}
			if (!ok)
			{
				blocked = true;
				break;
			}
			std::copy(next, next + 3, pos);
			std::copy(n, n + 3, a.ground_n);
			left -= step;
			++g_stats.steps;
		}
		if (blocked)
		{
			++g_stats.blocked;
			a.speed *= 0.5f;
		}
		if (dt > 0.0f)
		{
			a.vel[0] = float(pos[0] - a.pos[0]) / dt;
			a.vel[1] = float(pos[1] - a.pos[1]) / dt;
			a.vel[2] = float(pos[2] - a.pos[2]) / dt;
		}
		std::copy(pos, pos + 3, a.pos);
		if (airborne)
		{
			a.gait = Gait::fall;
			g_vz = 0.0f;
			g_fallTime = 0.0f;
			++g_stats.falls;
		}
		else
		{
			const float ground_speed = std::sqrt(a.vel[0] * a.vel[0] + a.vel[1] * a.vel[1]);
			a.gait = ground_speed < 0.15f ? Gait::idle : ground_speed < kWalkTop ? Gait::walk : Gait::run;
		}
		// stepping up or down: the drawn height follows quickly, not in one jump
		g_drawZ += (float(a.pos[2]) - g_drawZ) * std::min(1.0f, dt * 14.0f);
		if (std::fabs(float(a.pos[2]) - g_drawZ) > 1.5f)
			g_drawZ = float(a.pos[2]);
		g_stats.last_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	State state()
	{
		State s = g_state;
		if (!s.seated)
		{
			// on foot she stands upright, facing her heading
			s.fwd[0] = std::cos(s.heading), s.fwd[1] = std::sin(s.heading), s.fwd[2] = 0.0f;
			s.up[0] = 0.0f, s.up[1] = 0.0f, s.up[2] = 1.0f;
		}
		if (s.valid && s.gait != Gait::fall)
			s.pos[2] = g_drawZ;
		return s;
	}

	void set_enabled(bool on)
	{
		g_enabled = on;
		if (!on)
			g_state.valid = false; // reappears next to Sam when switched back on
		logf("astra %s", on ? "on" : "off");
	}

	bool enabled()
	{
		return g_enabled;
	}

	void seat_offset(float &lift, float &forward, bool set)
	{
		if (set)
			g_seatLift = lift, g_seatForward = forward;
		lift = g_seatLift;
		forward = g_seatForward;
	}

	void call()
	{
		g_callRequested = true;
	}

	Stats stats()
	{
		Stats st = g_stats;
		st.vehicle = ride_name(g_riding);
		return st;
	}

	const char *gait_name(Gait g)
	{
		switch (g)
		{
		case Gait::idle:
			return "idle";
		case Gait::walk:
			return "walk";
		case Gait::run:
			return "run";
		case Gait::fall:
			return "fall";
		}
		return "?";
	}
}
