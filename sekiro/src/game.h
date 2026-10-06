// What SekiCraft knows about Sekiro 1.06's memory, and the Sekiro <-> Minecraft coordinate mapping.
//
// Pointer paths come from the community Sekiro Practice cheat table (ElaDiDu/Sekiro-Practice-CT)
// and were checked live with tools/re (sekiro_peek.py, record.py).
#pragma once

#include <cstdint>
#include <numbers>

namespace sekicraft::game
{
	// ---- coordinates --------------------------------------------------------------------------
	// Sekiro: Y up, 1 unit = 1 m, left-handed (a left turn decreases atan2(dx, dz)).
	// Minecraft: Y up, 1 block, right-handed. One axis flips; 1 m = 1 block.
	// Wolf's facing angle θ (radians) has forward = (-sin θ, -cos θ) in Sekiro's (x, z), which after
	// the flip is (-sin θ, cos θ): exactly Minecraft's forward for yaw = θ in degrees.

	struct Vec3
	{
		float x, y, z;
	};

	inline Vec3 toMc(Vec3 s) { return { s.x, s.y, -s.z }; }
	inline Vec3 fromMc(Vec3 m) { return { m.x, m.y, -m.z }; }
	inline float yawToMc(float theta) { return theta * float(180.0 / std::numbers::pi); }
	inline float yawFromMc(float degrees) { return degrees * float(std::numbers::pi / 180.0); }

	// ---- memory -------------------------------------------------------------------------------

	// Finds the globals. False if this isn't a sekiro.exe we understand.
	bool init();

	// Wolf's physics module, or 0 while there is no player (title screen, loading).
	std::uintptr_t playerPhysics();

	struct PlayerState
	{
		Vec3  pos;    // Sekiro coords, feet
		float theta;  // facing, radians
	};

	bool readPlayer(PlayerState& out);

	// Moves Wolf (what the cheat table's teleport does). Only the physics position is written.
	bool writePlayerPos(Vec3 pos);
	bool writePlayerTheta(float theta);

	// Wolf's "Draw" flag (player ChrIns +0x1A11, bit 3; 1 = drawn). Hidden while Minecraft has
	// control, so the first-person camera doesn't look out through his head.
	bool setPlayerDrawn(bool drawn);

	// ---- physics queries ----------------------------------------------------------------------
	// FrpgCastRay (sekiro.exe+0x94CC50 on 1.06) against FrpgHavokMan's physics world
	// ([[sekiro.exe+0x3D6D640]+0x98]), filter 0x4E as the community tools use it. Interface facts
	// from SekiroTool (MIT, Shilkey & Centz). Call it on the game's own thread only.

	struct RayHit
	{
		Vec3  pos;       // Sekiro coords
		Vec3  normal;
		float fraction;  // along delta
	};

	bool castRayReady();
	bool castRay(Vec3 start, Vec3 delta, RayHit& out);

	// ---- camera -------------------------------------------------------------------------------
	// Sekiro's debug free camera: [[FieldArea]+0x20]+0xE8, a 4x4 matrix at +0x10 (rows right, up,
	// forward, position; left-handed: right x up = forward), vertical FOV (radians) at +0x50.
	// Free-cam mode byte at [FieldArea]+0x20 +0xE0: 0 normal, 1 free camera.

	bool setFreeCamera(bool on);

	// Puts the free camera at pos (Sekiro coords) looking along facing theta (Wolf's convention)
	// and pitch (radians, positive looks down, like Minecraft).
	bool writeCamera(Vec3 pos, float theta, float pitch, float fovRadians);

	// Exception-safe memory access (a bad pointer during loading must never crash the game).
	bool readBytes(std::uintptr_t addr, void* out, std::size_t n);
	bool writeBytes(std::uintptr_t addr, const void* in, std::size_t n);

	template <class T>
	bool read(std::uintptr_t addr, T& out)
	{
		return readBytes(addr, &out, sizeof(T));
	}
}
