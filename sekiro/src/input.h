// Input bridge: Sekiro reads keyboard and mouse through DirectInput 8. While Minecraft has
// control, the hooks hand every key and mouse movement to Minecraft and give Sekiro an idle
// keyboard and mouse, so Wolf's own controls and camera stay still.
#pragma once

#include "sekicraft_protocol.h"

#include <string>
#include <vector>

namespace sekicraft::input
{
	bool install();
	void uninstall();

	// true: input goes to Minecraft (Sekiro sees nothing). false: Sekiro gets its input as usual.
	void setRouting(bool toMinecraft);
	bool routing();

	struct Pending
	{
		std::vector<proto::InputEvent> events;  // keys, buttons, scroll (Minecraft codes)
		float lookDx = 0, lookDy = 0;           // raw mouse counts since the last take
		bool  escapePressed = false;            // Esc: hand control back to Sekiro
	};

	// Everything gathered since the last call (called once per frame by the core).
	Pending take();

	// How often Sekiro called each hooked input function since the last call (diagnostics).
	std::string callStats();

	// Called about once per game frame on Sekiro's own thread (from its keyboard poll), whether or
	// not input is routed. Physics queries (ray casts) must run here, not on our worker thread.
	using GameThreadTick = void (*)();
	void setGameThreadTick(GameThreadTick tick);
}
