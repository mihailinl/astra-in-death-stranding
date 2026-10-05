#pragma once
// Astra's picture in DS's: the engine's frames (shared memory, "MCPT" layout: premultiplied colour + linear depth +
// the camera each was rendered from) are uploaded into AstraPassthrough.fx's textures; the effect re-projects the
// newest one onto DS's current camera, depth-tests it against DS (and Minecraft, when its effect runs before) and
// lays DS's UI back over her.

namespace layer
{
	/// From DllMain: subscribes to ReShade's effect events.
	void install();
	/// Composite her this frame (her engine is linked and she is out).
	void set_active(bool active);
	/// DS's clip planes (its depth buffer -> metres).
	void set_host_planes(float near_clip, float far_clip);
	/// DS's camera in the engine's coordinates (Minecraft's angle convention, degrees; vertical fov): her picture is
	/// re-projected from the camera it was rendered with to this one.
	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z);
	/// How far she went since a frame (astralink::moved_since): an older picture of her moves along.
	void set_motion(bool (*moved_since)(unsigned long long frame, double out[3]));
	/// Where the engine says its frames are ("/dev/shm/astra-frame" or "Local\\AstraFrame"); reopened if it changed.
	/// (Present thread only.)
	void set_frames_at(const char *where);
	/// Frames uploaded so far.
	unsigned long long frames();
}
