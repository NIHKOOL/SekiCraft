#include "scenedepth.h"

#include "log.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace sekicraft::scenedepth
{
	namespace
	{
		using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
		ClearDsvFn       g_origClear = nullptr;
		void*            g_target = nullptr;
		std::atomic<int> g_inFlight{ 0 };

		// Depth textures cleared since the last Present (any thread: deferred contexts too).
		struct Cleared
		{
			ID3D11Texture2D* tex;  // AddRef'd
			UINT             w, h;
			DXGI_FORMAT      format;
			UINT             bind;
			float            clearDepth;
		};
		std::mutex           g_mutex;
		std::vector<Cleared> g_cleared;
		std::atomic<int>     g_clearCalls{ 0 };

		// Render thread.
		ID3D11Texture2D*          r_tex = nullptr;  // the chosen scene depth (AddRef'd)
		ID3D11ShaderResourceView* r_srv = nullptr;
		float                     r_clearDepth = NAN;
		std::set<std::pair<UINT, int>> r_logged;    // (size, format) combinations already logged
		// Centre readback: a 1x1 R32_FLOAT target, copied to staging, mapped a frame later.
		ID3D11Texture2D*          r_probeRt = nullptr;
		ID3D11RenderTargetView*   r_probeRtv = nullptr;
		ID3D11Texture2D*          r_probeStaging = nullptr;
		ID3D11VertexShader*       r_vs = nullptr;
		ID3D11PixelShader*        r_ps = nullptr;
		ID3D11Buffer*             r_cb = nullptr;
		bool                      r_probePending = false;
		std::atomic<float>        g_centre{ NAN };
		std::atomic<int>          g_frames{ 0 }, g_framesWithDepth{ 0 };

		constexpr char kProbeShader[] = R"(
cbuffer P : register(b0) { uint2 centre; uint2 pad; };
Texture2D<float> depth : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float PSMain() : SV_Target { return depth.Load(int3(centre, 0)); }
)";

		template <class T>
		void release(T*& p)
		{
			if (p) {
				p->Release();
				p = nullptr;
			}
		}

		void STDMETHODCALLTYPE hookClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth, UINT8 stencil)
		{
			++g_inFlight;
			++g_clearCalls;
			if (dsv && (flags & D3D11_CLEAR_DEPTH)) {
				ID3D11Resource* res = nullptr;
				dsv->GetResource(&res);
				ID3D11Texture2D* tex = nullptr;
				if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
					D3D11_TEXTURE2D_DESC d{};
					tex->GetDesc(&d);
					std::lock_guard lock(g_mutex);
					if (g_cleared.size() < 64)
						g_cleared.push_back({ tex, d.Width, d.Height, d.Format, d.BindFlags, depth });
					else
						tex->Release();
				}
				release(res);
			}
			g_origClear(ctx, dsv, flags, depth, stencil);
			--g_inFlight;
		}

		// Also every depth buffer bound for drawing (engines that "clear" depth by drawing never call
		// ClearDepthStencilView). Remembered once per DSV per frame.
		using OMSetRTsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		OMSetRTsFn             g_origOMSet = nullptr;     // immediate context
		OMSetRTsFn             g_origOMSetD = nullptr;    // deferred contexts
		void*                  g_targetOMSet = nullptr;
		void*                  g_targetOMSetD = nullptr;
		std::atomic<int>       g_bindCalls{ 0 };
		std::vector<ID3D11DepthStencilView*> g_boundThisFrame;  // g_mutex

		void noteDepthBound(ID3D11DepthStencilView* dsv);

		void STDMETHODCALLTYPE hookOMSet(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
		{
			++g_inFlight;
			++g_bindCalls;
			noteDepthBound(dsv);
			g_origOMSet(ctx, n, rtvs, dsv);
			--g_inFlight;
		}

		std::atomic<int> g_bindDeferredCalls{ 0 };

		void STDMETHODCALLTYPE hookOMSetD(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
		{
			++g_inFlight;
			++g_bindDeferredCalls;
			noteDepthBound(dsv);
			g_origOMSetD(ctx, n, rtvs, dsv);
			--g_inFlight;
		}

		using OMSetRTsUavsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
			ID3D11UnorderedAccessView* const*, const UINT*);
		OMSetRTsUavsFn   g_origOMSetUav = nullptr;
		void*            g_targetOMSetUav = nullptr;
		std::atomic<int> g_bindUavCalls{ 0 };

		void STDMETHODCALLTYPE hookOMSetUav(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
			UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
		{
			++g_inFlight;
			++g_bindUavCalls;
			// D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL leaves the bound targets alone.
			if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
				noteDepthBound(dsv);
			g_origOMSetUav(ctx, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
			--g_inFlight;
		}

		void noteDepthBound(ID3D11DepthStencilView* dsv)
		{
			if (dsv) {
				bool seen;
				{
					std::lock_guard lock(g_mutex);
					seen = std::find(g_boundThisFrame.begin(), g_boundThisFrame.end(), dsv) != g_boundThisFrame.end();
					if (!seen && g_boundThisFrame.size() < 256)
						g_boundThisFrame.push_back(dsv);
				}
				if (!seen) {
					ID3D11Resource* res = nullptr;
					dsv->GetResource(&res);
					ID3D11Texture2D* tex = nullptr;
					if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
						D3D11_TEXTURE2D_DESC d{};
						tex->GetDesc(&d);
						std::lock_guard lock(g_mutex);
						if (g_cleared.size() < 64)
							g_cleared.push_back({ tex, d.Width, d.Height, d.Format, d.BindFlags, NAN });
						else
							tex->Release();
					}
					release(res);
				}
			}
		}

		DXGI_FORMAT srvFormat(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
			default: return DXGI_FORMAT_UNKNOWN;
			}
		}

		bool initProbe(ID3D11Device* device)
		{
			if (r_ps)
				return true;
			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
			if (FAILED(D3DCompile(kProbeShader, sizeof(kProbeShader) - 1, "sekicraft_depthprobe", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errors)) ||
				FAILED(D3DCompile(kProbeShader, sizeof(kProbeShader) - 1, "sekicraft_depthprobe", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errors))) {
				logf("depth: probe shader failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				release(errors);
				release(vsBlob);
				return false;
			}
			device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &r_vs);
			device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &r_ps);
			release(vsBlob);
			release(psBlob);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = td.Height = 1;
			td.MipLevels = td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R32_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_RENDER_TARGET;
			device->CreateTexture2D(&td, nullptr, &r_probeRt);
			if (r_probeRt)
				device->CreateRenderTargetView(r_probeRt, nullptr, &r_probeRtv);
			td.BindFlags = 0;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			device->CreateTexture2D(&td, nullptr, &r_probeStaging);
			D3D11_BUFFER_DESC cbd{};
			cbd.ByteWidth = 16;
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			device->CreateBuffer(&cbd, nullptr, &r_cb);
			return r_vs && r_ps && r_probeRtv && r_probeStaging && r_cb;
		}

		// Reads the centre texel of the scene depth (state is saved and restored by the caller).
		void probeCentre(ID3D11DeviceContext* ctx, UINT w, UINT h)
		{
			if (r_probePending) {
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(ctx->Map(r_probeStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
					g_centre = *static_cast<const float*>(m.pData);
					ctx->Unmap(r_probeStaging, 0);
					r_probePending = false;
				}
			}
			if (r_probePending || !r_srv)
				return;
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(r_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return;
			const UINT centre[4] = { w / 2, h / 2, 0, 0 };
			std::memcpy(m.pData, centre, sizeof(centre));
			ctx->Unmap(r_cb, 0);
			const D3D11_VIEWPORT vp{ 0, 0, 1, 1, 0, 1 };
			ctx->OMSetRenderTargets(1, &r_probeRtv, nullptr);
			ctx->RSSetViewports(1, &vp);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(r_vs, nullptr, 0);
			ctx->PSSetShader(r_ps, nullptr, 0);
			ctx->PSSetShaderResources(0, 1, &r_srv);
			ctx->PSSetConstantBuffers(0, 1, &r_cb);
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView* none = nullptr;
			ctx->PSSetShaderResources(0, 1, &none);
			ctx->CopyResource(r_probeStaging, r_probeRt);
			r_probePending = true;
		}
	}

	bool install(void* clearDepthStencilView, void* omSetRenderTargets, void* omSetRenderTargetsAndUavs, void* deferredOmSetRenderTargets)
	{
		if (deferredOmSetRenderTargets && deferredOmSetRenderTargets != omSetRenderTargets) {
			g_targetOMSetD = deferredOmSetRenderTargets;
			if (MH_CreateHook(g_targetOMSetD, reinterpret_cast<void*>(&hookOMSetD), reinterpret_cast<void**>(&g_origOMSetD)) != MH_OK ||
				MH_EnableHook(g_targetOMSetD) != MH_OK) {
				logf("depth: couldn't hook the deferred context's OMSetRenderTargets");
				g_targetOMSetD = nullptr;
			} else {
				logf("depth: deferred contexts have their own OMSetRenderTargets (%p): hooked", g_targetOMSetD);
			}
		}
		g_targetOMSetUav = omSetRenderTargetsAndUavs;
		if (g_targetOMSetUav && (MH_CreateHook(g_targetOMSetUav, reinterpret_cast<void*>(&hookOMSetUav), reinterpret_cast<void**>(&g_origOMSetUav)) != MH_OK ||
			MH_EnableHook(g_targetOMSetUav) != MH_OK)) {
			logf("depth: couldn't hook OMSetRenderTargetsAndUnorderedAccessViews");
			g_targetOMSetUav = nullptr;
		}
		g_targetOMSet = omSetRenderTargets;
		if (g_targetOMSet && (MH_CreateHook(g_targetOMSet, reinterpret_cast<void*>(&hookOMSet), reinterpret_cast<void**>(&g_origOMSet)) != MH_OK ||
			MH_EnableHook(g_targetOMSet) != MH_OK)) {
			logf("depth: couldn't hook OMSetRenderTargets");
			g_targetOMSet = nullptr;
		}
		g_target = clearDepthStencilView;
		if (!g_target || MH_CreateHook(g_target, reinterpret_cast<void*>(&hookClear), reinterpret_cast<void**>(&g_origClear)) != MH_OK ||
			MH_EnableHook(g_target) != MH_OK) {
			logf("depth: couldn't hook ClearDepthStencilView");
			g_target = nullptr;
			return false;
		}
		logf("depth: ClearDepthStencilView hooked (%p)", g_target);
		return true;
	}

	void uninstall()
	{
		if (!g_target)
			return;
		MH_DisableHook(g_target);
		MH_RemoveHook(g_target);
		for (void** t : { &g_targetOMSet, &g_targetOMSetUav, &g_targetOMSetD }) {
			if (*t) {
				MH_DisableHook(*t);
				MH_RemoveHook(*t);
				*t = nullptr;
			}
		}
		for (int i = 0; i < 200 && g_inFlight > 0; ++i)
			Sleep(5);
		g_target = nullptr;
		std::lock_guard lock(g_mutex);
		for (auto& c : g_cleared)
			release(c.tex);
		g_cleared.clear();
	}

	void onPresent(ID3D11Device* device, ID3D11DeviceContext* ctx, unsigned width, unsigned height, bool probe)
	{
		std::vector<Cleared> cleared;
		{
			std::lock_guard lock(g_mutex);
			cleared.swap(g_cleared);
			g_boundThisFrame.clear();
		}
		++g_frames;
		static bool loggedVtable = false;
		if (!loggedVtable) {
			loggedVtable = true;
			void** vt = *reinterpret_cast<void***>(ctx);
			logf("depth: Sekiro's context: ClearDepthStencilView %p (hooked %p), OMSetRenderTargets %p (hooked %p)", vt[53], g_target, vt[33], g_targetOMSet);
		}
		// The scene depth: the first back-buffer-sized depth texture cleared this frame that can be
		// read as a texture.
		ID3D11Texture2D* pick = nullptr;
		float pickClear = NAN;
		for (auto& c : cleared) {
			const auto key = std::make_pair(c.w * 65536u + c.h, int(c.format));
			if (r_logged.size() < 32 && r_logged.insert(key).second)
				logf("depth: Sekiro clears a %ux%u depth buffer, format %d, bind 0x%x, clear value %.3f", c.w, c.h, int(c.format), c.bind, c.clearDepth);
			if (!pick && c.w == width && c.h == height && (c.bind & D3D11_BIND_SHADER_RESOURCE) && srvFormat(c.format) != DXGI_FORMAT_UNKNOWN) {
				pick = c.tex;
				pickClear = c.clearDepth;
				continue;  // keep its reference
			}
			release(c.tex);
		}
		if (pick != r_tex) {
			release(r_srv);
			release(r_tex);
			r_tex = pick;
			if (r_tex) {
				D3D11_TEXTURE2D_DESC d{};
				r_tex->GetDesc(&d);
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				sd.Format = srvFormat(d.Format);
				sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				sd.Texture2D.MipLevels = 1;
				if (FAILED(device->CreateShaderResourceView(r_tex, &sd, &r_srv)))
					logf("depth: couldn't view the scene depth (format %d)", int(d.Format));
			}
		} else if (pick) {
			pick->Release();  // same texture as last frame: drop the extra reference
		}
		r_clearDepth = pickClear;
		if (r_srv) {
			++g_framesWithDepth;
			if (probe && initProbe(device))
				probeCentre(ctx, width, height);
		}
	}

	ID3D11ShaderResourceView* view()
	{
		return r_srv;
	}

	float centreDepth()
	{
		return g_centre;
	}

	void releaseGpu()
	{
		release(r_srv);
		release(r_tex);
		release(r_probeRtv);
		release(r_probeRt);
		release(r_probeStaging);
		release(r_vs);
		release(r_ps);
		release(r_cb);
		r_probePending = false;
	}

	std::string stats()
	{
		char buf[240];
		std::snprintf(buf, sizeof(buf), "depth: %d depth clears, %d+%d target binds, %d deferred binds, %d of %d frames with scene depth, centre depth %.6f (clear value %.3f)",
			g_clearCalls.exchange(0), g_bindCalls.exchange(0), g_bindUavCalls.exchange(0), g_bindDeferredCalls.exchange(0), g_framesWithDepth.exchange(0), g_frames.exchange(0), g_centre.load(), r_clearDepth);
		return buf;
	}
}
