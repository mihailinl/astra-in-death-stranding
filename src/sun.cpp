#include "sun.h"
#include "game.h"
#include "guard.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace sun
{
	namespace
	{
		// SunLight: vtable rva (its RTTI's GetRTTI getter), Light's colour, intensity and enable, the WorldNode transform
		struct Layout
		{
			uintptr_t vtbl;
			size_t transform, color, intensity, enable;
		};
		constexpr Layout kDS1 = {0x3c07790, 0x20, 0x16C, 0x178, 0x24A}; // Death Stranding DC, Steam build 13300582
		constexpr Layout kDS2 = {0x341DA70, 0x20, 0x1D4, 0x1E0, 0x2E8}; // Death Stranding 2 (RTTI dump)
		const Layout &L()
		{
			return game::version() == 2 ? kDS2 : kDS1;
		}
		constexpr ULONGLONG kRescanMs = 10000;

		std::atomic<uintptr_t> g_sun{0};
		int g_axis = 1;
		std::atomic<bool> g_scanning{false};
		ULONGLONG g_nextScan = 0;
		char g_status[200] = "sun: not searched yet";

		struct Raw
		{
			double pos[3];
			float col[3][3];
			float color[3];
			float intensity;
			uint8_t enable;
			uintptr_t vtbl;
		};

		bool read_raw(uintptr_t p, Raw &r)
		{
			return guard::run("sun read", [&] {
				r.vtbl = *reinterpret_cast<const uintptr_t *>(p);
				std::memcpy(r.pos, reinterpret_cast<const void *>(p + L().transform), sizeof(r.pos));
				std::memcpy(r.col, reinterpret_cast<const void *>(p + L().transform + 24), sizeof(r.col));
				std::memcpy(r.color, reinterpret_cast<const void *>(p + L().color), sizeof(r.color));
				std::memcpy(&r.intensity, reinterpret_cast<const void *>(p + L().intensity), sizeof(r.intensity));
				r.enable = *reinterpret_cast<const uint8_t *>(p + L().enable);
			});
		}

		/// A transform with unit, orthogonal columns; a colour and an intensity that are numbers.
		bool plausible(const Raw &r)
		{
			for (int i = 0; i < 3; ++i)
			{
				if (!std::isfinite(r.pos[i]) || std::fabs(r.pos[i]) > 1e7)
					return false;
				const float l = std::sqrt(r.col[i][0] * r.col[i][0] + r.col[i][1] * r.col[i][1] + r.col[i][2] * r.col[i][2]);
				if (!std::isfinite(l) || std::fabs(l - 1.0f) > 0.02f)
					return false;
				if (!std::isfinite(r.color[i]) || r.color[i] < 0.0f || r.color[i] > 1e4f)
					return false;
			}
			const float d01 = r.col[0][0] * r.col[1][0] + r.col[0][1] * r.col[1][1] + r.col[0][2] * r.col[1][2];
			const float d12 = r.col[1][0] * r.col[2][0] + r.col[1][1] * r.col[2][1] + r.col[1][2] * r.col[2][2];
			return std::fabs(d01) < 0.02f && std::fabs(d12) < 0.02f && std::isfinite(r.intensity) && r.intensity >= 0.0f && r.intensity < 1e7f;
		}

		/// Every committed private read-write page, for the SunLight vtable (seconds, on its own thread).
		void scan()
		{
			const uintptr_t base = uintptr_t(GetModuleHandleW(nullptr));
			const uintptr_t want = base + L().vtbl;
			std::vector<uintptr_t> found;
			unsigned long long bytes = 0;
			MEMORY_BASIC_INFORMATION mbi;
			uintptr_t addr = 0x10000;
			const ULONGLONG t0 = GetTickCount64();
			while (addr < 0x7FFFFFFF0000ull && VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
			{
				const uintptr_t next = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
				if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) != 0 && (mbi.Protect & PAGE_GUARD) == 0)
				{
					const auto *p = static_cast<const uintptr_t *>(mbi.BaseAddress);
					const size_t n = mbi.RegionSize / sizeof(uintptr_t);
					bytes += mbi.RegionSize;
					// a page freed under us faults: that region is skipped (guarded), the scan goes on
					guard::run("sun scan", [&] {
						for (size_t i = 0; i < n; ++i)
							if (p[i] == want && found.size() < 64)
								found.push_back(uintptr_t(p + i));
					});
				}
				if (next <= addr)
					break;
				addr = next;
			}
			uintptr_t best = 0;
			float best_intensity = -1.0f;
			for (const uintptr_t f : found)
			{
				Raw r;
				if (!read_raw(f, r) || !plausible(r))
					continue;
				logf("SUN candidate %p: enable %d intensity %.3f color %.3f %.3f %.3f | c0 %.3f %.3f %.3f | c1 %.3f %.3f %.3f | c2 %.3f %.3f %.3f",
					reinterpret_cast<void *>(f), r.enable, r.intensity, r.color[0], r.color[1], r.color[2], r.col[0][0], r.col[0][1], r.col[0][2], r.col[1][0],
					r.col[1][1], r.col[1][2], r.col[2][0], r.col[2][1], r.col[2][2]);
				const float score = (r.enable ? 1e6f : 0.0f) + r.intensity;
				if (score > best_intensity)
					best_intensity = score, best = f;
			}
			g_sun = best;
			std::snprintf(g_status, sizeof(g_status), "sun: %s (%zu found in %.1f GB, %.1f s)", best ? "found" : "not found", found.size(),
				double(bytes) / 1e9, double(GetTickCount64() - t0) / 1000.0);
			logf("%s", g_status);
			g_scanning = false;
		}
	}

	void start()
	{
		if (g_scanning.exchange(true))
			return;
		g_nextScan = GetTickCount64() + kRescanMs;
		std::thread(scan).detach();
	}

	State read()
	{
		State s{};
		const uintptr_t p = g_sun;
		Raw r;
		if (p == 0 || !read_raw(p, r) || r.vtbl != uintptr_t(GetModuleHandleW(nullptr)) + L().vtbl || !plausible(r))
		{
			// none yet, or it went away (a new area): look again now and then
			if (!g_scanning && GetTickCount64() >= g_nextScan)
			{
				g_sun = 0;
				start();
			}
			return s;
		}
		s.valid = true;
		// toward the light: against the way it shines (its forward axis, col1; col2 is the other candidate, overlay)
		for (int i = 0; i < 3; ++i)
		{
			s.toward[i] = -r.col[g_axis][i];
			s.color[i] = r.color[i];
			for (int j = 0; j < 3; ++j)
				s.columns[i][j] = r.col[i][j];
		}
		s.intensity = r.enable ? r.intensity : 0.0f;
		return s;
	}

	int &axis()
	{
		return g_axis;
	}

	const char *status()
	{
		return g_status;
	}
}
