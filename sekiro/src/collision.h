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

namespace sekicraft::collision
{
	// Game thread, about once per frame: casts rays for the current job within a time budget.
	void tickGameThread();

	// Worker thread, every loop: follows the player (MC coords), queues regions, and sends what's
	// finished. epoch is SkyState::collisionEpoch; it bumps (and Minecraft drops everything) when
	// the player teleports or the world changes.
	void update(GameLink& link, bool inGame, game::Vec3 playerMc, std::uint32_t& epoch);

	// Forget everything (e.g. the core is about to unload).
	void reset();

	std::string stats();
}
