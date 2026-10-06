// fakegame: stands in for Sekiro so the Minecraft side can be tested without the game.
//
// It does what sekicraft.dll will do inside sekiro.exe, minus the game:
//   - creates the shared memory (Local\SekiCraft_v1) and keeps a heartbeat going
//   - reports a player standing on a flat floor (SkyState, kSkyInGame)
//   - streams that floor as collision (exact triangles + 8x8x8 voxel blocks)
//   - drains what Minecraft sends back (render ring, events)
//   - optionally holds W for a few seconds once Minecraft is in the world (--walk)
//   - prints Minecraft's player state (McState) once a second
//
// Usage: fakegame [--walk] [--seconds N]
#include "sekicraft_flatfloor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace proto = sekicraft::proto;

namespace
{
	constexpr int    kFloorY = 64;       // top surface of the floor (MC block y)
	constexpr double kSpawnX = 0.5, kSpawnZ = 0.5;
	constexpr int    kScancodeW = 26;    // SDL scancode for W
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);  // print immediately, even when piped to a log

	bool walk = false;
	int seconds = 0;  // 0 = run until Ctrl+C
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--walk"))
			walk = true;
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = std::atoi(argv[++i]);
	}

	sekicraft::GameLink link;
	switch (link.open()) {
	case sekicraft::GameLink::OpenResult::kFailed:
		std::printf("fakegame: couldn't create the shared memory (Windows error %lu)\n", GetLastError());
		return 1;
	case sekicraft::GameLink::OpenResult::kOtherGameLive:
		std::printf("fakegame: another game side (pid %u) is live (is Sekiro with sekicraft.dll running?)\n", link.otherGamePid());
		return 1;
	case sekicraft::GameLink::OpenResult::kTookOver:
		std::printf("fakegame: taking over the existing mapping (Minecraft kept it open)\n");
		break;
	case sekicraft::GameLink::OpenResult::kCreated:
		break;
	}

	proto::SkyState st{};
	st.flags = proto::kSkyInGame;
	st.worldId = 0x0B000000;  // fake area id (Sekiro m11_00 Ashina would be area 11)
	st.collisionEpoch = link.previousCollisionEpoch() + 1;
	st.posX = kSpawnX;
	st.posY = kFloorY;
	st.posZ = kSpawnZ;
	st.teleportSeq = link.previousTeleportSeq() + 1;
	st.viewportW = 1280;
	st.viewportH = 720;
	st.gameHour = 12.0f;
	link.writeSkyState(st);
	sekicraft::sendFlatFloor(link, st.collisionEpoch, kSpawnX, kSpawnZ, kFloorY);

	std::printf("fakegame: shared memory %ls ready (protocol v%u, %llu MB). Waiting for Minecraft...\n",
		proto::kMappingName, proto::kVersion, (unsigned long long)(proto::kMappingBytes >> 20));

	const ULONGLONG start = GetTickCount64();
	ULONGLONG lastPrint = 0, walkStart = 0;
	bool linked = false, walking = false, walked = false;
	double walkFromX = 0, walkFromZ = 0;
	std::uint64_t renderBytes = 0, lastFrame = 0;

	for (;;) {
		const ULONGLONG now = GetTickCount64();
		link.heartbeat();

		const bool mcAlive = link.minecraftAlive();
		if (mcAlive && !linked) {
			linked = true;
			std::printf("fakegame: Minecraft linked (pid %u)\n", link.minecraftPid());
		} else if (!mcAlive && linked) {
			linked = false;
			std::printf("fakegame: Minecraft heartbeat lost\n");
		}

		proto::McState mc{};
		const bool haveMc = mcAlive && link.readMcState(mc) && (mc.flags & proto::kMcInWorld);

		// The real game side draws the render ring and acts on events; we only keep them flowing.
		renderBytes += link.drainRender();
		link.drainEvents([](const proto::McEvent& ev) {
			std::printf("  event: type %u formId %08X a %.2f b %.2f c %.2f d %.2f\n", ev.type, ev.formId, ev.a, ev.b, ev.c, ev.d);
		});

		// One SkyState write per game frame: Minecraft paces its frames on the seqlock advancing.
		link.writeSkyState(st);

		if (walk && haveMc && mc.teleportAck == st.teleportSeq && !walking && !walked && (mc.flags & proto::kMcOnGround)) {
			walking = true;
			walkStart = now;
			walkFromX = mc.x;
			walkFromZ = mc.z;
			std::printf("fakegame: holding W for 3 s\n");
			link.sendInput(proto::kInKey, kScancodeW, 1);
		}
		if (walking && now - walkStart > 3000) {
			link.sendInput(proto::kInKey, kScancodeW, 0);
			walking = false;
			walked = true;
			const double dx = mc.x - walkFromX, dz = mc.z - walkFromZ;
			std::printf("fakegame: released W; moved %.2f blocks (dx %.2f, dz %.2f), y %.3f, onGround %d\n",
				std::sqrt(dx * dx + dz * dz), dx, dz, mc.y, (mc.flags & proto::kMcOnGround) ? 1 : 0);
		}

		if (now - lastPrint >= 1000) {
			lastPrint = now;
			if (haveMc) {
				std::printf("  mc: pos (%.2f, %.3f, %.2f) yaw %.1f flags 0x%02x  %llu fps  render ring %.1f MB total\n",
					mc.x, mc.y, mc.z, mc.yaw, mc.flags, (unsigned long long)(mc.frameCounter - lastFrame), double(renderBytes) / (1 << 20));
				lastFrame = mc.frameCounter;
			} else if (linked) {
				std::printf("  mc: linked, not in the world yet\n");
			}
		}

		if (seconds > 0 && now - start > ULONGLONG(seconds) * 1000)
			break;
		Sleep(16);
	}
	return 0;
}
