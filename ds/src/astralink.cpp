#include "astralink.h"
#include "astra.h"
#include "compositor.h"
#include "host.h"
#include "log.h"
#include "ws.h"
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
		unsigned long long g_frame = 0;
		Stats g_stats{};
		std::mutex g_eventLock;
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
							compositor::set_astra_frames_at(where.c_str());
					}
				}
			}
		}
	}

	void start()
	{
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
		compositor::set_astra_active(live);
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

		// DS's camera
		double cam[3];
		float fwd[3], up[3];
		to_gl(s.cam.pos, cam);
		dir_to_gl(s.cam.col[1], fwd);
		dir_to_gl(s.cam.col[2], up);
		float yaw, pitch, roll;
		host::camera_angles(s.cam, yaw, pitch, roll);
		sendf("{\"t\":\"cam\",\"id\":%llu,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"up\":[%.5f,%.5f,%.5f],\"fovY\":%.4f,"
			  "\"near\":%.3f,\"far\":%.1f,\"w\":%d,\"h\":%d,\"echo\":[%.4f,%.4f,%.4f]}",
			g_frame, cam[0], cam[1], cam[2], fwd[0], fwd[1], fwd[2], up[0], up[1], up[2], s.fov, std::max(s.near_plane, 0.05f), kFar, g_viewW,
			g_viewH, yaw, pitch, roll);
		compositor::set_astra_host_pose(yaw, pitch, roll, s.fov, cam[0], cam[1], cam[2]);

		// Astra: where DS's collision put her and which way she faces (placement; vel also swings her hair and
		// skirt), and raw physics as animator parameters: what a speed means (walk, run) is the animation set's call
		double feet[3];
		to_gl(a.pos, feet);
		const float face_ds[3] = {std::cos(a.heading), std::sin(a.heading), 0.0f};
		float face[3], vel[3];
		dir_to_gl(face_ds, face);
		dir_to_gl(a.vel, vel);
		const float ground_speed = std::sqrt(a.vel[0] * a.vel[0] + a.vel[1] * a.vel[1]);
		sendf("{\"t\":\"avatar\",\"id\":0,\"pos\":[%.4f,%.4f,%.4f],\"fwd\":[%.5f,%.5f,%.5f],\"vel\":[%.3f,%.3f,%.3f],"
			  "\"params\":{\"speed\":%.3f,\"airborne\":%s,\"vspeed\":%.3f}}",
			feet[0], feet[1], feet[2], face[0], face[1], face[2], vel[0], vel[1], vel[2], ground_speed,
			a.gait == astra::Gait::fall ? "true" : "false", a.vel[2]);
	}

	Stats stats()
	{
		Stats st = g_stats;
		st.frames = compositor::astra_frames();
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
