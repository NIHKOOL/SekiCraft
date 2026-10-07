#include "world.h"

#include "log.h"
#include "sekicraft_protocol.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sekicraft::world
{
	namespace
	{
		namespace proto = sekicraft::proto;

		constexpr int kUploadsPerFrame = 48;  // section vertex buffers created per frame (hitch guard)

		long long sectionKey(int sx, int sy, int sz)
		{
			return (long long(sx & 0x3FFFFF) << 42) | (long long(sy & 0xFFFFF) << 22) | long long(sz & 0x3FFFFF);
		}

		struct SectionMesh
		{
			int                          sx = 0, sy = 0, sz = 0;
			std::vector<proto::RenVertex> verts;  // empty: remove the section
		};

		struct Region
		{
			std::uint32_t             x, y, w, h;
			std::vector<std::uint8_t> pixels;
		};

		// ---- worker -> render thread (g_mutex) ----
		std::mutex                                                   g_mutex;
		bool                                                         g_clear = false;
		std::shared_ptr<std::vector<std::uint8_t>>                   g_atlas;  // full atlas waiting for upload
		std::uint32_t                                                g_atlasW = 0, g_atlasH = 0;
		std::vector<Region>                                          g_regions;
		std::unordered_map<long long, std::shared_ptr<SectionMesh>>  g_sections;
		std::atomic<long long>                                       g_bytesIn{ 0 }, g_sectionsIn{ 0 };

		// ---- render thread ----
		struct GpuSection
		{
			ID3D11Buffer* vb = nullptr;
			UINT          count = 0;
			int           sx = 0, sy = 0, sz = 0;
		};
		std::unordered_map<long long, GpuSection>                    r_sections;
		std::unordered_map<long long, std::shared_ptr<SectionMesh>>  r_waiting;  // received, not yet uploaded
		ID3D11Device*             r_device = nullptr;  // the device our objects belong to (not owned)
		ID3D11Texture2D*          r_atlas = nullptr;
		ID3D11ShaderResourceView* r_atlasSrv = nullptr;
		UINT                      r_atlasW = 0, r_atlasH = 0;
		ID3D11VertexShader*       r_vs = nullptr;
		ID3D11PixelShader*        r_ps = nullptr;
		ID3D11VertexShader*       r_vsCopy = nullptr;
		ID3D11PixelShader*        r_psCopy = nullptr;
		ID3D11DepthStencilState*  r_depthCopy = nullptr;
		ID3D11BlendState*         r_noColor = nullptr;
		ID3D11InputLayout*        r_layout = nullptr;
		ID3D11Buffer*             r_frameCb = nullptr;
		ID3D11Buffer*             r_sectionCb = nullptr;
		ID3D11SamplerState*       r_sampler = nullptr;
		ID3D11RasterizerState*    r_raster = nullptr;
		ID3D11DepthStencilState*  r_depthWrite = nullptr;
		ID3D11DepthStencilState*  r_depthRead = nullptr;
		ID3D11BlendState*         r_opaque = nullptr;
		ID3D11BlendState*         r_alpha = nullptr;
		ID3D11Texture2D*          r_depth = nullptr;
		ID3D11DepthStencilView*   r_dsv = nullptr;
		UINT                      r_depthW = 0, r_depthH = 0;
		bool                      r_failed = false;
		std::atomic<long long>    g_drawnSections{ 0 }, g_drawnVerts{ 0 };

		struct alignas(16) FrameCb
		{
			float viewProj[16];  // row-major, row vectors (v * M)
			float pass;          // 0: opaque + cutout, 1: translucent
			float pad[3];
		};

		struct alignas(16) SectionCb
		{
			float origin[4];  // section origin, Minecraft coords
		};

		constexpr char kShader[] = R"(
cbuffer Frame : register(b0) { row_major float4x4 viewProj; float renderPass; float3 pad0; };
cbuffer Section : register(b1) { float4 origin; };
Texture2D atlas : register(t0);
SamplerState samp : register(s0);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; uint light : LIGHT; uint flags : FLAGS; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; nointerpolation uint flags : FLAGS; };
VSOut VSMain(VSIn i) {
	VSOut o;
	float3 mc = origin.xyz + i.pos;
	o.pos = mul(float4(mc.x, mc.y, -mc.z, 1), viewProj);   // Minecraft -> Sekiro: z flips
	// Minecraft's light: the brighter of block light and sky light, on its brightness curve, times
	// its fixed per-face shading (the exporter leaves that out of the colour).
	float b = max(float(i.light & 0xFF), float((i.light >> 8) & 0xFF)) / 15.0;
	float lum = lerp(0.12, 1.0, b / (4.0 - 3.0 * b));
	uint face = (i.flags >> 4) & 7;   // Direction ordinal + 1: 1 down, 2 up, 3 north, 4 south, 5 west, 6 east
	float shade = face == 1 ? 0.5 : face == 2 ? 1.0 : (face == 3 || face == 4) ? 0.8 : face >= 5 ? 0.6 : 1.0;
	o.color = float4(i.color.rgb * lum * shade, i.color.a);
	o.uv = i.uv;
	o.flags = i.flags;
	return o;
}
// Sekiro's scene depth copied into ours: both use the same reversed, infinite-far projection.
Texture2D<float> sceneDepth : register(t1);
float4 VSCopy(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float PSCopy(float4 pos : SV_Position) : SV_Depth { return sceneDepth.Load(int3(pos.xy, 0)); }
float4 PSMain(VSOut i) : SV_Target {
	bool translucent = (i.flags & 2) != 0;
	if (translucent != (renderPass > 0.5)) discard;
	float4 c = atlas.Sample(samp, i.uv) * i.color;
	if (!translucent) {
		if ((i.flags & 1) != 0 && c.a < 0.1) discard;   // cutout: leaves, glass panes, flowers
		c.a = 1;
	}
	return float4(c.rgb * c.a, c.a);   // premultiplied for the translucent pass
}
)";

		template <class T>
		void release(T*& p)
		{
			if (p) {
				p->Release();
				p = nullptr;
			}
		}

		bool initPipeline(ID3D11Device* device)
		{
			if (r_device == device && r_vs)
				return true;
			if (r_failed)
				return false;
			releaseGpu();
			r_device = device;
			ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errors = nullptr;
			if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "sekicraft_world", nullptr, nullptr, "VSMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errors)) ||
				FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "sekicraft_world", nullptr, nullptr, "PSMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errors))) {
				logf("world: shader failed: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				release(errors);
				release(vsBlob);
				r_failed = true;
				return false;
			}
			device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &r_vs);
			device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &r_ps);
			{
				ID3DBlob *vsCopy = nullptr, *psCopy = nullptr;
				if (SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "sekicraft_world", nullptr, nullptr, "VSCopy", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsCopy, nullptr)) &&
					SUCCEEDED(D3DCompile(kShader, sizeof(kShader) - 1, "sekicraft_world", nullptr, nullptr, "PSCopy", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psCopy, nullptr))) {
					device->CreateVertexShader(vsCopy->GetBufferPointer(), vsCopy->GetBufferSize(), nullptr, &r_vsCopy);
					device->CreatePixelShader(psCopy->GetBufferPointer(), psCopy->GetBufferSize(), nullptr, &r_psCopy);
				}
				release(vsCopy);
				release(psCopy);
			}
			const D3D11_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "LIGHT", 0, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "FLAGS", 0, DXGI_FORMAT_R32_UINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			device->CreateInputLayout(layout, 5, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &r_layout);
			release(vsBlob);
			release(psBlob);

			D3D11_BUFFER_DESC cbd{};
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			cbd.ByteWidth = sizeof(FrameCb);
			device->CreateBuffer(&cbd, nullptr, &r_frameCb);
			cbd.ByteWidth = sizeof(SectionCb);
			device->CreateBuffer(&cbd, nullptr, &r_sectionCb);

			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			device->CreateSamplerState(&sd, &r_sampler);

			D3D11_RASTERIZER_DESC rd{};
			rd.FillMode = D3D11_FILL_SOLID;
			rd.CullMode = D3D11_CULL_NONE;  // the z flip mirrors Minecraft's winding; don't rely on it
			rd.DepthClipEnable = TRUE;
			device->CreateRasterizerState(&rd, &r_raster);

			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;  // reversed depth: bigger is nearer
			dd.FrontFace = dd.BackFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
			device->CreateDepthStencilState(&dd, &r_depthWrite);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			device->CreateDepthStencilState(&dd, &r_depthRead);
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
			device->CreateDepthStencilState(&dd, &r_depthCopy);

			D3D11_BLEND_DESC bd{};
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
			device->CreateBlendState(&bd, &r_opaque);
			{
				D3D11_BLEND_DESC nb{};
				nb.RenderTarget[0].RenderTargetWriteMask = 0;
				device->CreateBlendState(&nb, &r_noColor);
			}
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			device->CreateBlendState(&bd, &r_alpha);

			const bool ok = r_vs && r_ps && r_layout && r_frameCb && r_sectionCb && r_sampler && r_raster && r_depthWrite && r_depthRead && r_opaque && r_alpha;
			logf("world: block renderer %s", ok ? "ready" : "failed to initialize");
			r_failed = !ok;
			return ok;
		}

		bool ensureDepth(ID3D11Device* device, UINT w, UINT h)
		{
			if (r_dsv && r_depthW == w && r_depthH == h)
				return true;
			release(r_dsv);
			release(r_depth);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = w;
			td.Height = h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_D32_FLOAT;
			td.SampleDesc.Count = 1;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(device->CreateTexture2D(&td, nullptr, &r_depth)) || FAILED(device->CreateDepthStencilView(r_depth, nullptr, &r_dsv)))
				return false;
			r_depthW = w;
			r_depthH = h;
			return true;
		}

		void uploadAtlas(ID3D11Device* device, ID3D11DeviceContext* ctx, const std::vector<std::uint8_t>& px, UINT w, UINT h)
		{
			release(r_atlasSrv);
			release(r_atlas);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = w;
			td.Height = h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
			if (FAILED(device->CreateTexture2D(&td, &init, &r_atlas)) || FAILED(device->CreateShaderResourceView(r_atlas, nullptr, &r_atlasSrv))) {
				logf("world: atlas %ux%u upload failed", w, h);
				return;
			}
			(void)ctx;
			r_atlasW = w;
			r_atlasH = h;
			logf("world: atlas %ux%u uploaded", w, h);
		}

		void freeSection(GpuSection& s)
		{
			release(s.vb);
			s.count = 0;
		}

		// Row-vector matrices (v * M), as HLSL's row_major mul(v, M) expects.
		void mul(const float* a, const float* b, float* out)
		{
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
		}

		void viewProjection(const game::CameraState& cam, float aspect, float* out)
		{
			const float* R = cam.world + 0;
			const float* U = cam.world + 4;
			const float* F = cam.world + 8;
			const float* P = cam.world + 12;
			auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			const float view[16] = {
				R[0], U[0], F[0], 0,
				R[1], U[1], F[1], 0,
				R[2], U[2], F[2], 0,
				-dot(P, R), -dot(P, U), -dot(P, F), 1,
			};
			// Sekiro's projection, measured from its depth buffer: left-handed, reversed and with an
			// infinite far plane, so stored depth = near / view distance (sky = 0). Matching it lets
			// its depth buffer occlude our blocks directly.
			const float zn = cam.nearZ > 0.001f && cam.nearZ < 10.0f ? cam.nearZ : 0.08f;
			const float ys = 1.0f / std::tan(cam.fov * 0.5f), xs = ys / aspect;
			const float proj[16] = {
				xs, 0, 0, 0,
				0, ys, 0, 0,
				0, 0, 0, 1,
				0, 0, zn, 0,
			};
			mul(view, proj, out);
		}
	}

	void consume(std::uint32_t type, const std::uint8_t* p, std::uint32_t bytes)
	{
		g_bytesIn += bytes;
		switch (type) {
		case proto::kRenClearAll: {
			std::lock_guard lock(g_mutex);
			g_clear = true;
			g_sections.clear();
			g_regions.clear();
			break;
		}
		case proto::kRenAtlas: {
			if (bytes < sizeof(proto::RenAtlas))
				return;
			proto::RenAtlas a;
			std::memcpy(&a, p, sizeof(a));
			const std::size_t n = std::size_t(a.width) * a.height * 4;
			if (bytes < sizeof(a) + n)
				return;
			auto px = std::make_shared<std::vector<std::uint8_t>>(p + sizeof(a), p + sizeof(a) + n);
			std::lock_guard lock(g_mutex);
			g_atlas = std::move(px);
			g_atlasW = a.width;
			g_atlasH = a.height;
			g_regions.clear();  // older animation frames are inside the new atlas already
			break;
		}
		case proto::kRenAtlasRegion: {
			if (bytes < sizeof(proto::RenAtlasRegion))
				return;
			proto::RenAtlasRegion r;
			std::memcpy(&r, p, sizeof(r));
			const std::size_t n = std::size_t(r.width) * r.height * 4;
			if (bytes < sizeof(r) + n)
				return;
			std::lock_guard lock(g_mutex);
			g_regions.push_back({ r.x, r.y, r.width, r.height, std::vector<std::uint8_t>(p + sizeof(r), p + sizeof(r) + n) });
			break;
		}
		case proto::kRenSection: {
			if (bytes < sizeof(proto::RenSection))
				return;
			proto::RenSection s;
			std::memcpy(&s, p, sizeof(s));
			if (bytes < sizeof(s) + std::size_t(s.vertexCount) * sizeof(proto::RenVertex))
				return;
			auto mesh = std::make_shared<SectionMesh>();
			mesh->sx = s.sx;
			mesh->sy = s.sy;
			mesh->sz = s.sz;
			mesh->verts.resize(s.vertexCount);
			std::memcpy(mesh->verts.data(), p + sizeof(s), std::size_t(s.vertexCount) * sizeof(proto::RenVertex));
			++g_sectionsIn;
			std::lock_guard lock(g_mutex);
			g_sections[sectionKey(s.sx, s.sy, s.sz)] = std::move(mesh);
			break;
		}
		default:
			break;  // lights, entities, avatar, dug blocks: later phases
		}
	}

	void draw(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, unsigned width, unsigned height, const game::CameraState& cam,
		ID3D11ShaderResourceView* sceneDepth)
	{
		if (!initPipeline(device))
			return;

		// Take what the worker received.
		bool clear = false;
		std::shared_ptr<std::vector<std::uint8_t>> atlas;
		UINT atlasW = 0, atlasH = 0;
		std::vector<Region> regions;
		std::unordered_map<long long, std::shared_ptr<SectionMesh>> sections;
		{
			std::lock_guard lock(g_mutex);
			clear = g_clear;
			g_clear = false;
			atlas = std::move(g_atlas);
			g_atlas.reset();
			atlasW = g_atlasW;
			atlasH = g_atlasH;
			regions.swap(g_regions);
			sections.swap(g_sections);
		}
		if (clear) {
			for (auto& [k, s] : r_sections)
				freeSection(s);
			r_sections.clear();
			r_waiting.clear();
		}
		if (atlas)
			uploadAtlas(device, ctx, *atlas, atlasW, atlasH);
		if (r_atlas) {
			for (const Region& r : regions) {
				if (r.x + r.w > r_atlasW || r.y + r.h > r_atlasH)
					continue;
				const D3D11_BOX box{ r.x, r.y, 0, r.x + r.w, r.y + r.h, 1 };
				ctx->UpdateSubresource(r_atlas, 0, &box, r.pixels.data(), r.w * 4, 0);
			}
		}
		for (auto& [k, m] : sections)
			r_waiting[k] = std::move(m);

		// Upload a limited number of sections per frame.
		int uploads = 0;
		for (auto it = r_waiting.begin(); it != r_waiting.end() && uploads < kUploadsPerFrame;) {
			const SectionMesh& m = *it->second;
			GpuSection& g = r_sections[it->first];
			freeSection(g);
			g.sx = m.sx;
			g.sy = m.sy;
			g.sz = m.sz;
			if (!m.verts.empty()) {
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = UINT(m.verts.size() * sizeof(proto::RenVertex));
				bd.Usage = D3D11_USAGE_IMMUTABLE;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				D3D11_SUBRESOURCE_DATA init{ m.verts.data(), 0, 0 };
				if (SUCCEEDED(device->CreateBuffer(&bd, &init, &g.vb)))
					g.count = UINT(m.verts.size());
				++uploads;
			}
			if (!g.vb)
				r_sections.erase(it->first);
			it = r_waiting.erase(it);
		}

		if (!r_atlasSrv || r_sections.empty() || !ensureDepth(device, width, height))
			return;

		FrameCb frame{};
		viewProjection(cam, float(width) / float(height), frame.viewProj);

		const D3D11_VIEWPORT vp{ 0, 0, float(width), float(height), 0, 1 };
		const float factor[4]{};
		ctx->OMSetRenderTargets(1, &rtv, r_dsv);
		ctx->RSSetViewports(1, &vp);
		ctx->RSSetState(r_raster);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		// Start from Sekiro's depth (its walls hide our blocks), or from "nothing" (0) without it.
		if (sceneDepth && r_vsCopy && r_psCopy) {
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(r_vsCopy, nullptr, 0);
			ctx->PSSetShader(r_psCopy, nullptr, 0);
			ctx->PSSetShaderResources(1, 1, &sceneDepth);
			ctx->OMSetBlendState(r_noColor, factor, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(r_depthCopy, 0);
			ctx->Draw(3, 0);
			ID3D11ShaderResourceView* none = nullptr;
			ctx->PSSetShaderResources(1, 1, &none);
		} else {
			ctx->ClearDepthStencilView(r_dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
		}
		ctx->IASetInputLayout(r_layout);
		ctx->VSSetShader(r_vs, nullptr, 0);
		ctx->PSSetShader(r_ps, nullptr, 0);
		ctx->PSSetShaderResources(0, 1, &r_atlasSrv);
		ctx->PSSetSamplers(0, 1, &r_sampler);
		ID3D11Buffer* cbs[2] = { r_frameCb, r_sectionCb };
		ctx->VSSetConstantBuffers(0, 2, cbs);
		ctx->PSSetConstantBuffers(0, 1, &r_frameCb);

		long long drawnSections = 0, drawnVerts = 0;
		for (int pass = 0; pass < 2; ++pass) {
			frame.pass = float(pass);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(ctx->Map(r_frameCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return;
			std::memcpy(mapped.pData, &frame, sizeof(frame));
			ctx->Unmap(r_frameCb, 0);
			ctx->OMSetBlendState(pass == 0 ? r_opaque : r_alpha, factor, 0xFFFFFFFF);
			ctx->OMSetDepthStencilState(pass == 0 ? r_depthWrite : r_depthRead, 0);
			for (const auto& [k, s] : r_sections) {
				if (!s.vb)
					continue;
				SectionCb sc{ { float(s.sx * 16), float(s.sy * 16), float(s.sz * 16), 0 } };
				if (FAILED(ctx->Map(r_sectionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
					continue;
				std::memcpy(mapped.pData, &sc, sizeof(sc));
				ctx->Unmap(r_sectionCb, 0);
				const UINT stride = sizeof(proto::RenVertex), offset = 0;
				ctx->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
				ctx->Draw(s.count, 0);
				if (pass == 0) {
					++drawnSections;
					drawnVerts += s.count;
				}
			}
		}
		g_drawnSections = drawnSections;
		g_drawnVerts = drawnVerts;
	}

	void releaseGpu()
	{
		for (auto& [k, s] : r_sections)
			freeSection(s);
		r_sections.clear();
		r_waiting.clear();
		release(r_atlasSrv);
		release(r_atlas);
		release(r_vs);
		release(r_ps);
		release(r_vsCopy);
		release(r_psCopy);
		release(r_depthCopy);
		release(r_noColor);
		release(r_layout);
		release(r_frameCb);
		release(r_sectionCb);
		release(r_sampler);
		release(r_raster);
		release(r_depthWrite);
		release(r_depthRead);
		release(r_opaque);
		release(r_alpha);
		release(r_dsv);
		release(r_depth);
		r_atlasW = r_atlasH = r_depthW = r_depthH = 0;
		r_device = nullptr;
		r_failed = false;
	}

	std::string stats()
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf), "world: %.1f MB received, %lld sections received, drawing %lld sections (%lld vertices)",
			double(g_bytesIn.load()) / (1 << 20), g_sectionsIn.load(), g_drawnSections.load(), g_drawnVerts.load());
		return buf;
	}
}
