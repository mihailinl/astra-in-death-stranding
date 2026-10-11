#include "layer.h"
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

namespace layer
{
	namespace
	{
		constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
		constexpr int kHeader = 4096, kSlotDesc = 256, kSlotDescBytes = 128;
		constexpr int32_t kFlagNoOverlay = 16; // the slot holds colour + depth only (overlay plane unused)
		constexpr const char *kEffect = "AstraPassthrough.fx";

		struct Pose
		{
			float yaw = 0, pitch = 0, roll = 0, fov = 45;
			double x = 0, y = 0, z = 0;
			bool valid = false;
		};
		struct Layer
		{
			resource tex = {0};
			resource_view srv = {0};
		};

		std::atomic<bool> g_active{false};
		std::atomic<float> g_hostNear{0.2f}, g_hostFar{4000.0f};
		std::mutex g_poseLock;
		Pose g_host; // DS's camera (engine coordinates), newest
		Pose g_pose; // the camera the shown picture was rendered with
		unsigned long long g_hostFrame = 0;
		bool (*g_movedSince)(unsigned long long, double[3]) = nullptr;
		float g_sunToward[3] = {0, 1, 0}, g_shadow = 0.0f;
		double g_feet[3] = {};
		float g_size = 1.0f;
		constexpr float kHeight = 1.40f; // the built-in character's height at size 1 (measured in her pictures)

		wchar_t g_name[128] = L"Local\\AstraFrame", g_path[260] = L"Z:\\dev\\shm\\astra-frame";
		HANDLE g_file = INVALID_HANDLE_VALUE, g_mapping = nullptr;
		const uint8_t *g_view = nullptr;
		int32_t g_slots = 0;
		int64_t g_stride = 0, g_lastPublish = -1;
		DWORD g_nextOpen = 0;
		Layer g_world, g_depth, g_sunMap;
		uint32_t g_sunN = 0;
		// her depth seen from the sun (plane 3, flag 32: "SUNV" header + N x N float32), for her shadow on DS's ground
		struct SunView
		{
			bool valid = false;
			double center[3] = {};
			float right[3] = {}, up[3] = {}, dir[3] = {};
			float half = 1.0f, near_dist = 0.0f;
		} g_sunView;
		constexpr int32_t kFlagSunView = 32;
		constexpr uint32_t kSunMagic = 0x564E5553; // "SUNV"
		constexpr int kSunHeader = 128;
		uint32_t g_width = 0, g_height = 0;
		bool g_hasFrame = false;
		int32_t g_flags = 24;
		float g_near = 0.05f, g_far = 300.0f;
		std::atomic<unsigned long long> g_frames{0};
		std::atomic<unsigned long long> g_wantFrame{0}, g_waitTimeouts{0};
		std::atomic<float> g_waitMs{6.0f};

		template <typename T>
		T read(const uint8_t *p)
		{
			T v;
			std::memcpy(&v, p, sizeof(T));
			return v;
		}

		void close_mapping()
		{
			if (g_view != nullptr)
				UnmapViewOfFile(g_view);
			if (g_mapping != nullptr)
				CloseHandle(g_mapping);
			if (g_file != INVALID_HANDLE_VALUE)
				CloseHandle(g_file);
			g_view = nullptr;
			g_mapping = nullptr;
			g_file = INVALID_HANDLE_VALUE;
		}

