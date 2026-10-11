#include "astralink.h"
#include "astra.h"
#include "layer.h"
#include "log.h"
#include "ws.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace astralink
{
	namespace
	{
		constexpr int kPort = 25600;                      // the engine's game bridge
		constexpr double kMaxPixels = 1920.0 * 1080.0;   // her picture: DS's aspect, about 1080p worth (scaled up)
		constexpr double kRebase = 1500.0;                // move the floating origin when Sam is this far from it
		constexpr float kFar = 300.0f;                    // she's never far: a short depth range keeps it precise

		WsClient g_ws;
		int g_generation = -1;
		double g_origin[3] = {};
		bool g_originSet = false;
		uint32_t g_viewSent = 0;
		int g_viewW = 1920, g_viewH = 1080;
		bool g_seated = false;
		ULONGLONG g_nextLight = 0;
		constexpr ULONGLONG kLightEveryMs = 100;
		constexpr float kNoonSun = 2.1e6f; // DS's daylight sun: intensity 2.0e6 x colour 1.05 (measured, afternoon, clear)
		ULONGLONG g_paramTick = 0;
		double g_prevPos[3] = {};
		float g_speed = 0.0f, g_airTime = 0.0f, g_scale = 0.0f;
		constexpr float kAirborneAfter = 0.18f; // seconds falling before she's "in the air" for the animations
		unsigned long long g_seatedAt = 0;
		constexpr unsigned long long kSitDownMs = 1300; // the engine's seat_in clip
		unsigned long long g_frame = 0;
		Stats g_stats{};
		std::mutex g_eventLock;
		struct History
		{
			unsigned long long frame;
			double pos[3]; // her feet (or seat), engine coordinates
		};
		constexpr unsigned kHistory = 64;
		History g_history[kHistory] = {};
		History g_latest = {};
		std::mutex g_historyLock;
		char g_event[200] = "";

		/// DS world -> the engine's (glTF) coordinates, relative to the floating origin.
		void to_gl(const double ds[3], double out[3])
		{
			out[0] = ds[0] - g_origin[0];
			out[1] = ds[2] - g_origin[2];
			out[2] = -(ds[1] - g_origin[1]);
		}

		void dir_to_gl(const float ds[3], float out[3])
		{
			out[0] = ds[0];
			out[1] = ds[2];
			out[2] = -ds[1];
		}

		/// A DS camera's rotation in Minecraft's angle convention (degrees: yaw 0 = +Z of the engine's frame and
		/// counter-clockwise from above is negative, pitch positive looks down, roll): echoed back with each picture
		void camera_angles(const game::WorldTransform &t, float &yaw, float &pitch, float &roll)
		{
			const float rad2deg = 57.29577951f;
			const float *r = t.col[0], *f = t.col[1], *u = t.col[2];
			float y = 180.0f - std::atan2(-f[0], f[1]) * rad2deg;
			y = std::fmod(y + 180.0f, 360.0f);
			yaw = (y < 0.0f ? y + 360.0f : y) - 180.0f;
			pitch = -std::asin(std::clamp(f[2], -1.0f, 1.0f)) * rad2deg;
			roll = std::atan2(r[2], u[2]) * rad2deg;
		}

		void sendf(const char *fmt, ...)
		{
			char buffer[1024];
			va_list args;
			va_start(args, fmt);
			const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
			va_end(args);
			if (n > 0 && n < int(sizeof(buffer)) && g_ws.send(buffer))
				++g_stats.sent;
		}

		void receive()
		{
			std::string m;
			int logged = 0;
			while (g_ws.poll(m))
			{
				++g_stats.received;
				{
					std::lock_guard<std::mutex> lock(g_eventLock);
					std::snprintf(g_event, sizeof(g_event), "%.199s", m.c_str());
				}
				if (logged++ < 4)
					logf("astra <- %.300s", m.c_str());
				// the engine's hello says where its frames are: {"t":"hello","v":1,"shm":"/dev/shm/astra-frame",...}
				if (m.rfind("{\"t\":\"hello\"", 0) == 0)
				{
					const size_t at = m.find("\"shm\":\"");
					if (at != std::string::npos)
					{
						const size_t from = at + 7, to = m.find('"', from);
						std::string where = m.substr(from, to - from);
						for (size_t i = where.find("\\\\"); i != std::string::npos; i = where.find("\\\\", i + 1))
							where.erase(i, 1); // JSON's "\\" -> "\"
						if (!where.empty() && to != std::string::npos)
							layer::set_frames_at(where.c_str());
					}
				}
			}
		}
	}

	void start()
	{
		layer::set_motion(moved_since);
		g_ws.start("127.0.0.1", kPort);
	}

	void stop()
	{
		g_ws.stop();
	}

	bool connected()
	{
		return g_ws.connected();
	}

	void send_raw(const char *message)
	{
		if (g_ws.send(message))
			++g_stats.sent;
	}

	void frame(const game::Snapshot &s, int bw, int bh)
	{
		++g_frame;
		receive();
		const astra::State a = astra::state();
		const bool live = g_ws.connected() && s.ok && a.valid && astra::enabled();
		layer::set_active(live);
		if (!g_ws.connected())
			return;
		if (g_ws.generation() != g_generation)
		{
			// a new connection: say who we are, then the picture size again
			g_generation = g_ws.generation();
			g_viewSent = 0;
			sendf("{\"t\":\"hello\",\"v\":1,\"client\":\"ds-astra\"}");
			logf("astra: engine linked");
		}
		// her picture: DS's aspect, about 1080p worth (the engine caps it at its maximum, same aspect)
		if (bw > 0 && bh > 0 && uint32_t(bw * 65536 + bh) != g_viewSent)
		{
			g_viewSent = uint32_t(bw * 65536 + bh);
			const double scale = std::min(1.0, std::sqrt(kMaxPixels / (double(bw) * bh)));
			g_viewW = int(bw * scale + 0.5);
			g_viewH = int(bh * scale + 0.5);
		}
		if (!live)
			return;

		// the floating origin follows Sam in big steps
		const double dx = s.sam.pos[0] - g_origin[0], dy = s.sam.pos[1] - g_origin[1], dz = s.sam.pos[2] - g_origin[2];
		if (!g_originSet || std::fabs(dx) > kRebase || std::fabs(dy) > kRebase || std::fabs(dz) > kRebase)
		{
			for (int i = 0; i < 3; ++i)
				g_origin[i] = std::floor(s.sam.pos[i] / 1000.0) * 1000.0;
			g_originSet = true;
			++g_stats.rebases;
			logf("astra: origin %.0f %.0f %.0f", g_origin[0], g_origin[1], g_origin[2]);
		}

		// Astra: where DS's collision put her and which way she faces (placement; vel also swings her hair and
		// skirt), and raw physics as animator parameters: what a speed means (walk, run) is the animation set's call
		double feet[3];
		to_gl(a.pos, feet);
		float face[3], her_up[3], vel[3];
		dir_to_gl(a.fwd, face);
		dir_to_gl(a.up, her_up);
		dir_to_gl(a.vel, vel);
		// animator parameters, raw but steady: her pace over the ground smoothed (per-frame displacement is noisy,
		// and a jump back to Sam isn't a sprint), and "in the air" only after a moment off the ground (a one-frame
		// drop on rough ground would be a hop and a landing crouch)
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
		// sitting down: her feet stay where they are while the sit-down clip plays, then her hips go onto the seat
		// (the engine eases between the two)
		const bool hips = a.seated && GetTickCount64() - g_seatedAt > kSitDownMs;
		sendf("{\"t\":\"avatar\",\"id\":0,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"up\":[%.5f,%.5f,%.5f],\"vel\":[%.3f,%.3f,%.3f],"
			  "\"scale\":%.3f,\"params\":{\"speed\":%.3f,\"airborne\":%s,\"vspeed\":%.3f,\"seated\":%s}%s}",
			feet[0], feet[1], feet[2], face[0], face[1], face[2], her_up[0], her_up[1], her_up[2], vel[0], vel[1], vel[2], g_scale, a.seated ? 0.0f : ground_speed,
			airborne ? "true" : "false", a.vel[2], a.seated ? "true" : "false",
			hips ? ",\"anchor\":\"hips\"" : ""); // seated, pos is the seat: her hips go there
		// riding with Sam: she sits down on a chair-like seat ("seat": the engine's sitting-on-a-chair state) and
		// stands up again ("unseat": its stand-up clip, then the graph's own states) when he gets out
		if (a.seated != g_seated)
		{
			g_seated = a.seated;
			g_seatedAt = GetTickCount64();
			sendf(a.seated ? "{\"t\":\"play\",\"state\":\"seat\"}" : "{\"t\":\"trigger\",\"name\":\"unseat\"}");
		}

		// DS's camera, after her pose: the engine draws a picture when a camera comes, with the newest pose it has
		double cam[3];
		float fwd[3], up[3];
		to_gl(s.cam.pos, cam);
		dir_to_gl(s.cam.col[1], fwd);
		dir_to_gl(s.cam.col[2], up);
		float yaw, pitch, roll;
		camera_angles(s.cam, yaw, pitch, roll);
		sendf("{\"t\":\"cam\",\"id\":%llu,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"up\":[%.5f,%.5f,%.5f],\"fovY\":%.4f,"
			  "\"near\":%.3f,\"far\":%.1f,\"w\":%d,\"h\":%d,\"echo\":[%.4f,%.4f,%.4f]}",
			g_frame, cam[0], cam[1], cam[2], fwd[0], fwd[1], fwd[2], up[0], up[1], up[2], s.fov, std::max(s.near_plane, 0.05f), kFar, g_viewW,
			g_viewH, yaw, pitch, roll);
		layer::set_host_pose(yaw, pitch, roll, s.fov, cam[0], cam[1], cam[2]);
		layer::expect_frame(g_frame);
		// where she was for this camera: the compositor moves an older picture of her by how far she went since
		{
			std::lock_guard<std::mutex> lock(g_historyLock);
			History &h = g_history[g_frame % kHistory];
			h.frame = g_frame;
			std::copy(feet, feet + 3, h.pos);
			g_latest = h;
		}
		layer::set_feet(feet, g_scale > 0.0f ? g_scale : astra::scale());
	}

	void set_light(bool valid, const float toward_ds[3], const float color[3], float intensity, float visible)
	{
		if (!g_ws.connected() || !valid)
			return;
		const ULONGLONG now = GetTickCount64();
		if (now < g_nextLight)
			return;
		g_nextLight = now + kLightEveryMs;
		float dir[3];
		dir_to_gl(toward_ds, dir);
		// colour as a hue (max channel 1) and a brightness relative to DS's noon sun
		const float m = std::max({color[0], color[1], color[2], 1e-6f});
		sendf("{\"t\":\"light\",\"sun\":{\"dir\":[%.4f,%.4f,%.4f],\"color\":[%.3f,%.3f,%.3f],\"intensity\":%.3f,\"visible\":%.2f}}", dir[0], dir[1], dir[2],
			color[0] / m, color[1] / m, color[2] / m, intensity * m / kNoonSun, visible);
	}

	bool moved_since(unsigned long long frame, double out[3])
	{
		std::lock_guard<std::mutex> lock(g_historyLock);
		const History &h = g_history[frame % kHistory];
		if (h.frame != frame || g_latest.frame == 0)
			return false;
		for (int i = 0; i < 3; ++i)
			out[i] = g_latest.pos[i] - h.pos[i];
		return true;
	}

	Stats stats()
	{
		Stats st = g_stats;
		st.frames = layer::frames();
		return st;
	}

	const char *last_event()
	{
		static char copy[200];
		std::lock_guard<std::mutex> lock(g_eventLock);
		std::memcpy(copy, g_event, sizeof(copy));
		return copy;
	}
}
