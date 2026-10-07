// Sekiro's scene depth buffer, so Minecraft's blocks hide behind Sekiro's walls.
//
// Sekiro clears its depth buffers every frame; a hook on ID3D11DeviceContext::ClearDepthStencilView
// sees them all, and the one the size of the back buffer is the scene's. At Present it is still
// full of the frame's depth, so it can be read as a texture.
#pragma once

#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace sekicraft::scenedepth
{
	// The vtable entries, from a throwaway context.
	bool install(void* clearDepthStencilView, void* omSetRenderTargets, void* omSetRenderTargetsAndUavs, void* deferredOmSetRenderTargets);
	void uninstall();

	// Render thread, in Present: choose this frame's scene depth and (re)build its view. probe: also
	// read back the centre texel (changes pipeline state: only when the caller restores it).
	void onPresent(ID3D11Device* device, ID3D11DeviceContext* context, unsigned width, unsigned height, bool probe);

	// The scene depth as a texture (r = stored depth), or nullptr. Valid until the next onPresent.
	ID3D11ShaderResourceView* view();

	// Diagnostics: the stored depth at the screen centre, read back a frame late (NAN if none).
	float centreDepth();

	void releaseGpu();
	std::string stats();
}
