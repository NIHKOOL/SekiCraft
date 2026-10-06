// Compositing adapted from SkyCraft's Skyrim-side Overlay.cpp (MIT, chasmlol): the same shader,
// blend states and state save/restore, hooked through MinHook instead of SKSE.
#include "overlay.h"

#include "log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace sekicraft::overlay
{
	namespace
	{
		using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
		PresentFn         g_origPresent = nullptr;
		void*             g_targetPresent = nullptr;
		std::atomic<int>  g_inFlight{ 0 };
		std::atomic<bool> g_enabled{ false };

		std::atomic<GameLink*>     g_link{ nullptr };
		std::mutex                 g_settingsMutex;
		Settings                   g_settings;
		std::atomic<std::uint32_t> g_viewW{ 0 }, g_viewH{ 0 };

		// Render-thread state (only touched inside Present).
		ID3D11Device*             device = nullptr;
		ID3D11DeviceContext*      context = nullptr;
		ID3D11Texture2D*          texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		UINT                      texW = 0, texH = 0;
		ID3D11VertexShader*       vs = nullptr;
		ID3D11PixelShader*        ps = nullptr;
		ID3D11PixelShader*        psInvert = nullptr;
		ID3D11BlendState*         blend = nullptr;
		ID3D11BlendState*         invertBlend = nullptr;
		ID3D11SamplerState*       sampler = nullptr;
		ID3D11RasterizerState*    raster = nullptr;
		ID3D11DepthStencilState*  depth = nullptr;
		ID3D11Buffer*             params = nullptr;
		bool                      haveFrame = false;
		bool                      flipY = true;
		bool                      initFailed = false;

		struct alignas(16) Params
		{
			float cursor[2];
			float viewport[2];
			float cursorOn;
			float flipY;
			float pad[2];
			float invertRect[4];  // back buffer pixels: x0, y0, x1, y1 (empty: none)
		};

		constexpr char kShader[] = R"(
cbuffer Params : register(b0) { float2 cursor; float2 viewport; float cursorOn; float flipY; float2 pad; float4 invertRect; };
Texture2D overlay : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}
bool InInvertRect(float2 p) { return all(p >= invertRect.xy) && all(p < invertRect.zw); }
float4 Overlay(float2 uv) {
	if (flipY > 0.5) uv.y = 1 - uv.y;
	return overlay.Sample(samp, uv);   // premultiplied alpha straight from Minecraft
}
// Minecraft's crosshair and attack indicator: drawn with Minecraft's invert blend against Sekiro's
// picture, in a pass of their own; left out of the main one.
float4 PSInvert(VSOut i) : SV_Target {
	if (!InInvertRect(i.pos.xy)) discard;
	return float4(Overlay(i.uv).rgb, 0);
}
float4 PSMain(VSOut i) : SV_Target {
	if (InInvertRect(i.pos.xy)) return 0;
	float4 c = Overlay(i.uv);
	if (cursorOn > 0.5) {
		float2 p = i.pos.xy - cursor;
		if (p.x >= 0 && p.y >= 0 && p.y < 18 && p.x <= p.y * 0.6) {
			bool edge = p.x < 1.5 || p.x > p.y * 0.6 - 1.5 || p.y > 16.5;
			c = float4(edge ? float3(0, 0, 0) : float3(1, 1, 1), 1);
		}
	}
	return c;
}
)";

		template <class T>
		void safeRelease(T*& p)
		{
			if (p) {
				p->Release();
				p = nullptr;
			}
		}

		void releaseResources()
		{
			safeRelease(srv);
			safeRelease(texture);
			safeRelease(vs);
			safeRelease(ps);
			safeRelease(psInvert);
			safeRelease(blend);
			safeRelease(invertBlend);
			safeRelease(sampler);
			safeRelease(raster);
			safeRelease(depth);
			safeRelease(params);
			safeRelease(context);
			safeRelease(device);
			texW = texH = 0;
			haveFrame = false;
			initFailed = false;
		}

		bool compile(const char* entry, const char* target, ID3DBlob** out)
		{
			ID3DBlob* errors = nullptr;
			const HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "sekicraft_overlay", nullptr, nullptr, entry, target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
			if (FAILED(hr)) {
				logf("overlay: shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				safeRelease(errors);
				return false;
			}
			safeRelease(errors);
			return true;
		}

		bool initResources(IDXGISwapChain* swapChain)
		{
			if (device)
				return true;
			if (initFailed || FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
				initFailed = true;
				return false;
			}
			device->GetImmediateContext(&context);

			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *psInvertBlob = nullptr;
			if (!compile("VSMain", "vs_5_0", &vsBlob) || !compile("PSMain", "ps_5_0", &psBlob) || !compile("PSInvert", "ps_5_0", &psInvertBlob)) {
				safeRelease(vsBlob);
				safeRelease(psBlob);
				initFailed = true;
				return false;
			}
			device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
			device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
			device->CreatePixelShader(psInvertBlob->GetBufferPointer(), psInvertBlob->GetBufferSize(), nullptr, &psInvert);
			safeRelease(vsBlob);
			safeRelease(psBlob);
			safeRelease(psInvertBlob);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			device->CreateBlendState(&bd, &blend);
			// Minecraft's BlendFunction.INVERT; the back buffer's alpha is left alone.
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			device->CreateBlendState(&bd, &invertBlend);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			device->CreateSamplerState(&sd, &sampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;
			rd.DepthClipEnable = TRUE;
			device->CreateRasterizerState(&rd, &raster);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = FALSE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			dd.FrontFace = dd.BackFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
			device->CreateDepthStencilState(&dd, &depth);

			D3D11_BUFFER_DESC cbd{};
			cbd.ByteWidth = sizeof(Params);
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			device->CreateBuffer(&cbd, nullptr, &params);

			const bool ok = vs && ps && psInvert && blend && invertBlend && sampler && raster && depth && params;
			logf("overlay: renderer %s", ok ? "ready" : "failed to initialize");
			initFailed = !ok;
			return ok;
		}

		bool ensureTexture(UINT w, UINT h)
		{
			if (texture && texW == w && texH == h)
				return true;
			safeRelease(srv);
			safeRelease(texture);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = w;
			td.Height = h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &texture)) || FAILED(device->CreateShaderResourceView(texture, nullptr, &srv))) {
				logf("overlay: texture %ux%u creation failed", w, h);
				return false;
			}
			texW = w;
			texH = h;
			logf("overlay: texture %ux%u", w, h);
			return true;
		}

		void uploadLatestFrame(GameLink& link)
		{
			if (!link.acquireOverlayFrame())
				return;
			const auto* hdr = link.overlayHeader();
			if (hdr->width == 0 || hdr->height == 0 || hdr->width > proto::kMaxOverlayW || hdr->height > proto::kMaxOverlayH)
				return;
			if (!ensureTexture(hdr->width, hdr->height))
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return;
			const std::uint8_t* src = link.overlayPixels();
			const UINT rowBytes = hdr->width * 4;
			auto* dst = static_cast<std::uint8_t*>(mapped.pData);
			if (mapped.RowPitch == rowBytes) {
				std::memcpy(dst, src, std::size_t(rowBytes) * hdr->height);
			} else {
				for (UINT y = 0; y < hdr->height; ++y)
					std::memcpy(dst + std::size_t(y) * mapped.RowPitch, src + std::size_t(y) * rowBytes, rowBytes);
			}
			context->Unmap(texture, 0);
			flipY = (hdr->flags & 1) != 0;
			haveFrame = true;
		}

		// A typeless back buffer needs an explicit view format.
		DXGI_FORMAT viewFormat(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default: return f;
			}
		}

		void drawOverlay(IDXGISwapChain* swapChain)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (SUCCEEDED(swapChain->GetDesc(&desc))) {
				g_viewW = desc.BufferDesc.Width;
				g_viewH = desc.BufferDesc.Height;
			}
			GameLink* link = g_link.load();
			Settings st;
			{
				std::lock_guard lock(g_settingsMutex);
				st = g_settings;
			}
			if (!link || !initResources(swapChain))
				return;
			uploadLatestFrame(*link);  // keep consuming even while hidden, so the newest frame is ready
			if (!st.show || !haveFrame)
				return;

			ID3D11Texture2D* backBuffer = nullptr;
			if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))))
				return;
			D3D11_TEXTURE2D_DESC bbDesc{};
			backBuffer->GetDesc(&bbDesc);
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = viewFormat(bbDesc.Format);
			rtvDesc.ViewDimension = bbDesc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
			ID3D11RenderTargetView* rtv = nullptr;
			const HRESULT hr = device->CreateRenderTargetView(backBuffer, &rtvDesc, &rtv);
			safeRelease(backBuffer);
			if (FAILED(hr)) {
				static bool logged = false;
				if (!logged) {
					logged = true;
					logf("overlay: back buffer view failed (format %d)", int(bbDesc.Format));
				}
				return;
			}

			// Save the pipeline state we touch.
			ID3D11RenderTargetView*   oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
			ID3D11DepthStencilView*   oldDsv = nullptr;
			ID3D11BlendState*         oldBlend = nullptr;
			float                     oldFactor[4]{};
			UINT                      oldMask = 0;
			ID3D11RasterizerState*    oldRaster = nullptr;
			ID3D11DepthStencilState*  oldDepth = nullptr;
			UINT                      oldStencil = 0;
			D3D11_VIEWPORT            oldVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                      oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			D3D11_PRIMITIVE_TOPOLOGY  oldTopo{};
			ID3D11InputLayout*        oldLayout = nullptr;
			ID3D11VertexShader*       oldVs = nullptr;
			ID3D11PixelShader*        oldPs = nullptr;
			ID3D11ShaderResourceView* oldSrv = nullptr;
			ID3D11SamplerState*       oldSampler = nullptr;
			ID3D11Buffer*             oldCb = nullptr;
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
			context->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
			context->RSGetState(&oldRaster);
			context->OMGetDepthStencilState(&oldDepth, &oldStencil);
			context->RSGetViewports(&oldVpCount, oldVps);
			context->IAGetPrimitiveTopology(&oldTopo);
			context->IAGetInputLayout(&oldLayout);
			context->VSGetShader(&oldVs, nullptr, nullptr);
			context->PSGetShader(&oldPs, nullptr, nullptr);
			context->PSGetShaderResources(0, 1, &oldSrv);
			context->PSGetSamplers(0, 1, &oldSampler);
			context->PSGetConstantBuffers(0, 1, &oldCb);

			bool invert = false;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				auto* p = static_cast<Params*>(mapped.pData);
				// The cursor lives in overlay pixels; scale if the overlay and back buffer differ.
				const float sx = texW ? float(bbDesc.Width) / float(texW) : 1.0f;
				const float sy = texH ? float(bbDesc.Height) / float(texH) : 1.0f;
				p->cursor[0] = st.cursorX * sx;
				p->cursor[1] = st.cursorY * sy;
				p->viewport[0] = float(bbDesc.Width);
				p->viewport[1] = float(bbDesc.Height);
				p->cursorOn = st.cursor ? 1.0f : 0.0f;
				p->flipY = flipY ? 1.0f : 0.0f;
				// Around the screen centre, where Minecraft puts the crosshair (15 GUI pixels) and the
				// attack indicator under it (16 x 16 from 9 GUI pixels below the centre).
				const float g = float(st.guiScale) * sx;
				const float cx = float(bbDesc.Width) * 0.5f, cy = float(bbDesc.Height) * 0.5f;
				invert = st.crosshair && st.guiScale > 0;
				p->invertRect[0] = invert ? cx - 12.0f * g : 0.0f;
				p->invertRect[1] = invert ? cy - 12.0f * g : 0.0f;
				p->invertRect[2] = invert ? cx + 12.0f * g : 0.0f;
				p->invertRect[3] = invert ? cy + 28.0f * g : 0.0f;
				context->Unmap(params, 0);
			}

			D3D11_VIEWPORT vp{ 0, 0, float(bbDesc.Width), float(bbDesc.Height), 0, 1 };
			const float factor[4]{};
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->OMSetBlendState(blend, factor, 0xFFFFFFFF);
			context->OMSetDepthStencilState(depth, 0);
			context->RSSetState(raster);
			context->RSSetViewports(1, &vp);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(vs, nullptr, 0);
			context->PSSetShader(ps, nullptr, 0);
			context->PSSetShaderResources(0, 1, &srv);
			context->PSSetSamplers(0, 1, &sampler);
			context->PSSetConstantBuffers(0, 1, &params);
			context->Draw(3, 0);
			if (invert) {
				context->OMSetBlendState(invertBlend, factor, 0xFFFFFFFF);
				context->PSSetShader(psInvert, nullptr, 0);
				context->Draw(3, 0);
			}

			// Restore.
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
			context->OMSetBlendState(oldBlend, oldFactor, oldMask);
			context->OMSetDepthStencilState(oldDepth, oldStencil);
			context->RSSetState(oldRaster);
			context->RSSetViewports(oldVpCount, oldVps);
			context->IASetPrimitiveTopology(oldTopo);
			context->IASetInputLayout(oldLayout);
			context->VSSetShader(oldVs, nullptr, 0);
			context->PSSetShader(oldPs, nullptr, 0);
			context->PSSetShaderResources(0, 1, &oldSrv);
			context->PSSetSamplers(0, 1, &oldSampler);
			context->PSSetConstantBuffers(0, 1, &oldCb);
			for (auto*& r : oldRtv)
				safeRelease(r);
			safeRelease(oldDsv);
			safeRelease(oldBlend);
			safeRelease(oldRaster);
			safeRelease(oldDepth);
			safeRelease(oldLayout);
			safeRelease(oldVs);
			safeRelease(oldPs);
			safeRelease(oldSrv);
			safeRelease(oldSampler);
			safeRelease(oldCb);
			safeRelease(rtv);
		}

		HRESULT WINAPI hookPresent(IDXGISwapChain* swapChain, UINT sync, UINT flags)
		{
			++g_inFlight;
			if (g_enabled) {
				static bool loggedFirst = false;
				if (!loggedFirst) {
					loggedFirst = true;
					logf("overlay: first Present on thread %lu", GetCurrentThreadId());
				}
				drawOverlay(swapChain);
			} else if (device) {
				releaseResources();  // unloading: free GPU objects on the render thread
			}
			const HRESULT hr = g_origPresent(swapChain, sync, flags);
			--g_inFlight;
			return hr;
		}

		// IDXGISwapChain::Present from a throwaway device + swap chain on a hidden window.
		void* findPresent()
		{
			WNDCLASSEXW wc{ sizeof(wc) };
			wc.lpfnWndProc = DefWindowProcW;
			wc.hInstance = GetModuleHandleW(nullptr);
			wc.lpszClassName = L"SekiCraftDummyWindow";
			RegisterClassExW(&wc);
			HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
			if (!hwnd)
				return nullptr;
			DXGI_SWAP_CHAIN_DESC scd{};
			scd.BufferCount = 1;
			scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			scd.OutputWindow = hwnd;
			scd.SampleDesc.Count = 1;
			scd.Windowed = TRUE;
			IDXGISwapChain*      sc = nullptr;
			ID3D11Device*        dev = nullptr;
			ID3D11DeviceContext* ctx = nullptr;
			void* present = nullptr;
			if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, nullptr, &ctx))) {
				present = (*reinterpret_cast<void***>(sc))[8];
				sc->Release();
				dev->Release();
				ctx->Release();
			}
			DestroyWindow(hwnd);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return present;
		}
	}

	bool install()
	{
		g_targetPresent = findPresent();
		if (!g_targetPresent) {
			logf("overlay: couldn't find IDXGISwapChain::Present");
			return false;
		}
		if (MH_CreateHook(g_targetPresent, reinterpret_cast<void*>(&hookPresent), reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
			MH_EnableHook(g_targetPresent) != MH_OK) {
			logf("overlay: couldn't hook Present");
			g_targetPresent = nullptr;
			return false;
		}
		g_enabled = true;
		logf("overlay: Present hooked (%p)", g_targetPresent);
		return true;
	}

	void uninstall()
	{
		if (!g_targetPresent)
			return;
		// Let one more frame through with drawing off, so GPU objects are released on the render
		// thread, then unhook and wait for any frame still inside the hook.
		g_enabled = false;
		g_link = nullptr;
		for (int i = 0; i < 50 && device; ++i)
			Sleep(10);
		MH_DisableHook(g_targetPresent);
		MH_RemoveHook(g_targetPresent);
		for (int i = 0; i < 200 && g_inFlight > 0; ++i)
			Sleep(5);
		g_targetPresent = nullptr;
	}

	void setLink(GameLink* link)
	{
		g_link = link;
	}

	void update(const Settings& settings)
	{
		std::lock_guard lock(g_settingsMutex);
		g_settings = settings;
	}

	void viewport(std::uint32_t& width, std::uint32_t& height)
	{
		width = g_viewW;
		height = g_viewH;
	}
}
