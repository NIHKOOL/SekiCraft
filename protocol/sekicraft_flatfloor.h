// A temporary flat collision floor for the Minecraft side, until real Sekiro collision streams in.
// Used by tools/fakegame and by the Sekiro DLL's early phases.
#pragma once

#include "sekicraft_link.h"

#include <cmath>
#include <string>

namespace sekicraft
{
	// Clears collision (new epoch) and sends a square floor whose top is exactly at surfaceY,
	// spanning +-radiusRegions*8 blocks around (centerX, centerZ). MC coordinates.
	inline void sendFlatFloor(GameLink& link, std::uint32_t epoch, double centerX, double centerZ, double surfaceY, int radiusRegions = 6)
	{
		namespace proto = sekicraft::proto;
		constexpr int kRegion = 8;  // collision regions are 8-block cubes (SkyCollision.REGION_SIZE)
		auto floorDiv = [](int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); };

		link.writeCollision(proto::kColClear, &epoch, sizeof(epoch));

		const std::uint32_t triFlags = proto::kTriTerrain | proto::kTriDiggable |
		                               (std::uint32_t(proto::kDigGrass) << proto::kTriMaterialShift);
		const int surfaceBlock = int(std::floor(surfaceY));
		const int ryTri = floorDiv(surfaceBlock, kRegion);          // region holding the surface plane
		const int voxelY = surfaceBlock - 1;                         // full block just under the surface
		const int ryVox = floorDiv(voxelY, kRegion);
		const int rcx = floorDiv(int(std::floor(centerX)), kRegion);
		const int rcz = floorDiv(int(std::floor(centerZ)), kRegion);
		const float fy = float(surfaceY);

		std::string buf;
		for (int rx = rcx - radiusRegions; rx < rcx + radiusRegions; ++rx) {
			for (int rz = rcz - radiusRegions; rz < rcz + radiusRegions; ++rz) {
				const int x0 = rx * kRegion, z0 = rz * kRegion;
				const float fx0 = float(x0), fx1 = float(x0 + kRegion), fz0 = float(z0), fz1 = float(z0 + kRegion);

				// Exact triangles: the floor's top face, wound so the normal points up (out of the solid).
				proto::ColRegion tr{ x0, ryTri * kRegion, z0, x0 + kRegion - 1, ryTri * kRegion + kRegion - 1, z0 + kRegion - 1, epoch, 2 };
				proto::ColTri tris[2] = {
					{ { fx0, fy, fz0, fx0, fy, fz1, fx1, fy, fz0 }, triFlags },
					{ { fx1, fy, fz0, fx0, fy, fz1, fx1, fy, fz1 }, triFlags },
				};
				buf.assign(reinterpret_cast<const char*>(&tr), sizeof(tr));
				buf.append(reinterpret_cast<const char*>(tris), sizeof(tris));
				link.writeCollision(proto::kColTris, buf.data(), std::uint32_t(buf.size()));

				// Voxels: one solid layer just under the surface; everything else in the box is empty.
				// The box reaches one region further down: Minecraft holds the player until the
				// regions at and below their feet are known (SkyClient.holdUntilReady).
				proto::ColRegion vr{ x0, (ryVox - 1) * kRegion, z0, x0 + kRegion - 1, ryTri * kRegion + kRegion - 1, z0 + kRegion - 1, epoch, 0 };
				buf.assign(reinterpret_cast<const char*>(&vr), sizeof(vr));
				for (int x = x0; x < x0 + kRegion; ++x) {
					for (int z = z0; z < z0 + kRegion; ++z) {
						proto::ColBlock b{ x, voxelY, z, 0, {} };
						for (auto& layer : b.bits)
							layer = ~0ull;
						buf.append(reinterpret_cast<const char*>(&b), sizeof(b));
						++vr.count;
					}
				}
				std::memcpy(buf.data(), &vr, sizeof(vr));
				link.writeCollision(proto::kColRegion, buf.data(), std::uint32_t(buf.size()));
			}
		}
	}
}
