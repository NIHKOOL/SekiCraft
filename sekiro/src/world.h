// Minecraft's blocks drawn in Sekiro's 3D view.
//
// Minecraft meshes its blocks itself (models, tint, ambient occlusion, light) and streams them
// through the render ring: a texture atlas, then one triangle list per 16x16x16 section. The
// worker thread collects them (consume); the render thread uploads and draws them from Sekiro's
// camera at Present (draw), before Minecraft's hand and HUD go on top.
#pragma once

#include "game.h"
#include "sekicraft_protocol.h"

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

namespace sekicraft::world
{
	// Worker thread: one render-ring message.
	void consume(std::uint32_t type, const std::uint8_t* payload, std::uint32_t bytes);

	// Render thread: draw every section into rtv (width x height) as seen by cam, hidden behind
	// Sekiro's own geometry where sceneDepth (its depth buffer) is given. feet: where the Minecraft
	// player stood when cam was written (its third-person body goes there).
	void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
		const game::CameraState& cam, const float feet[3], ID3D11ShaderResourceView* sceneDepth);

	// Worker thread: this frame's world entities (arrows, dropped items, cracks, the targeted block's
	// outline).
	void setWorldEntities(const proto::WorldEntities& entities);

	// Game thread, with each camera write: where the Minecraft player's feet are (Minecraft coords).
	void setDriveFeet(const float feet[3]);
	void driveFeet(float out[3]);

	// Light blocks from Sekiro's picture (on), or from Minecraft's light values only (off). midGrey is
	// the scene luminance (linear) that counts as normally lit: lower makes blocks brighter.
	void setSceneLighting(bool on, float midGrey);

	// Render thread: free GPU objects (unloading or device lost).
	void releaseGpu();

	std::string stats();
}