		/// The engine's frames: a Win32 named mapping (Windows), else Wine's view of the /dev/shm file (Linux).
		bool open_mapping()
		{
			if (g_view != nullptr)
				return true;
			if (GetTickCount() < g_nextOpen)
				return false;
			g_nextOpen = GetTickCount() + 1000;
			g_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, g_name);
			if (g_mapping == nullptr)
			{
				g_file = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (g_file == INVALID_HANDLE_VALUE)
					return false;
				g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			}
			if (g_mapping == nullptr)
			{
				close_mapping();
				return false;
			}
			const auto *header = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
			if (header == nullptr || read<uint32_t>(header) != kMagic)
			{
				if (header != nullptr)
					UnmapViewOfFile(header);
				close_mapping();
				return false;
			}
			g_slots = read<int32_t>(header + 12);
			g_stride = read<int64_t>(header + 16);
			UnmapViewOfFile(header);
			g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(kHeader + g_stride * g_slots)));
			if (g_view == nullptr)
			{
				close_mapping();
				return false;
			}
			g_lastPublish = -1;
			logf("layer: connected to the engine's frames (%d slots)", g_slots);
			return true;
		}

		void destroy_layers(device *dev)
		{
			for (Layer *l : {&g_world, &g_depth, &g_sunMap})
			{
				if (l->srv.handle != 0)
					dev->destroy_resource_view(l->srv);
				if (l->tex.handle != 0)
					dev->destroy_resource(l->tex);
				*l = Layer();
			}
			g_width = g_height = 0;
			g_sunN = 0;
			g_hasFrame = false;
		}

		bool create_layer(device *dev, Layer &l, uint32_t w, uint32_t h, format fmt)
		{
			if (!dev->create_resource(resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
					nullptr, resource_usage::shader_resource, &l.tex))
				return false;
			return dev->create_resource_view(l.tex, resource_usage::shader_resource, resource_view_desc(fmt), &l.srv);
		}

		void bind(effect_runtime *runtime)
		{
			runtime->update_texture_bindings("ASTRAWORLD", g_world.srv, g_world.srv);
			runtime->update_texture_bindings("ASTRADEPTH", g_depth.srv, g_depth.srv);
			runtime->update_texture_bindings("ASTRASUN", g_sunMap.srv, g_sunMap.srv);
		}

		/// The newest published frame, if not shown yet.
		void upload(effect_runtime *runtime)
		{
			if (read<uint32_t>(g_view) != kMagic)
			{
				close_mapping(); // the engine made a new one
				return;
			}
			const int64_t published = read<int64_t>(g_view + 32);
			if (published == g_lastPublish)
				return;
			const int32_t slot = read<int32_t>(g_view + 40);
			if (slot < 0 || slot >= g_slots)
				return;
			const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
			const int64_t seq = read<int64_t>(desc);
			if (seq & 1)
				return;
			const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
			const int32_t flags = read<int32_t>(desc + 44);
			if (w == 0 || h == 0 || int64_t(w) * h * 4 * ((flags & kFlagNoOverlay) ? 2 : 3) > g_stride)
				return;
			device *dev = runtime->get_device();
			if (w != g_width || h != g_height)
			{
				destroy_layers(dev);
				if (!create_layer(dev, g_world, w, h, format::r8g8b8a8_unorm) || !create_layer(dev, g_depth, w, h, format::r32_float))
				{
					destroy_layers(dev);
					return;
				}
				g_width = w;
				g_height = h;
				bind(runtime);
			}
			const uint8_t *base = g_view + kHeader + g_stride * slot;
			const size_t plane = size_t(w) * h * 4;
			subresource_data data;
			data.row_pitch = w * 4;
			data.slice_pitch = static_cast<uint32_t>(plane);
			data.data = const_cast<uint8_t *>(base);
			dev->update_texture_region(data, g_world.tex, 0);
			data.data = const_cast<uint8_t *>(base + plane);
			dev->update_texture_region(data, g_depth.tex, 0);
			// her depth from the sun, when the engine sends it (plane 3)
			SunView sv;
			if ((flags & kFlagSunView) != 0 && (flags & kFlagNoOverlay) == 0)
			{
				const uint8_t *p3 = base + 2 * plane;
				const uint32_t n = read<uint32_t>(p3 + 4);
				if (read<uint32_t>(p3) == kSunMagic && n >= 16 && n <= 2048 && size_t(kSunHeader) + size_t(n) * n * 4 <= plane)
				{
					if (n != g_sunN)
					{
						if (g_sunMap.srv.handle != 0)
							dev->destroy_resource_view(g_sunMap.srv);
						if (g_sunMap.tex.handle != 0)
							dev->destroy_resource(g_sunMap.tex);
						g_sunMap = Layer();
						g_sunN = 0;
						if (create_layer(dev, g_sunMap, n, n, format::r32_float))
							g_sunN = n;
						bind(runtime);
					}
					if (g_sunN == n)
					{
						subresource_data sd;
						sd.row_pitch = n * 4;
						sd.slice_pitch = n * n * 4;
						sd.data = const_cast<uint8_t *>(p3 + kSunHeader);
						dev->update_texture_region(sd, g_sunMap.tex, 0);
						for (int i = 0; i < 3; ++i)
						{
							sv.center[i] = read<double>(p3 + 8 + 8 * i);
							sv.right[i] = read<float>(p3 + 32 + 4 * i);
							sv.up[i] = read<float>(p3 + 44 + 4 * i);
							sv.dir[i] = read<float>(p3 + 56 + 4 * i);
						}
						sv.half = read<float>(p3 + 68);
						sv.near_dist = read<float>(p3 + 72);
						sv.valid = sv.half > 0.0f;
					}
				}
			}
			if (read<int64_t>(desc) != seq)
				return; // rewritten while we copied: the next one
			g_sunView = sv;
			g_lastPublish = published;
			g_near = read<float>(desc + 32);
			g_far = read<float>(desc + 36);
			g_flags = flags;
			g_hostFrame = read<uint64_t>(desc + 16);
			g_pose.fov = read<float>(desc + 40);
			g_pose.x = read<double>(desc + 48);
			g_pose.y = read<double>(desc + 56);
			g_pose.z = read<double>(desc + 64);
			g_pose.yaw = read<float>(desc + 72);
			g_pose.pitch = read<float>(desc + 76);
			g_pose.roll = read<float>(desc + 80);
			g_pose.valid = true;
			g_hasFrame = true;
			++g_frames;
		}

		/// Camera-to-world rotation in Minecraft's convention: rotationYXZ(pi - yaw, -pitch, roll), looking down -z.
		void camera_rotation(const Pose &p, float m[3][3])
		{
			const float d2r = 3.14159265f / 180.0f;
			const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
			const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
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

		void set_float3(effect_runtime *runtime, const char *name, float x, float y, float z)
		{
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, name); v.handle != 0)
				runtime->set_uniform_value_float(v, x, y, z);
		}

		/// Her picture for the camera just sent is a millisecond or two away: wait for it (bounded) instead of
		/// re-projecting an older one, so she sits exactly on this frame.
		void wait_for_frame()
		{
			const unsigned long long want = g_wantFrame;
			const float budget = g_waitMs;
			if (want == 0 || budget <= 0.0f || g_view == nullptr)
				return;
			LARGE_INTEGER f, t0, t;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t0);
			for (;;)
			{
				const int32_t slot = read<int32_t>(g_view + 40);
				if (slot >= 0 && slot < g_slots && read<uint64_t>(g_view + kSlotDesc + kSlotDescBytes * slot + 16) >= want)
					break;
				QueryPerformanceCounter(&t);
				if (float(t.QuadPart - t0.QuadPart) * 1000.0f / float(f.QuadPart) > budget)
				{
					++g_waitTimeouts;
					break;
				}
				YieldProcessor();
				SwitchToThread();
			}
		}

		void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
		{
			bool on = g_active && open_mapping();
			if (on)
			{
				wait_for_frame();
				upload(runtime);
			}
			on = on && g_hasFrame;
			// the technique is always enabled (annotation); AstraActive gates it
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraActive"); v.handle != 0)
				runtime->set_uniform_value_bool(v, on);
			if (!on)
				return;
			set_float3(runtime, "AstraPlanes", g_near, g_far, float(g_flags));
			// Minecraft's effect (when installed and compositing) leaves its coverage and depth in McInfoTex for her
			bool mc = false;
			if (const effect_uniform_variable v = runtime->find_uniform_variable("MCPassthrough.fx", "McActive"); v.handle != 0)
				runtime->get_uniform_value_bool(v, &mc, 1);
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "McPresent"); v.handle != 0)
				runtime->set_uniform_value_bool(v, mc);
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "HostPlanes"); v.handle != 0)
				runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());

			Pose host;
			{
				std::lock_guard<std::mutex> lock(g_poseLock);
				host = g_host;
			}
			// she moved on by D since her picture (running, riding): as if its camera had been D further on too
			Pose from = g_pose;
			double moved[3];
			if (g_movedSince != nullptr && g_movedSince(g_hostFrame, moved))
			{
				from.x += moved[0];
				from.y += moved[1];
				from.z += moved[2];
			}
			float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, t[3] = {0, 0, 0};
			if (host.valid)
			{
				// rows of R_from^T R_host (DS camera ray -> her picture's camera ray), and DS's camera position in it
				float rh[3][3], rm[3][3];
				camera_rotation(host, rh);
				camera_rotation(from, rm);
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j)
					{
						m[i][j] = 0;
						for (int k = 0; k < 3; ++k)
							m[i][j] += rm[k][i] * rh[k][j];
					}
				const float d[3] = {float(host.x - from.x), float(host.y - from.y), float(host.z - from.z)};
				for (int i = 0; i < 3; ++i)
					t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
			}
			// her shadow: the sun's direction and her feet in DS's camera space (camera-to-world rotation transposed)
			if (host.valid)
			{
				float rh[3][3];
				camera_rotation(host, rh);
				float sunv[3], feetv[3];
				double fl[3];
				float st[3], strength;
				{
					std::lock_guard<std::mutex> lock(g_poseLock);
					std::copy(g_feet, g_feet + 3, fl);
					std::copy(g_sunToward, g_sunToward + 3, st);
					strength = g_shadow;
				}
				const float fd[3] = {float(fl[0] - host.x), float(fl[1] - host.y), float(fl[2] - host.z)};
				for (int i = 0; i < 3; ++i)
				{
					sunv[i] = rh[0][i] * st[0] + rh[1][i] * st[1] + rh[2][i] * st[2];
					feetv[i] = rh[0][i] * fd[0] + rh[1][i] * fd[1] + rh[2][i] * fd[2];
				}
				set_float3(runtime, "SunView", sunv[0], sunv[1], sunv[2]);
				set_float3(runtime, "UpView", rh[1][0], rh[1][1], rh[1][2]); // the world's +Y in DS's camera space
				// her view from the sun, in DS's camera space (moved along with her like her picture)
				const bool sun_map = g_sunView.valid && g_sunN != 0;
				if (sun_map)
				{
					const SunView &v = g_sunView;
					const double cw[3] = {v.center[0] + (from.x - g_pose.x), v.center[1] + (from.y - g_pose.y), v.center[2] + (from.z - g_pose.z)};
					const float cd[3] = {float(cw[0] - host.x), float(cw[1] - host.y), float(cw[2] - host.z)};
					float c[3], r[3], u[3], d[3];
					for (int i = 0; i < 3; ++i)
					{
						c[i] = rh[0][i] * cd[0] + rh[1][i] * cd[1] + rh[2][i] * cd[2];
						r[i] = rh[0][i] * v.right[0] + rh[1][i] * v.right[1] + rh[2][i] * v.right[2];
						u[i] = rh[0][i] * v.up[0] + rh[1][i] * v.up[1] + rh[2][i] * v.up[2];
						d[i] = rh[0][i] * v.dir[0] + rh[1][i] * v.dir[1] + rh[2][i] * v.dir[2];
					}
					set_float3(runtime, "SunVCenter", c[0], c[1], c[2]);
					set_float3(runtime, "SunVRight", r[0], r[1], r[2]);
					set_float3(runtime, "SunVUp", u[0], u[1], u[2]);
					set_float3(runtime, "SunVDir", d[0], d[1], d[2]);
					set_float3(runtime, "SunVParams", v.half, v.near_dist, float(g_sunN));
				}
				if (const effect_uniform_variable uv = runtime->find_uniform_variable(kEffect, "SunVActive"); uv.handle != 0)
					runtime->set_uniform_value_bool(uv, sun_map);
				set_float3(runtime, "AstraFeetView", feetv[0], feetv[1], feetv[2]);
				if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraShadow"); v.handle != 0)
					runtime->set_uniform_value_float(v, strength);
				float size;
				{
					std::lock_guard<std::mutex> lock(g_poseLock);
					size = g_size;
				}
				if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraHeight"); v.handle != 0)
					runtime->set_uniform_value_float(v, kHeight * size);
				if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraSize"); v.handle != 0)
					runtime->set_uniform_value_float(v, size);
			}
			// her picture was drawn from this very camera (she hasn't moved since either): no re-projection at all
			const bool identity = host.valid && std::fabs(host.yaw - from.yaw) < 1e-3f && std::fabs(host.pitch - from.pitch) < 1e-3f &&
				std::fabs(host.roll - from.roll) < 1e-3f && std::fabs(host.fov - from.fov) < 1e-3f && std::fabs(host.x - from.x) < 1e-4 &&
				std::fabs(host.y - from.y) < 1e-4 && std::fabs(host.z - from.z) < 1e-4;
			if (const effect_uniform_variable v = runtime->find_uniform_variable(kEffect, "AstraWarpIdentity"); v.handle != 0)
				runtime->set_uniform_value_bool(v, identity);
			set_float3(runtime, "AstraWarpRow0", m[0][0], m[0][1], m[0][2]);
			set_float3(runtime, "AstraWarpRow1", m[1][0], m[1][1], m[1][2]);
			set_float3(runtime, "AstraWarpRow2", m[2][0], m[2][1], m[2][2]);
			set_float3(runtime, "AstraWarpT", t[0], t[1], t[2]);
			const float d2r = 3.14159265f / 180.0f;
			set_float3(runtime, "AstraWarpTan", std::tan((host.valid ? host.fov : g_pose.fov) * d2r * 0.5f), std::tan(g_pose.fov * d2r * 0.5f),
				float(g_width) / float(g_height));
		}

		void on_reloaded_effects(effect_runtime *runtime)
		{
			if (g_width != 0)
				bind(runtime);
		}

		void on_destroy_effect_runtime(effect_runtime *runtime)
		{
			destroy_layers(runtime->get_device());
		}
	}

	void install()
	{
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		g_host = {yaw, pitch, roll, fov, x, y, z, true};
	}

	void set_motion(bool (*moved_since)(unsigned long long frame, double out[3]))
	{
		g_movedSince = moved_since;
	}

	void set_frames_at(const char *where)
	{
		// "/dev/shm/name" (Linux: the file, seen by Wine under Z:) or a Win32 mapping name ("Local\\name")
		wchar_t name[128] = L"", path[260] = L"";
		if (where[0] == '/')
		{
			const int n = std::swprintf(path, 260, L"Z:%hs", where);
			for (int i = 0; i < n; ++i)
				if (path[i] == L'/')
					path[i] = L'\\';
			std::swprintf(name, 128, L"Local\\%hs", std::strrchr(where, '/') + 1);
		}
		else
			std::swprintf(name, 128, L"%hs", where);
		if (std::wcscmp(name, g_name) == 0 && std::wcscmp(path, g_path) == 0)
			return;
		std::wcscpy(g_name, name);
		std::wcscpy(g_path, path);
		close_mapping();
		g_hasFrame = false;
		g_nextOpen = 0;
		logf("layer: the engine's frames are at %s", where);
	}

	void set_sun(const float toward[3], float strength)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		std::copy(toward, toward + 3, g_sunToward);
		g_shadow = strength;
	}

	void set_feet(const double feet[3], float size)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		std::copy(feet, feet + 3, g_feet);
		g_size = size;
	}

	void expect_frame(unsigned long long cam_id)
	{
		g_wantFrame = cam_id;
	}

	float &wait_ms()
	{
		static float v = 6.0f;
		g_waitMs = v;
		return v;
	}

	unsigned long long wait_timeouts()
	{
		return g_waitTimeouts;
	}

	unsigned long long frames()
	{
		return g_frames;
	}
}
