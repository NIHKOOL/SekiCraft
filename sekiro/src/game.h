// What SekiCraft knows about Sekiro 1.06's memory, and the Sekiro <-> Minecraft coordinate mapping.
//
// Pointer paths come from the community Sekiro Practice cheat table (ElaDiDu/Sekiro-Practice-CT)
// and were checked live with tools/re (sekiro_peek.py, record.py).
#pragma once

#include <cstdint>
#include <vector>
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

	// ---- characters (ChrIns) ------------------------------------------------------------------
	// Every character (Wolf, enemies, NPCs, invisible helpers) is a ChrIns. Interface facts from
	// SekiroTool (MIT): handle +0x8, character id +0x68, modules +0x1FF8 -> data +0x18 (HP +0x130,
	// max +0x134, posture +0x148, max +0x14C, flags +0x228) and physics +0x68 (facing +0x74,
	// position +0x80). Team byte +0x74 and the physics module's pointer back to its ChrIns (+0x8)
	// checked live. Enemies are team 6.

	struct Character
	{
		std::uintptr_t chr = 0, data = 0, physics = 0;
		std::uint32_t  handle = 0, characterId = 0;
		int            team = 0;
		Vec3           pos{};  // Sekiro coords, feet
		float          theta = 0;
		int            hp = 0, maxHp = 0, posture = 0, maxPosture = 0;
	};

	// Reads a character, checking that the pointer really is one (its physics module points back).
	bool readCharacter(std::uintptr_t chr, Character& out);

	// Wolf's ChrIns (0 when there is none).
	std::uintptr_t player();

	// Finds every loaded character by scanning the memory around the characters already known
	// (Sekiro's own update lists miss sleeping ones). A few ms: call about once a second.
	void scanCharacters(std::vector<std::uintptr_t>& out);

	// HP and posture, through Sekiro's own setters where they're there (1.06), else plain writes.
	// Game thread only.
	void setHp(const Character& c, int hp);
	void setPosture(const Character& c, int posture);
	// Deathblow "lives" left (data +0x25C): the red dots over a boss's health bar.
	int  lives(const Character& c);
	void setLives(const Character& c, int lives);
	// Restores posture to full (Sekiro's own setter, refill mode).
	void refillPosture(const Character& c);
	// The NoDeath flag (data +0x228, bit 2): HP can't reach 0.
	bool setNoDeath(const Character& c, bool on);

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

	struct CameraState
	{
		float world[16];  // rows: right, up, forward, position (Sekiro coords)
		float fov;        // vertical, radians
		float aspect, nearZ, farZ;
	};

	// The free camera as it is now (what Sekiro renders while Minecraft has control).
	bool readFreeCamera(CameraState& out);

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
