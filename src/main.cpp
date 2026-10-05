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
#include "ui.h"
#include <windows.h>
#include <cstdarg>
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

	void on_present(effect_runtime *runtime)
	{
		++g_frame;
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
		// her walking calls into the game: a fault switches it off instead of letting DS's crash handler end the game
		if (!g_followBroken && !guard::run("Astra follows Sam", [] { astra::frame(g_snap); }))
			g_followBroken = true;
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		astralink::frame(g_snap, int(w), int(h));
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
		float lift, forward;
		astra::seat_offset(lift, forward, false);
		ImGui::SetNextItemWidth(160);
		bool seat = ImGui::SliderFloat("seat height", &lift, -1.0f, 1.0f, "%.2f m");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		seat |= ImGui::SliderFloat("seat forward", &forward, -1.0f, 1.0f, "%.2f m");
		if (seat)
			astra::seat_offset(lift, forward, true);
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
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_overlay("Astra", on_overlay);
		break;
	case DLL_PROCESS_DETACH:
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
