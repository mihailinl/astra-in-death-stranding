#include "compositor.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT": Minecraft's frame export, and the Astra engine's (same layout)
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr int32_t kFlagNoOverlay = 16; // the slot holds colour + depth only
	constexpr const char *kEffect = "MCPassthrough.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<float> g_hostNear{0.15f};
	std::atomic<float> g_hostFar{10000.0f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;
	std::atomic<int> g_poseLag{0}; // measured: 0 matches best (third and first person)

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
	};

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};

	/// A picture rendered elsewhere from DS's camera and handed over in shared memory ("MCPT" layout, see the mod's
	/// FrameExporter): Minecraft's world (and its hand and HUD), or the Astra avatar engine's Astra.
	struct Source
	{
		// Where its frames are:
		//   Linux (DS under Wine/Proton, the renderer native): a file in /dev/shm, which Wine sees as Z:\dev\shm\...
		//   Windows (both native): the named mapping the renderer creates
		// (both are tried, named mapping first: no need to detect Wine, which Proton may hide)
		wchar_t mapping_name[128], mapping_path[260];
		const char *label;
		const char *tex_world, *tex_depth, *tex_overlay; // the effect's texture semantics (overlay: may be null)
		// the effect's uniforms
		const char *u_active, *u_planes, *u_rows[3], *u_t, *u_tan;

		std::atomic<bool> active{false};
		std::mutex pose_lock;
		// DS's recent camera poses in this source's coordinates, newest last: the picture being presented may be
		// a frame older than the camera read (g_poseLag back)
		Pose host_poses[4];
		unsigned host_pose_count = 0;
		Pose pose; // the camera the source rendered its frame with

		HANDLE file = INVALID_HANDLE_VALUE;
		HANDLE mapping = nullptr;
		const uint8_t *view = nullptr;
		int32_t slots = 0;
		int64_t stride = 0;
		int64_t last_publish = -1;
		DWORD next_open = 0;
		Layer world, depth, overlay;
		uint32_t width = 0, height = 0;
		bool has_frame = false;
		float near_plane = 0.05f, far_plane = 2048.0f;
		int32_t flags = 7;
		unsigned long long frames = 0;
	};

	Source g_mc{L"Local\\MCPassthroughFrame", L"Z:\\dev\\shm\\dsmc-frame", "Minecraft", "MCWORLD", "MCDEPTH", "MCOVERLAY",
		"McActive", "McPlanes", {"WarpRow0", "WarpRow1", "WarpRow2"}, "WarpT", "WarpTan"};
	Source g_astra{L"Local\\AstraFrame", L"Z:\\dev\\shm\\astra-frame", "Astra", "ASTRAWORLD", "ASTRADEPTH", nullptr,
		"AstraActive", "AstraPlanes", {"AstraWarpRow0", "AstraWarpRow1", "AstraWarpRow2"}, "AstraWarpT", "AstraWarpTan"};
	Source *const g_sources[] = {&g_mc, &g_astra};

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	void close_mapping(Source &src)
	{
		if (src.view != nullptr)
			UnmapViewOfFile(src.view);
		if (src.mapping != nullptr)
			CloseHandle(src.mapping);
		if (src.file != INVALID_HANDLE_VALUE)
			CloseHandle(src.file);
		src.view = nullptr;
		src.mapping = nullptr;
		src.file = INVALID_HANDLE_VALUE;
	}

	bool open_mapping(Source &src)
	{
		if (src.view != nullptr)
			return true;
		if (GetTickCount() < src.next_open)
			return false;
		src.next_open = GetTickCount() + 1000;
		src.mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, src.mapping_name);
		if (src.mapping == nullptr)
		{
			src.file = CreateFileW(src.mapping_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (src.file == INVALID_HANDLE_VALUE)
				return false;
			src.mapping = CreateFileMappingW(src.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		}
		if (src.mapping == nullptr)
		{
			close_mapping(src);
			return false;
		}
		const auto *header = static_cast<const uint8_t *>(MapViewOfFile(src.mapping, FILE_MAP_READ, 0, 0, kHeader));
		if (header == nullptr || read<uint32_t>(header) != kMagic)
		{
			if (header != nullptr)
				UnmapViewOfFile(header);
			close_mapping(src);
			return false;
		}
		src.slots = read<int32_t>(header + 12);
		src.stride = read<int64_t>(header + 16);
		UnmapViewOfFile(header);
		src.view = static_cast<const uint8_t *>(MapViewOfFile(src.mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + src.stride * src.slots)));
		if (src.view == nullptr)
		{
			close_mapping(src);
			return false;
		}
		src.last_publish = -1;
		logf("compositor: connected to %s's frame export (%d slots)", src.label, src.slots);
		return true;
	}

	void destroy_layers(device *dev, Source &src)
	{
		for (Layer *layer : {&src.world, &src.depth, &src.overlay})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		src.width = src.height = 0;
		src.has_frame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void bind(effect_runtime *runtime, Source &src)
	{
		runtime->update_texture_bindings(src.tex_world, src.world.srv, src.world.srv);
		runtime->update_texture_bindings(src.tex_depth, src.depth.srv, src.depth.srv);
		if (src.tex_overlay != nullptr)
			runtime->update_texture_bindings(src.tex_overlay, src.overlay.srv, src.overlay.srv);
	}

	/// Upload the newest published frame, if there is one we haven't shown yet.
	void upload(effect_runtime *runtime, Source &src)
	{
		// the renderer restarted: a new file or mapping replaced the one we hold
		const uint32_t magic = read<uint32_t>(src.view);
		if (magic != kMagic)
		{
			close_mapping(src);
			return;
		}
		const int64_t published = read<int64_t>(src.view + 32);
		if (published == src.last_publish)
			return;
		const int32_t slot = read<int32_t>(src.view + 40);
		if (slot < 0 || slot >= src.slots)
			return;
		const uint8_t *desc = src.view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		const int32_t flags = read<int32_t>(desc + 44);
		const bool has_overlay = (flags & kFlagNoOverlay) == 0;
		if (w == 0 || h == 0 || int64_t(w) * h * 4 * (has_overlay ? 3 : 2) > src.stride)
			return;
		device *dev = runtime->get_device();
		if (w != src.width || h != src.height)
		{
			destroy_layers(dev, src);
			if (!create_layer(dev, src.world, w, h, format::r8g8b8a8_unorm) || !create_layer(dev, src.depth, w, h, format::r32_float) ||
				(src.tex_overlay != nullptr && !create_layer(dev, src.overlay, w, h, format::r8g8b8a8_unorm)))
			{
				destroy_layers(dev, src);
				return;
			}
			src.width = w;
			src.height = h;
			bind(runtime, src);
		}
		const uint8_t *base = src.view + kHeader + src.stride * slot;
		const size_t layer = size_t(w) * h * 4;
		subresource_data data;
		data.row_pitch = w * 4;
		data.slice_pitch = static_cast<uint32_t>(layer);
		data.data = const_cast<uint8_t *>(base);
		dev->update_texture_region(data, src.world.tex, 0);
		data.data = const_cast<uint8_t *>(base + layer);
		dev->update_texture_region(data, src.depth.tex, 0);
		if (src.tex_overlay != nullptr && has_overlay)
		{
			data.data = const_cast<uint8_t *>(base + 2 * layer);
			dev->update_texture_region(data, src.overlay.tex, 0);
		}
		if (read<int64_t>(desc) != seq)
			return; // the renderer rewrote the slot mid-copy: show the next one instead
		src.last_publish = published;
		src.near_plane = read<float>(desc + 32);
		src.far_plane = read<float>(desc + 36);
		src.flags = flags;
		src.pose.fov = read<float>(desc + 40);
		src.pose.yaw = read<float>(desc + 72);
		src.pose.pitch = read<float>(desc + 76);
		src.pose.roll = read<float>(desc + 80);
		src.pose.x = read<double>(desc + 48);
		src.pose.y = read<double>(desc + 56);
		src.pose.z = read<double>(desc + 64);
		src.pose.valid = true;
		src.has_frame = true;
		++src.frames;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in GTA's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload MCPassthrough.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a GTA restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"reshade-shaders\\Shaders\\MCPassthrough.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
	}

	void set_bool(effect_runtime *runtime, const char *name, bool value)
	{
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
			runtime->set_uniform_value_bool(v, value);
	}

	void set_float3(effect_runtime *runtime, const char *name, float x, float y, float z)
	{
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
			runtime->set_uniform_value_float(v, x, y, z);
	}

	/// One source's frame and its re-projection from the camera it was rendered with to DS's latest (extrapolated
	/// by the effect's PosePrediction frames). False: nothing of it to show this frame.
	bool prepare(effect_runtime *runtime, Source &src, float predict)
	{
		bool on = src.active && open_mapping(src);
		if (on)
			upload(runtime, src);
		on = on && src.has_frame;
		// The technique stays enabled (preset); the Active uniforms gate it, so DS passes through untouched until a
		// frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		set_bool(runtime, src.u_active, on);
		if (!on)
			return false;
		set_float3(runtime, src.u_planes, src.near_plane, src.far_plane, float(src.flags));

		Pose host, prev;
		{
			std::lock_guard<std::mutex> lock(src.pose_lock);
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 2));
			if (src.host_pose_count > lag)
				host = src.host_poses[(src.host_pose_count - 1 - lag) & 3];
			if (src.host_pose_count > lag + 1)
				prev = src.host_poses[(src.host_pose_count - 2 - lag) & 3];
		}
		const bool warp = host.valid && src.pose.valid && !(&src == &g_mc && g_cameraLocked);
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, src.pose, m);
			// T = R_src^T (host position - the source camera's position), in the source camera's space
			float rm[3][3];
			camera_rotation(src.pose, rm);
			const float d[3] = {float(host.x - src.pose.x), float(host.y - src.pose.y), float(host.z - src.pose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
		}
		for (int i = 0; i < 3; ++i)
			set_float3(runtime, src.u_rows[i], m[i][0], m[i][1], m[i][2]);
		set_float3(runtime, src.u_t, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : src.pose.fov) * d2r * 0.5f), tanSrc = std::tan(src.pose.fov * d2r * 0.5f);
		set_float3(runtime, src.u_tan, tanHost, tanSrc, float(src.width) / float(src.height));
		return true;
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		float predict = 0.0f;
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		bool any = false;
		for (Source *src : g_sources)
			any = prepare(runtime, *src, predict) || any;
		if (!any)
			return;

		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		set_float3(runtime, "Shake", g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		for (Source *src : g_sources)
			if (src->width != 0)
				bind(runtime, *src);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		for (Source *src : g_sources)
			destroy_layers(runtime->get_device(), *src);
	}

	void push_pose(Source &src, float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(src.pose_lock);
		src.host_poses[src.host_pose_count & 3] = {yaw, pitch, roll, fov, x, y, z, true};
		++src.host_pose_count;
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		// the add-on itself is registered in DllMain; this only subscribes the compositor's events
		(void)module;
		if (g_registered)
			return true;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "DSMC: compositor events registered");
		return true;
	}

	void unregister(void *module)
	{
		(void)module;
		g_registered = false;
	}

	void set_active(bool active)
	{
		g_mc.active = active;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		push_pose(g_mc, yaw, pitch, roll, fov, x, y, z);
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}

	void set_astra_active(bool active)
	{
		g_astra.active = active;
	}

	void set_astra_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		push_pose(g_astra, yaw, pitch, roll, fov, x, y, z);
	}

	unsigned long long astra_frames()
	{
		return g_astra.frames;
	}

	void set_astra_frames_at(const char *where)
	{
		// "/dev/shm/name" (Linux: the file, seen by Wine under Z:) or a Win32 mapping name ("Local\\name")
		wchar_t name[128] = L"", path[260] = L"";
		if (where[0] == '/')
		{
			int n = std::swprintf(path, 260, L"Z:%hs", where);
			for (int i = 0; i < n; ++i)
				if (path[i] == L'/')
					path[i] = L'\\';
			std::swprintf(name, 128, L"Local\\%hs", std::strrchr(where, '/') + 1);
		}
		else
			std::swprintf(name, 128, L"%hs", where);
		if (std::wcscmp(name, g_astra.mapping_name) == 0 && std::wcscmp(path, g_astra.mapping_path) == 0)
			return;
		std::wcscpy(g_astra.mapping_name, name);
		std::wcscpy(g_astra.mapping_path, path);
		close_mapping(g_astra);
		g_astra.has_frame = false;
		g_astra.next_open = 0;
		logf("compositor: Astra's frames at %s", where);
	}
}
