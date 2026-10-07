// Draws Minecraft's hand, HUD and screens over Sekiro's picture, from a hook on DXGI Present.
#pragma once

#include "sekicraft_link.h"

#include <cstdint>
#include <string>

namespace sekicraft::overlay
{
	bool install();    // hooks IDXGISwapChain::Present (MinHook must be initialized)
	void uninstall();  // unhooks and waits until no frame is inside the hook

	struct Settings
	{
		bool  show = false;      // draw Minecraft's layer at all (Minecraft has control)
		bool  world = false;     // draw Minecraft's blocks in the 3D view (from the free camera)
		bool  cursor = false;    // a Minecraft screen is open: draw a mouse cursor
		float cursorX = 0, cursorY = 0;  // in overlay pixels
		bool  crosshair = false; // draw Minecraft's crosshair with its invert blend
		int   guiScale = 0;
	};

	// From the worker: the link to read frames from (nullptr: none) and what to draw.
	void setLink(GameLink* link);
	void update(const Settings& settings);

	std::string stats();

	// Draw blocks with the camera from this many frames ago (to match the frame Sekiro rendered).
	void setCameraLag(int frames);

	// Sekiro's back buffer size as last seen in Present (0 until the first frame).
	void viewport(std::uint32_t& width, std::uint32_t& height);
}
