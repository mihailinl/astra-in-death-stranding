#pragma once
// The Astra foundation: astra-reshade's astra.addon64 (extern/astra-reshade), the ReShade add-on that composites
// Astra into a game's picture - the link to her engine, her pictures from its frame ring, re-projection onto DS's
// camera, the depth test. ReShade loads it beside this add-on. Its PROVIDER API (include/astra_provider.h) is how this
// add-on tells it where DS's camera is and where she stands; it is looked up by name in the loaded module.
#include "astra_provider.h"

namespace foundation
{
	/// The provider API (version 2) of the loaded astra.addon64.
	struct Api
	{
		decltype(&astra_provider_version) version;
		decltype(&astra_provider_camera2) camera2;
		decltype(&astra_provider_avatar) avatar;
		decltype(&astra_provider_anchor) anchor;
		decltype(&astra_provider_param) param;
		decltype(&astra_provider_visible) visible;
		decltype(&astra_provider_light) light;
		decltype(&astra_provider_cue) cue;
		decltype(&astra_provider_identity) identity;
		decltype(&astra_provider_log) log;
		decltype(&astra_provider_on_present) on_present;
	};

	/// Looks the foundation up (call again until it is there: ReShade may load it after this add-on). True once
	/// every function above was found.
	bool find();
	/// The API, or nullptr before find() succeeded. Any thread.
	const Api *api();
	/// What find() saw, for the overlay.
	const char *status();
}
