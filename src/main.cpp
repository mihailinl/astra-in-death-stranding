// Astra in Death Stranding: the game's part of Astra, the Astra companion's avatar, in Death Stranding Director's Cut
// and Death Stranding 2 - a PROVIDER add-on (astra-ds.addon64) for the Astra foundation (astra-reshade's
// astra.addon64, extern/astra-reshade), which ReShade loads beside it and which composites her into DS's picture.
// This add-on decides what only DS can: where she is and what DS's camera and sun are.
//   astra.cpp       where she walks: follows Sam on DS's real collision, rides in his vehicles
//   sun.cpp         DS's sun (or moon), found in its memory
//   foundation.cpp  the foundation's provider API, looked up in its module
//   main.cpp        once per presented frame, from the foundation's present callback: DS's camera, her placement,
//                   her animator parameters and the sun, handed to the foundation
// Shared with the Minecraft mod (extern/minecraft-in-death-stranding): DS's camera and Sam (game), DS's collision
// (physics), fault containment (guard) and DS's UI over what add-ons draw (ui).
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "astra.h"
#include "foundation.h"
#include "game.h"
#include "guard.h"
#include "log.h"
#include "physics.h"
#include "sun.h"
#include "ui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "Astra in Death Stranding";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Astra, the Astra companion's avatar, walks with Sam in Death Stranding (Director's Cut) and Death Stranding 2 (needs astra.addon64, the "
	"Astra foundation).";

namespace
{
	bool g_resolved = false;
	char g_status[256] = "not resolved";
	char g_physStatus[160] = "physics: not resolved";
	game::Snapshot g_snap;
	ULONGLONG g_readPausedUntil = 0;
	bool g_followBroken = false;
	sun::State g_sun{};
	float g_lit = 1.0f;
	std::atomic<bool> g_linked{false}; // the foundation was found and calls per_frame at every Present

	// the log: the foundation's (astra-reshade.log, beside Astra's settings for DS); lines from before it was found wait
	std::mutex g_logLock;
	std::vector<std::string> g_early;

	/// The camera read retries after a fault (pointers are briefly stale while the game loads)
	void read_snapshot()
	{
		if (GetTickCount64() < g_readPausedUntil)
		{
			g_snap.ok = false;
			return;
		}
		if (!guard::run("camera read", [] { game::read(g_snap); }))
		{
			g_snap.ok = false;
			g_readPausedUntil = GetTickCount64() + 2000;
		}
	}

	// Death Stranding 2: once per game frame on its update thread (physics::install_tick): her walking and how much of
	// her is in the sun, from a snapshot of its own
	game::Snapshot g_tickSnap;
	sun::State g_tickSun{};
	std::atomic<float> g_tickLit{1.0f};
	ULONGLONG g_tickPausedUntil = 0;

	void game_tick()
	{
		if (!g_resolved || g_followBroken || GetTickCount64() < g_tickPausedUntil)
			return;
		if (!guard::run("game tick: camera read", [] { game::read(g_tickSnap); }) || !g_tickSnap.ok)
		{
			g_tickPausedUntil = GetTickCount64() + 2000;
			return;
		}
		if (!guard::run("Astra follows Sam", [] { astra::frame(g_tickSnap); }))
		{
			g_followBroken = true;
			return;
		}
		const sun::State sunst = g_tickSun;
		if (sunst.valid)
		{
			float lit = 1.0f;
			guard::run("Astra in the sun", [&] { lit = astra::sunlit(sunst.toward, reinterpret_cast<const void *>(g_tickSnap.sam_ptr)); });
			g_tickLit = lit;
		}
	}

	// ---- DS -> the foundation (glTF: right-handed, Y up, metres; DS: Z up) ----------------------------------------

	void to_gl(const double ds[3], double out[3])
	{
		out[0] = ds[0];
		out[1] = ds[2];
		out[2] = -ds[1];
	}

	void dir_to_gl(const float ds[3], float out[3])
	{
		out[0] = ds[0];
		out[1] = ds[2];
		out[2] = -ds[1];
	}

