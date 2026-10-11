// Astra in Death Stranding: a ReShade add-on of its own (next to the Minecraft one, or alone).
//   astra.cpp      where she walks: follows Sam on DS's real collision, rides in his vehicles
//   astralink.cpp  the Astra avatar engine's game bridge: DS's camera and her pose out, her pictures back
//   layer.cpp      her pictures into AstraPassthrough.fx, against DS's depth (and Minecraft's)
// Shared with the Minecraft mod (extern/minecraft-in-death-stranding): DS's camera and Sam (game), DS's collision
// (physics), fault containment (guard), the WebSocket client (ws) and DS's UI over what add-ons draw (ui).
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include "astra.h"
#include "astralink.h"
#include "game.h"
#include "guard.h"
#include "layer.h"
#include "log.h"
#include "physics.h"
#include "sun.h"
#include "ui.h"
#include <windows.h>
#include <cstdarg>
#include <algorithm>
#include <atomic>
#include <cstdio>

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "Astra";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Astra, the Astra companion's avatar, walks with Sam in Death Stranding.";

namespace
{
	bool g_resolved = false;
	char g_status[256] = "not resolved";
	char g_physStatus[160] = "physics: not resolved";
	game::Snapshot g_snap;
	uint64_t g_frame = 0;
	FILE *g_log = nullptr;
	ULONGLONG g_readPausedUntil = 0;
	bool g_followBroken = false;
	sun::State g_sun{};
	float g_lit = 1.0f;

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

	// When DS's camera is read and her camera goes to the engine: at the start of the Present, before ReShade renders
	// its effects (so her picture is re-projected onto the camera being presented, and the engine gets it soonest), or,
	// as before, after them (reshade_present: her picture then lands on the previous frame's camera). Overlay switch.
	bool g_earlyCamera = true;
	uint32_t g_bbWidth = 0, g_bbHeight = 0;

	void per_frame(uint32_t w, uint32_t h);

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

	void on_present(effect_runtime *runtime)
	{
		++g_frame;
		runtime->get_screenshot_width_and_height(&g_bbWidth, &g_bbHeight);
		if (!g_earlyCamera)
			per_frame(g_bbWidth, g_bbHeight);
	}

	void on_present_early(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
	{
		if (g_earlyCamera)
			per_frame(g_bbWidth, g_bbHeight);
	}

	void per_frame(uint32_t w, uint32_t h)
	{
		if (!g_resolved)
			return;
		static bool started = false;
		if (!started)
		{
			started = true; // threads start here, not in DllMain (loader lock)
			astralink::start();
		}
		read_snapshot();
		if (g_snap.ok && g_snap.near_plane > 0.0f && g_snap.far_plane > g_snap.near_plane)
			layer::set_host_planes(g_snap.near_plane, g_snap.far_plane);
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

		// DS's sun (or moon): her light in the engine, and her shadow on DS's ground
		g_sun = sun::read();
		g_tickSun = g_sun;
		float lit = ds2 ? g_tickLit.load() : 1.0f;
		if (!ds2 && g_sun.valid && g_snap.ok && !g_followBroken)
			guard::run("Astra in the sun", [&] { lit = astra::sunlit(g_sun.toward, reinterpret_cast<const void *>(g_snap.sam_ptr)); });
		g_lit += (lit - g_lit) * 0.15f;
		astralink::set_light(g_sun.valid, g_sun.toward, g_sun.color, g_sun.intensity, g_lit);
		// the frame's cam goes last: the engine renders the moment it reads it (anything after lands on the next frame)
		astralink::frame(g_snap, int(w), int(h));
		const float toward_gl[3] = {g_sun.toward[0], g_sun.toward[2], -g_sun.toward[1]};
		// her shadow: while the light is up (it fades as it sets) and she stands in it
		const float up = g_sun.valid ? std::clamp(g_sun.toward[2] * 5.0f, 0.0f, 1.0f) : 0.0f;
		layer::set_sun(toward_gl, up * g_lit);
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

	void on_overlay(effect_runtime *)
	{
		ImGui::TextUnformatted(g_status);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", g_physStatus);
		if (g_followBroken)
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Her walking hit a fault and was switched off (see astra.log GUARD)");
		const astralink::Stats ls = astralink::stats();
		ImGui::Text("Astra engine: %s   sent %llu  received %llu  frames %llu", astralink::connected() ? "linked" : "waiting (127.0.0.1:25600)", ls.sent,
			ls.received, ls.frames);
		if (const char *ev = astralink::last_event(); ev[0])
			ImGui::TextDisabled("last: %s", ev);
		ImGui::Checkbox("Camera of this frame (off: the previous one)", &g_earlyCamera);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		ImGui::SliderFloat("wait for her picture", &layer::wait_ms(), 0.0f, 12.0f, "%.1f ms");
		ImGui::SameLine();
		ImGui::TextDisabled("(late %llu times)", layer::wait_timeouts());
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

void logf(const char *fmt, ...)
{
	if (g_log == nullptr)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	std::fprintf(g_log, "%02d:%02d:%02d.%03d f%llu ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, (unsigned long long)g_frame);
	va_list args;
	va_start(args, fmt);
	std::vfprintf(g_log, fmt, args);
	va_end(args);
	std::fputc('\n', g_log);
	std::fflush(g_log);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		{
			wchar_t path[MAX_PATH];
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (wchar_t *slash = wcsrchr(path, L'\\'))
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"astra.log");
			_wfopen_s(&g_log, path, L"a");
		}
		logf("---- Astra loaded");
		guard::install();
		g_resolved = game::resolve(g_status, sizeof(g_status));
		logf("resolve: %s", g_status);
		physics::resolve(g_physStatus, sizeof(g_physStatus));
		logf("%s", g_physStatus);
		ui::install(); // DS's UI over her (unless another of our add-ons already does it for every effect)
		layer::install();
		reshade::register_event<reshade::addon_event::present>(on_present_early);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_overlay("Astra", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
		physics::uninstall_tick();
		if (reserved == nullptr) // FreeLibrary; on process exit the link thread is already gone, joining would hang
			astralink::stop();
		ui::uninstall();
		reshade::unregister_addon(module);
		if (g_log)
			std::fclose(g_log);
		break;
	}
	return TRUE;
}
