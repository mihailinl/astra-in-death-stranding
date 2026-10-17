// src/foundation.cpp's find()/api(): does the provider's link to the compositor (astra.addon64) come back after
// ReShade unloads and reloads it with a D3D device recreate (DS2's start-up reload; DS1 on an alt-tab or a
// resolution change)? The bug (51b5351): find() latches "found" forever on the FIRST success and never looks
// again (`if (g_ready.load() != nullptr) return true;`, no call to GetModuleHandleW on the second and later
// calls) - so api() keeps handing out pointers into a module that may since have been FreeLibrary'd.
//
// This drives the REAL foundation.cpp against two tiny stand-in DLLs (tests/stub_foundation.cpp, tagged A and
// B) that are loaded one at a time under the exact bare name foundation.cpp looks up ("astra.addon64",
// GetModuleHandleW) - a genuine FreeLibrary + LoadLibrary cycle, never two instances alive together, exactly
// like ReShade's own addon_manager.cpp (load_addons/unload_addons, ref-counted per device).
//
// A Windows program: tools/foundationtest.sh builds it with clang-cl and runs it under Wine, the same way as
// tests/relink_tests.cpp (astra-reshade).
#include "foundation.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_failed = 0, g_checks = 0;
#define CHECK(x)                                                                                                                                     \
	do                                                                                                                                               \
	{                                                                                                                                                \
		++g_checks;                                                                                                                                  \
		if (!(x))                                                                                                                                    \
		{                                                                                                                                            \
			++g_failed;                                                                                                                              \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);                                                                                 \
		}                                                                                                                                            \
	} while (0)

namespace
{
	std::string read_file(const char *path)
	{
		std::string out;
		if (FILE *f = std::fopen(path, "rb"))
		{
			char buf[4096];
			size_t n;
			while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
				out.append(buf, n);
			std::fclose(f);
		}
		return out;
	}

	size_t count(const std::string &text, const char *needle)
	{
		size_t c = 0, pos = 0;
		while ((pos = text.find(needle, pos)) != std::string::npos)
		{
			++c;
			pos += 1;
		}
		return c;
	}
}

int main()
{
	const char *dirA = std::getenv("ASTRA_STUB_DIR_A");
	const char *dirB = std::getenv("ASTRA_STUB_DIR_B");
	const char *logPath = std::getenv("ASTRA_STUB_LOG");
	if (!dirA || !dirB || !logPath)
	{
		std::printf("FAIL: ASTRA_STUB_DIR_A / ASTRA_STUB_DIR_B / ASTRA_STUB_LOG not set\n");
		return 1;
	}
	std::remove(logPath);

	// 1. nothing loaded yet: no link to have, no stale default either.
	CHECK(!foundation::find());
	CHECK(foundation::api() == nullptr);

	// 2. stub A loads where the compositor lives (the bare name find() looks up with GetModuleHandleW).
	const std::string pathA = std::string(dirA) + "\\astra.addon64";
	const HMODULE a = LoadLibraryA(pathA.c_str());
	CHECK(a != nullptr);
	CHECK(foundation::find());
	const foundation::Api *f1 = foundation::api();
	CHECK(f1 != nullptr);
	if (f1 != nullptr)
		f1->log("ping-A");
	CHECK(count(read_file(logPath), "A ping-A") == 1);
	std::printf("foundation: linked to stub A, its own log call reached it\n");

	// 3. stub A is unloaded (ReShade's unload_addons() when the device it was loaded for is destroyed): find()
	// must stop believing it immediately, api() must go back to nullptr - never a pointer into an unmapped
	// module. This is the exact pair that fails on 51b5351 (find() returns true, api() still returns f1).
	CHECK(FreeLibrary(a) != 0);
	CHECK(GetModuleHandleW(L"astra.addon64") == nullptr); // truly gone, not just believed gone
	CHECK(!foundation::find());
	CHECK(foundation::api() == nullptr);
	std::printf("foundation: unlinked once stub A was freed (api() is nullptr, not the stale pointer)\n");

	// 4. stub B loads where a device recreate's NEW instance would: find() must resolve against IT, and a call
	// through api() must reach B - the relink is to the module that is actually there now, never a pointer
	// resolved from A (A never answers again - it is not even loaded any more).
	const std::string pathB = std::string(dirB) + "\\astra.addon64";
	const HMODULE b = LoadLibraryA(pathB.c_str());
	CHECK(b != nullptr);
	CHECK(foundation::find());
	const foundation::Api *f2 = foundation::api();
	CHECK(f2 != nullptr);
	if (f2 != nullptr)
		f2->log("ping-B");
	const std::string afterB = read_file(logPath);
	CHECK(count(afterB, "A ping-A") == 1); // unchanged since step 2 - A took no further calls
	CHECK(count(afterB, "B ping-B") == 1); // B's own call reached B
	std::printf("foundation: relinked to stub B after A was freed (B answered, A stayed silent)\n");

	FreeLibrary(b);
	std::printf("foundation: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