	constexpr float kNoonSun = 2.1e6f;              // DS's daylight sun: intensity 2.0e6 x colour 1.05 (measured, afternoon, clear)
	constexpr float kAirborneAfter = 0.18f;         // seconds falling before she's "in the air" for the animations
	constexpr unsigned long long kSitDownMs = 1300; // the engine's seat_in clip
	unsigned long long g_frame = 0;
	ULONGLONG g_paramTick = 0;
	double g_prevPos[3] = {};
	float g_speed = 0.0f, g_airTime = 0.0f, g_scale = 0.0f;
	bool g_seated = false, g_hips = false;
	unsigned long long g_seatedAt = 0;

	/// Her placement and her animator parameters: raw facts, but steady - what they mean (walk, run, a chair) is her
	/// animation set's call.
	void send_placement(const foundation::Api &f, const astra::State &a)
	{
		double feet[3];
		to_gl(a.pos, feet);
		float face[3], her_up[3], vel[3];
		dir_to_gl(a.fwd, face);
		dir_to_gl(a.up, her_up);
		dir_to_gl(a.vel, vel);
		// her pace over the ground smoothed (per-frame displacement is noisy, and a jump back to Sam isn't a sprint), and
		// "in the air" only after a moment off the ground (a one-frame drop on rough ground would be a hop and a landing)
		const ULONGLONG now = GetTickCount64();
		const float dt = g_paramTick ? std::min(float(now - g_paramTick) * 0.001f, 0.1f) : 0.0f;
		g_paramTick = now;
		const double jump = std::hypot(a.pos[0] - g_prevPos[0], a.pos[1] - g_prevPos[1]);
		std::copy(a.pos, a.pos + 3, g_prevPos);
		const float raw_speed = std::sqrt(a.vel[0] * a.vel[0] + a.vel[1] * a.vel[1]);
		if (jump > 2.0 || a.seated)
			g_speed = 0.0f;
		else
			g_speed += (std::min(raw_speed, 12.0f) - g_speed) * std::min(1.0f, dt * 10.0f);
		const float ground_speed = g_speed < 0.05f ? 0.0f : g_speed;
		g_airTime = a.gait == astra::Gait::fall ? g_airTime + dt : 0.0f;
		const bool airborne = g_airTime > kAirborneAfter;
		// her size eases to the slider's (the engine applies it at once)
		g_scale = g_scale <= 0.0f ? astra::scale() : g_scale + (astra::scale() - g_scale) * std::min(1.0f, dt * 4.0f);

		// riding with Sam: she sits down on a chair-like seat ("seat": the engine's sitting-on-a-chair state) and stands up
		// again ("unseat": its stand-up clip, then the graph's own states) when he gets out
		if (a.seated != g_seated)
		{
			g_seated = a.seated;
			g_seatedAt = now;
			if (a.seated)
				f.cue("play", "seat");
			else
				f.cue("trigger", "unseat");
		}
		// sitting down: her feet stay where they are while the sit-down clip plays, then her hips go onto the seat (the
		// engine eases between the two); seated, her pos is the seat
		const bool hips = a.seated && now - g_seatedAt > kSitDownMs;
		if (hips != g_hips)
		{
			g_hips = hips;
			f.anchor(hips ? "hips" : "feet");
		}
		f.param("vspeed", a.vel[2]);
		f.param("seated", a.seated ? 1.0f : 0.0f);
		f.avatar(feet, face, her_up, vel, a.seated ? 0.0f : ground_speed, airborne, g_scale);
	}

