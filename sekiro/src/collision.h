// Sekiro's collision for Minecraft's physics, measured with the engine's own ray casts.
//
// Sekiro's collision meshes are two-sided and its ray hits don't say which side is solid, so
// nothing here guesses at volumes:
//  - vertical rays every 0.5 m find every surface in each column (multi-hit);
//  - surfaces of neighbouring columns at similar heights become floor/ceiling triangles;
//  - where neighbouring columns' heights don't line up, horizontal rays across the gap find the
//    actual walls (a cliff stops them, a doorway or an awning doesn't);
//  - voxels are a thin slab under each surface (for Minecraft's "is the ground here yet" checks).
// Regions are 8-block cubes, sent nearest first. Ray casts run on Sekiro's own thread
// (tickGameThread); building and sending runs on the worker (update).
#pragma once

#include "game.h"
#include "sekicraft_link.h"

#include <string>
#include <vector>

namespace sekicraft::collision
{
	// Game thread, about once per frame: casts rays for the current job within a time budget.
	void tickGameThread();

	// Worker thread, every loop: follows the player (MC coords), queues regions, and sends what's
	// finished. epoch is SkyState::collisionEpoch; it bumps (and Minecraft drops everything) when
	// the player teleports or the world changes.
	void update(GameLink& link, bool inGame, game::Vec3 playerMc, std::uint32_t& epoch);

	// Worker thread: Minecraft's dug blocks for one section (render ring kRenDug), and "everything
	// is being sent again" (kRenClearAll). Sekiro's geometry in a dug block is cut out of what
	// Minecraft collides with.
	void onDug(const std::uint8_t* payload, std::uint32_t bytes);
	void clearDug();

	// Changes whenever the dug blocks do. Worker thread.
	unsigned dugGeneration();
	// Is this Minecraft block dug? Worker thread.
	bool dugAt(int x, int y, int z);
	// Worker thread: n x n x n bytes, 1 where the block at origin + (x, y, z) is dug, laid out
	// x + n * (y + n * z) (a Texture3D's own layout).
	void dugWindow(int ox, int oy, int oz, int n, std::vector<std::uint8_t>& out);

	// Forget everything (e.g. the core is about to unload).
	void reset();

	std::string stats();
}
