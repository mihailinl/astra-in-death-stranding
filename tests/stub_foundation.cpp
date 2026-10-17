// A minimal stand-in for astra.addon64's provider API (version 2), for tests/foundation_relink_tests.cpp. Built
// twice under two different tags (ASTRA_STUB_TAG "A" / "B", tools/foundationtest.sh) so the test can tell WHICH
// module instance actually answered a call - the two files are never loaded at once (one is FreeLibrary'd before
// the other loads), standing in for ReShade unloading astra.addon64 at a device destroy and loading a fresh
// instance for the next one (DS2's start-up reload).
//
// Every call appends one line to the file named by the ASTRA_STUB_LOG environment variable - the only way to
// observe, from the test .exe, which DLL instance ran (its own statics are private to it).
#define ASTRA_PROVIDER_EXPORTS
#include "astra_provider.h"
#include <windows.h>
#include <cstdio>

#ifndef ASTRA_STUB_TAG
#define ASTRA_STUB_TAG '?'
#endif

namespace
{
	void note(const char *what)
	{
		char path[MAX_PATH];
		const DWORD n = GetEnvironmentVariableA("ASTRA_STUB_LOG", path, sizeof(path));
		if (n == 0 || n >= sizeof(path))
			return;
		if (FILE *f = std::fopen(path, "a"))
		{
			std::fprintf(f, "%c %s\n", ASTRA_STUB_TAG, what);
			std::fclose(f);
		}
	}
}

extern "C"
{
	int astra_provider_version(void)
	{
		note("version");
		return 2;
	}
	void astra_provider_camera2(unsigned long long, const double *, const float *, const float *, float, int, float, float, float)
	{
		note("camera2");
	}
	void astra_provider_avatar(const double *, const float *, const float *, const float *, float, int, float) { note("avatar"); }
	void astra_provider_anchor(const char *) { note("anchor"); }
	void astra_provider_param(const char *, float) { note("param"); }
	void astra_provider_visible(int) { note("visible"); }
	void astra_provider_light(const float *, const float *, float, float) { note("light"); }
	void astra_provider_cue(const char *, const char *) { note("cue"); }
	void astra_provider_identity(const char *, const char *) { note("identity"); }
	void astra_provider_log(const char *line) { note(line); }
	void astra_provider_on_present(void (*)(void *), void *) { note("on_present"); }
}