	/// Once per Present, called by the foundation before it reads anything: DS's camera read now IS the frame being
	/// presented. Present thread.
	void per_frame(void *)
	{
		const foundation::Api *f = foundation::api();
		if (f == nullptr)
			return;
		read_snapshot();
		// her walking calls into the game: a fault switches it off instead of letting DS's crash handler end the game.
		// Death Stranding 2 answers physics queries only on its own update thread: there it runs in game_tick()
		const bool ds2 = game::version() == 2;
		if (ds2)
		{
			static bool hooked = false;
			if (!hooked)
			{
				hooked = true;
				logf("DS2 game tick hook: %s", physics::install_tick(game_tick) ? "installed" : "FAILED (code mismatch)");
			}
		}
		else if (!g_followBroken && !guard::run("Astra follows Sam", [] { astra::frame(g_snap); }))
			g_followBroken = true;

		// DS's sun (or moon): her light in the engine, and her shadow on DS's ground (how much of her is in it)
		g_sun = sun::read();
		g_tickSun = g_sun;
		float lit = ds2 ? g_tickLit.load() : 1.0f;
		if (!ds2 && g_sun.valid && g_snap.ok && !g_followBroken)
			guard::run("Astra in the sun", [&] { lit = astra::sunlit(g_sun.toward, reinterpret_cast<const void *>(g_snap.sam_ptr)); });
		g_lit += (lit - g_lit) * 0.15f;

		const astra::State a = astra::state();
		const bool live = g_snap.ok && a.valid && astra::enabled();
		f->visible(live);
		if (!live)
			return;
		send_placement(*f, a);
		if (g_sun.valid)
		{
			float toward[3];
			dir_to_gl(g_sun.toward, toward);
			f->light(toward, g_sun.color, g_sun.intensity / kNoonSun, g_lit); // intensity relative to DS's clear noon
		}
		// DS's camera last (a habit of the protocol: the engine draws when a camera comes): Col1 looks, Col2 is up;
		// FieldOfView is vertical; its far plane only when it is one
		double eye[3];
		float fwd[3], up[3];
		to_gl(g_snap.cam.pos, eye);
		dir_to_gl(g_snap.cam.col[1], fwd);
		dir_to_gl(g_snap.cam.col[2], up);
		const float near_m = g_snap.near_plane > 0.0f ? g_snap.near_plane : 0.0f;
		const float far_m = near_m > 0.0f && g_snap.far_plane > near_m ? g_snap.far_plane : 0.0f;
		f->camera2(++g_frame, eye, fwd, up, g_snap.fov, 0, 0.0f, near_m, far_m);

		static ULONGLONG next_sun_log = 0;
		if (g_sun.valid && GetTickCount64() >= next_sun_log)
		{
			next_sun_log = GetTickCount64() + 10000;
			const auto &c = g_sun.columns;
			logf("SUN intensity %.4g colour %.3f %.3f %.3f lit %.2f | c0 %+.3f %+.3f %+.3f | c1 %+.3f %+.3f %+.3f | c2 %+.3f %+.3f %+.3f | cam fwd %+.3f %+.3f %+.3f",
				g_sun.intensity, g_sun.color[0], g_sun.color[1], g_sun.color[2], g_lit, c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2], c[2][0], c[2][1],
				c[2][2], g_snap.cam.col[1][0], g_snap.cam.col[1][1], g_snap.cam.col[1][2]);
		}
	}

	/// A device was destroyed. ReShade loads and unloads its add-ons with every D3D device - DS2 does this once at
	/// every start-up (a probe device, dropped, then the real one), DS1 on an alt-tab or a resolution change - so
	/// the compositor (astra.addon64) may be a DIFFERENT instance by the time the next frame presents, or gone for
	/// a moment. Dropping the link here (not re-resolving it: foundation::find() does that, on the present thread,
	/// against whatever module is actually loaded then) re-arms on_present's one-time registration instead of
	/// leaving it believing a stale instance is still good.
	void on_destroy_device(device *)
	{
		g_linked = false;
	}

	/// This add-on's own Present: only to find the foundation (ReShade may load it after this one) and say who
	/// provides; from then on the foundation calls per_frame at the start of every Present.
	void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
	{
		if (g_linked || !g_resolved || !foundation::find())
			return;
		const foundation::Api *f = foundation::api();
		{
			std::lock_guard<std::mutex> lock(g_logLock);
			for (const std::string &line : g_early)
				f->log(line.c_str());
			g_early.clear();
			g_linked = true;
		}
		if (game::version() == 2)
			f->identity("astra.death-stranding-2", "Death Stranding 2: On the Beach");
		else
			f->identity("astra.death-stranding", "Death Stranding Director's Cut");
		f->on_present(per_frame, nullptr);
		logf("linked: %s", foundation::status());
	}

	/// Minecraft in Death Stranding (MCPassthrough.fx, when installed) leaves its coverage and depth in McInfoTex -
	/// the foundation's occluder texture in DS (shaders/AstraGame.fxh): she hides behind its blocks while it composited.
	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		bool mc = false;
		if (const effect_uniform_variable v = runtime->find_uniform_variable("MCPassthrough.fx", "McActive"); v.handle != 0)
			runtime->get_uniform_value_bool(v, &mc, 1);
		if (const effect_uniform_variable v = runtime->find_uniform_variable("AstraPassthrough.fx", "AstraOccluderActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, mc);
	}

	void on_overlay(effect_runtime *)
	{
		ImGui::TextUnformatted(g_status);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", g_physStatus);
		if (g_linked)
			ImGui::TextDisabled("%s (its own Astra tab: her engine, her picture)", foundation::status());
		else
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", foundation::status());
		if (g_followBroken)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Her walking hit a fault and was switched off (see astra-reshade.log GUARD)");
		bool on = astra::enabled();
		if (ImGui::Checkbox("Astra follows Sam", &on))
			astra::set_enabled(on);
		ImGui::SameLine();
		if (ImGui::Button("Call Astra"))
			astra::call();
		const astra::State a = astra::state();
		const astra::Stats as = astra::stats();
		ImGui::Text("%s%s%s  %.1f m/s  steps %llu  blocked %d  falls %d  catch-ups %d  rides %d  %.2f ms", a.valid ? astra::gait_name(a.gait) : "-",
			as.vehicle[0] ? ", riding in the " : "", as.vehicle, a.speed, as.steps, as.blocked, as.falls, as.teleports, as.rides, as.last_ms);
		float size = astra::scale();
		ImGui::SetNextItemWidth(160);
		if (ImGui::SliderFloat("size", &size, 0.7f, 1.6f, "x%.2f"))
			astra::set_scale(size);
		ImGui::SameLine();
		float lift, forward;
		astra::seat_offset(lift, forward, false);
		ImGui::SetNextItemWidth(160);
		bool seat = ImGui::SliderFloat("seat height", &lift, -1.0f, 1.0f, "%.2f m");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		seat |= ImGui::SliderFloat("seat forward", &forward, -1.0f, 1.0f, "%.2f m");
		if (seat)
			astra::seat_offset(lift, forward, true);
		ImGui::TextDisabled("%s", sun::status());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(80);
		ImGui::SliderInt("sun axis", &sun::axis(), 1, 2);
		if (g_sun.valid)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("toward %+.2f %+.2f %+.2f  colour %.2f %.2f %.2f  intensity %.3g  she's lit %.0f%%", g_sun.toward[0], g_sun.toward[1],
				g_sun.toward[2], g_sun.color[0], g_sun.color[1], g_sun.color[2], g_sun.intensity, g_lit * 100.0f);
		}
		ui::overlay();
		if (g_snap.ok)
			ImGui::TextDisabled("Sam %.2f %.2f %.2f   her %.2f %.2f %.2f", g_snap.sam.pos[0], g_snap.sam.pos[1], g_snap.sam.pos[2], a.pos[0], a.pos[1], a.pos[2]);
		else
			ImGui::TextDisabled("snapshot: invalid (menu / loading?)");
	}
}

