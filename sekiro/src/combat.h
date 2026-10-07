// Combat between the Minecraft player and Sekiro's characters.
//
// Every character near Wolf is sent to Minecraft as an actor (SkyCraft's protocol), where it gets
// an invisible, hittable stand-in. Minecraft weapons hit those like any mob, and the damage comes
// back as a hit event: applied to the real character's HP and posture on Sekiro's thread.
//
// Sekiro's attacks still land on Wolf (he's where the Minecraft player is). While Minecraft has
// control Wolf can't die (NoDeath); the HP he loses becomes Minecraft damage, in proportion to his
// maximum (10% of Wolf's health = 10% of Minecraft's), and he's healed back up.
#pragma once

#include "sekicraft_link.h"

#include <cstdint>

#include <string>

namespace sekicraft::combat
{
	// Worker thread, every loop: find characters, send the actor table, forward hits on Wolf.
	void update(GameLink& link, bool minecraftControls);

	// Worker thread: a Minecraft event (hits on stand-ins).
	void onEvent(const proto::McEvent& ev);

	// Sekiro's thread, once per frame: apply queued hits, watch Wolf's HP.
	void tickGameThread(bool minecraftControls);

	// Deathblows: a Minecraft hit on an enemy whose posture is broken (or a boss with its HP gone)
	// doesn't do damage; it asks for Sekiro's own deathblow, which the core lets Sekiro perform.
	struct DeathblowRequest
	{
		std::uint32_t  handle = 0;
		std::uintptr_t chr = 0;
		int            livesBefore = 0;
	};
	bool takeDeathblowRequest(DeathblowRequest& out);  // worker
	bool takeMinecraftDeath();                         // worker: the Minecraft player died since last asked
	void killWolf();                                   // worker: Wolf dies (on Sekiro's thread, next frame)
	bool deathblowDone(const DeathblowRequest& r);     // worker: a life gone, or dead
	void forceDeathblow(const DeathblowRequest& r);    // worker: Sekiro didn't do it; do it in memory

	void reset();

	// Unloading: give Wolf his mortality back (the game thread may not tick again).
	void shutdown();

	std::string stats();
}
