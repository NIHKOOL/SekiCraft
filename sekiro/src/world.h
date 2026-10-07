// Minecraft's blocks drawn in Sekiro's 3D view.
//
// Minecraft meshes its blocks itself (models, tint, ambient occlusion, light) and streams them
// through the render ring: a texture atlas, then one triangle list per 16x16x16 section. The
// worker thread collects them (consume); the render thread uploads and draws them from Sekiro's
// camera at Present (draw), before Minecraft's hand and HUD go on top.
#pragma once

#include "game.h"

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
	// Sekiro's own geometry where sceneDepth (its depth buffer) is given.
	void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, unsigned width, unsigned height,
		const game::CameraState& cam, ID3D11ShaderResourceView* sceneDepth);

	// Render thread: free GPU objects (unloading or device lost).
	void releaseGpu();

	std::string stats();
}