/// The shared code's log (log.h): into the foundation's log once it is found; the first lines wait until then.
void logf(const char *fmt, ...)
{
	char line[512];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	{
		std::lock_guard<std::mutex> lock(g_logLock);
		if (!g_linked)
		{
			if (g_early.size() < 100)
				g_early.emplace_back(line);
			return;
		}
	}
	foundation::api()->log(line);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		logf("---- Astra in Death Stranding loaded");
		guard::install();
		g_resolved = game::resolve(g_status, sizeof(g_status));
		logf("resolve: %s", g_status);
		physics::resolve(g_physStatus, sizeof(g_physStatus));
		logf("%s", g_physStatus);
		ui::install(); // DS's UI over her (unless another of our add-ons already does it for every effect)
		reshade::register_event<reshade::addon_event::present>(on_present);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_overlay("Astra in Death Stranding", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
		physics::uninstall_tick();
		// FreeLibrary: the foundation must not call into this module again (at process exit nothing presents any more,
		// and the foundation may be gone already)
		if (reserved == nullptr && g_linked && GetModuleHandleW(L"astra.addon64") != nullptr)
			foundation::api()->on_present(nullptr, nullptr);
		ui::uninstall();
		guard::uninstall(); // never leave a vectored handler pointing into this module once it is unmapped (astra-reshade 8dc0961)
		reshade::unregister_addon(module);
		break;
	}
	return TRUE;
}
