#include "foundation.h"
#include <windows.h>
#include <atomic>
#include <cstdio>

namespace foundation
{
	namespace
	{
		constexpr const wchar_t *kModule = L"astra.addon64";
		Api g_api{};
		std::atomic<const Api *> g_ready{nullptr};
		HMODULE g_module = nullptr; // the instance g_api was resolved from (present thread only: find() is its one writer)
		char g_status[160] = "astra.addon64 (the Astra foundation) is not loaded: Astra cannot be shown";

		template <class Fn>
		bool get(HMODULE m, Fn &fn, const char *name)
		{
			fn = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, name)));
			return fn != nullptr;
		}
	}

	bool find()
	{
		const HMODULE m = GetModuleHandleW(kModule);
		if (m == g_module && g_ready.load(std::memory_order_acquire) != nullptr)
			return true; // still the instance we last resolved
		// the compositor is gone, or a DIFFERENT instance is loaded where an old one was linked (ReShade unloads
		// and reloads its add-ons with every D3D device - DS2 does this once at start-up): drop the stale link
		// FIRST, so a reader (per_frame, logf) sees nullptr rather than a pointer resolved from the old instance,
		// then re-resolve against the module that is actually loaded now.
		g_ready.store(nullptr, std::memory_order_release);
		g_module = nullptr;
		if (m == nullptr)
		{
			std::snprintf(g_status, sizeof(g_status), "astra.addon64 (the Astra foundation) is not loaded: Astra cannot be shown");
			return false;
		}
		if (!get(m, g_api.version, "astra_provider_version"))
		{
			std::snprintf(g_status, sizeof(g_status), "astra.addon64 has no provider API: not the Astra foundation");
			return false;
		}
		const int version = g_api.version();
		if (version < 2 || !get(m, g_api.camera2, "astra_provider_camera2") || !get(m, g_api.avatar, "astra_provider_avatar") ||
			!get(m, g_api.anchor, "astra_provider_anchor") || !get(m, g_api.param, "astra_provider_param") ||
			!get(m, g_api.visible, "astra_provider_visible") || !get(m, g_api.light, "astra_provider_light") || !get(m, g_api.cue, "astra_provider_cue") ||
			!get(m, g_api.identity, "astra_provider_identity") || !get(m, g_api.log, "astra_provider_log") ||
			!get(m, g_api.on_present, "astra_provider_on_present"))
		{
			std::snprintf(g_status, sizeof(g_status), "astra.addon64 is too old (provider API %d; this needs 2)", version);
			return false;
		}
		std::snprintf(g_status, sizeof(g_status), "Astra foundation: provider API %d", version);
		g_module = m;
		g_ready.store(&g_api, std::memory_order_release);
		return true;
	}

	const Api *api()
	{
		return g_ready.load(std::memory_order_acquire);
	}

	const char *status()
	{
		return g_status;
	}
}
