#include "world.h"

#include "log.h"
#include "sekicraft_protocol.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <initializer_list>
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

		// Entities: Minecraft's own meshes for mobs, players and particles (scene, avatar) with their
		// textures, plus the simple things the game side builds itself (world entities).
		struct Batches
		{
			float                         origin[3] = {};  // Minecraft coords the vertices are relative to
			std::vector<proto::RenBatch>  batches;
			std::vector<proto::RenVertex> verts;
		};
		struct PendingTexture
		{
			std::uint32_t             id, w, h;
			std::vector<std::uint8_t> pixels;
		};
		std::vector<PendingTexture> g_textures;
		std::shared_ptr<Batches>    g_scene, g_avatar;   // latest frame of each
		proto::WorldEntities        g_entities{};


		// ---- render thread ----
		struct GpuSection
		{
			ID3D11Buffer* vb = nullptr;
			UINT          count = 0;
			int           sx = 0, sy = 0, sz = 0;
		};
		std::unordered_map<long long, GpuSection>                    r_sections;
		std::unordered_map<long long, std::shared_ptr<SectionMesh>>  r_waiting;  // received, not yet uploaded
		struct GpuTexture
		{
			ID3D11Texture2D*          tex = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
		};
		std::unordered_map<std::uint32_t, GpuTexture> r_textures;  // entity textures by id (0 = atlas)
		std::shared_ptr<Batches>  r_scene, r_avatar;
		ID3D11Buffer*             r_dynVb = nullptr;  // per-frame vertices: scene, avatar, built entities, outline
		UINT                      r_dynCapacity = 0;
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
		ID3D11SamplerState*       r_samplerLinear = nullptr;
		ID3D11Texture2D*          r_sceneColor = nullptr;   // Sekiro's picture, copied before our blocks, with mips
		ID3D11ShaderResourceView* r_sceneColorSrv = nullptr;
		UINT                      r_sceneW = 0, r_sceneH = 0, r_sceneMips = 0;
		DXGI_FORMAT               r_sceneFormat = DXGI_FORMAT_UNKNOWN;
		std::atomic<int>          g_sceneLight{ 0 };  // off by default: looked wrong in testing (F6 to try)
		std::atomic<float>        g_midGrey{ 0.18f };
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
			float localMip;      // scene colour mip for the light around a pixel
			float globalMip;     // the 1x1 mip: the whole picture's average
			float sceneLight;    // 1: light blocks from Sekiro's picture, 0: Minecraft's light only
			float viewport[2];
			float midGrey;       // scene luminance that counts as "normally lit" (linear)
			float pad;
		};

		struct alignas(16) SectionCb
		{
			float origin[4];        // what the vertices are relative to, Minecraft coords
			float forceTranslucent; // a translucent entity batch (its vertices may not say so)
			float pad[3];
		};

		// Vertex flags, as SkyCraft defines them: 1 cutout, 2 translucent, 4 untextured (ours: Minecraft
		// never sets it), 8 full-detail texture (Minecraft's entities), bits 4-6 face (7: entity faces).
		constexpr std::uint32_t kFlagCutout = 1;
		constexpr std::uint32_t kFlagTranslucent = 2;
		constexpr std::uint32_t kFlagUntextured = 4;
		constexpr std::uint32_t kFullSkyLight = 15u << 8;
		constexpr float         kPi = 3.14159265358979f;

		constexpr char kShader[] = R"(
cbuffer Frame : register(b0) {
	row_major float4x4 viewProj;
	float renderPass; float localMip; float globalMip; float sceneLight;
	float2 viewport; float midGrey; float pad0;
};
cbuffer Section : register(b1) { float4 origin; float forceTranslucent; float3 pad1; };
Texture2D atlas : register(t0);
Texture2D sceneColor : register(t2);   // Sekiro's picture before our blocks, with mips
SamplerState samp : register(s0);
SamplerState sampLinear : register(s1);
struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; uint light : LIGHT; uint flags : FLAGS; };
struct VSOut {
	float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0;
	float2 light : TEXCOORD1;   // Minecraft block light, sky light (0..1)
	nointerpolation uint flags : FLAGS;
};
VSOut VSMain(VSIn i) {
	VSOut o;
	float3 mc = origin.xyz + i.pos;
	o.pos = mul(float4(mc.x, mc.y, -mc.z, 1), viewProj);   // Minecraft -> Sekiro: z flips
	// Minecraft's fixed per-face shading (the exporter leaves it out of the colour).
	uint face = (i.flags >> 4) & 7;   // Direction ordinal + 1: 1 down, 2 up, 3 north, 4 south, 5 west, 6 east
	float shade = face == 1 ? 0.5 : face == 2 ? 1.0 : (face == 3 || face == 4) ? 0.8 : (face == 5 || face == 6) ? 0.6 : 1.0;   // 7: entity, unshaded
	o.color = float4(i.color.rgb * shade, i.color.a);
	o.light = float2(float(i.light & 0xFF), float((i.light >> 8) & 0xFF)) / 15.0;
	o.uv = i.uv;
	o.flags = i.flags;
	return o;
}
// Sekiro's scene depth copied into ours: both use the same reversed, infinite-far projection.
Texture2D<float> sceneDepth : register(t1);
float4 VSCopy(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float PSCopy(float4 pos : SV_Position) : SV_Depth { return sceneDepth.Load(int3(pos.xy, 0)); }

float  mcCurve(float b) { return b / (4.0 - 3.0 * b); }   // Minecraft's light brightness curve
float3 toLinear(float3 c) { return pow(max(c, 0), 2.2); }
float  luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// How brightly a block here is lit (linear). From Sekiro's picture: the blurred scene around this
// screen spot says how lit the surroundings are (sun, shade, night, interiors); the whole picture's
// average colour tints it. Minecraft's sky light still darkens what's under Minecraft roofs, and its
// block light (torches, lava, glowstone) adds warm light of its own.
float3 illumination(float2 screenPos, float2 mcLight) {
	float blockLight = mcCurve(mcLight.x);
	float skyLight = lerp(0.12, 1.0, mcCurve(mcLight.y));
	if (sceneLight < 0.5)
		return max(blockLight, skyLight);
	float2 uv = screenPos / viewport;
	float3 around = toLinear(sceneColor.SampleLevel(sampLinear, uv, localMip).rgb);
	float3 whole = toLinear(sceneColor.SampleLevel(sampLinear, float2(0.5, 0.5), globalMip).rgb);
	float  e = clamp(luma(around) / midGrey, 0.04, 1.6);
	float3 tint = lerp(float3(1, 1, 1), whole / max(luma(whole), 1e-4), 0.5);
	float3 sky = e * tint * skyLight;
	return max(sky, blockLight * float3(1.0, 0.85, 0.6));
}

float4 PSMain(VSOut i) : SV_Target {
	bool translucent = (i.flags & 2) != 0 || forceTranslucent > 0.5;
	if (translucent != (renderPass > 0.5)) discard;
	float4 c = ((i.flags & 4) != 0 ? float4(1, 1, 1, 1) : atlas.Sample(samp, i.uv)) * i.color;   // 4: untextured (outlines)
	if (!translucent) {
		if ((i.flags & 1) != 0 && c.a < 0.1) discard;   // cutout: leaves, glass panes, flowers
		c.a = 1;
	}
	float3 lit = pow(max(toLinear(c.rgb) * illumination(i.pos.xy, i.light), 0), 1.0 / 2.2);
	return float4(lit * c.a, c.a);   // premultiplied for the translucent pass
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
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			device->CreateSamplerState(&sd, &r_samplerLinear);

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

		DXGI_FORMAT typedFormat(DXGI_FORMAT f)
		{
			switch (f) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default: return f;
			}
		}

		// Copies Sekiro's finished picture (before our blocks) into a mip-mapped texture, so the block
		// shader can read how bright the scene is around each pixel.
		bool captureScene(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv)
		{
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			ID3D11Texture2D* bb = nullptr;
			if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
				release(res);
				return false;
			}
			release(res);
			D3D11_TEXTURE2D_DESC bd{};
			bb->GetDesc(&bd);
			if (bd.SampleDesc.Count > 1) {  // a multisampled back buffer can't be copied this way
				release(bb);
				return false;
			}
			if (r_sceneW != bd.Width || r_sceneH != bd.Height || r_sceneFormat != bd.Format) {
				release(r_sceneColorSrv);
				release(r_sceneColor);
				r_sceneW = bd.Width;
				r_sceneH = bd.Height;
				r_sceneFormat = bd.Format;  // tried once per size/format, even if it fails
				D3D11_TEXTURE2D_DESC td{};
				td.Width = bd.Width;
				td.Height = bd.Height;
				td.MipLevels = 0;  // full chain
				td.ArraySize = 1;
				td.Format = bd.Format;
				td.SampleDesc.Count = 1;
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				sd.Format = typedFormat(bd.Format);
				sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				sd.Texture2D.MipLevels = UINT(-1);
				if (FAILED(device->CreateTexture2D(&td, nullptr, &r_sceneColor)) || FAILED(device->CreateShaderResourceView(r_sceneColor, &sd, &r_sceneColorSrv))) {
					logf("world: scene colour copy unavailable (format %d); blocks use Minecraft's light only", int(bd.Format));
					release(r_sceneColor);
					release(bb);
					return false;
				}
				D3D11_TEXTURE2D_DESC made{};
				r_sceneColor->GetDesc(&made);
				r_sceneMips = made.MipLevels;
				logf("world: lighting blocks from Sekiro's picture (%ux%u, format %d, %u mips)", bd.Width, bd.Height, int(bd.Format), r_sceneMips);
			}
			if (!r_sceneColorSrv) {
				release(bb);
				return false;
			}
			ctx->CopySubresourceRegion(r_sceneColor, 0, 0, 0, 0, bb, 0, nullptr);
			ctx->GenerateMips(r_sceneColorSrv);
			release(bb);
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

	namespace
	{
		// ---- entity geometry (adapted from SkyCraft's WorldRender.cpp, MIT, chasmlol) ----------

		void quad(std::vector<proto::RenVertex>& out, const float p[4][3], const float uv4[4], std::uint32_t color, std::uint32_t flags)
		{
			const float uv[4][2] = { { uv4[0], uv4[1] }, { uv4[2], uv4[1] }, { uv4[2], uv4[3] }, { uv4[0], uv4[3] } };
			for (int k : { 0, 1, 2, 0, 2, 3 })
				out.push_back({ p[k][0], p[k][1], p[k][2], uv[k][0], uv[k][1], color, kFullSkyLight, flags });
		}

		// A brightness times a tint (RGBA8, red in the low byte), as a vertex colour.
		std::uint32_t shadeColor(float shade, std::uint32_t tint = 0)
		{
			float r = shade, g = shade, b = shade;
			if (tint) {
				r *= float(tint & 0xFF) / 255.0f;
				g *= float((tint >> 8) & 0xFF) / 255.0f;
				b *= float((tint >> 16) & 0xFF) / 255.0f;
			}
			return 0xFF000000u | (std::uint32_t(b * 255.0f) << 16) | (std::uint32_t(g * 255.0f) << 8) | std::uint32_t(r * 255.0f);
		}

		// A box rotated by yaw about its vertical centre line, one texture per face group.
		void box(std::vector<proto::RenVertex>& out, const float mn[3], const float size[3], float yaw, const float side[4], const float top[4],
			const float bottom[4], std::uint32_t topTint, std::uint32_t flags)
		{
			const float cx = mn[0] + size[0] * 0.5f, cz = mn[2] + size[2] * 0.5f;
			const float c = std::cos(yaw), sn = std::sin(yaw);
			auto corner = [&](int i, float o[3]) {
				const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
				o[0] = cx + lx * c - lz * sn;
				o[1] = mn[1] + ((i & 2) ? size[1] : 0.0f);
				o[2] = cz + lx * sn + lz * c;
			};
			// corner bits: 1 = +x, 2 = +y, 4 = +z; each face TL, TR, BR, BL seen from outside
			static constexpr int kFaces[6][4] = {
				{ 6, 7, 5, 4 },  // south (+z)
				{ 3, 2, 0, 1 },  // north (-z)
				{ 7, 3, 1, 5 },  // east (+x)
				{ 2, 6, 4, 0 },  // west (-x)
				{ 2, 3, 7, 6 },  // top
				{ 4, 5, 1, 0 },  // bottom
			};
			// Minecraft Direction ordinal + 1, for the shader's face shading.
			static constexpr std::uint32_t kFaceIds[6] = { 4, 3, 6, 5, 2, 1 };
			for (int f = 0; f < 6; ++f) {
				float pts[4][3];
				for (int k = 0; k < 4; ++k)
					corner(kFaces[f][k], pts[k]);
				const float* uv = f == 4 ? top : f == 5 ? bottom : side;
				quad(out, pts, uv, shadeColor(1.0f, f == 4 ? topTint : 0), flags | (kFaceIds[f] << 4));
			}
		}

		// Minecraft's arrow (or a trident) at p, flying along unit direction d (Minecraft axes).
		void arrow(std::vector<proto::RenVertex>& out, const float p[3], const float d[3], const float* uvSide, const float* uvBack, bool trident)
		{
			float sd[3] = { d[2], 0.0f, -d[0] };
			float sl = std::sqrt(sd[0] * sd[0] + sd[2] * sd[2]);
			if (sl < 1e-3f) {
				sd[0] = 1.0f;
				sd[2] = 0.0f;
				sl = 1.0f;
			}
			sd[0] /= sl;
			sd[2] /= sl;
			const float u[3] = { sd[1] * d[2] - sd[2] * d[1], sd[2] * d[0] - sd[0] * d[2], sd[0] * d[1] - sd[1] * d[0] };
			constexpr float r = 0.70710678f;
			const float fins[2][3] = { { (u[0] + sd[0]) * r, (u[1] + sd[1]) * r, (u[2] + sd[2]) * r }, { (u[0] - sd[0]) * r, (u[1] - sd[1]) * r, (u[2] - sd[2]) * r } };
			auto at = [&](float along, const float* q, float side, const float* q2, float side2, float o[3]) {
				for (int k = 0; k < 3; ++k)
					o[k] = p[k] + d[k] * along + q[k] * side + (q2 ? q2[k] * side2 : 0.0f);
			};
			if (!trident) {
				// Minecraft's ArrowModel: two fins 16 long and 4 wide (1/16 block, scaled 0.9) and a
				// 4x4 back plate.
				constexpr float k = 0.9f / 16.0f;
				for (const auto& q : fins) {
					float pts[4][3];
					at(-12 * k, q, -2 * k, nullptr, 0, pts[0]);
					at(4 * k, q, -2 * k, nullptr, 0, pts[1]);
					at(4 * k, q, 2 * k, nullptr, 0, pts[2]);
					at(-12 * k, q, 2 * k, nullptr, 0, pts[3]);
					quad(out, pts, uvSide, 0xFFFFFFFFu, kFlagCutout);
				}
				float pts[4][3];
				at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, pts[0]);
				at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, pts[1]);
				at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, pts[2]);
				at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, pts[3]);
				quad(out, pts, uvBack, 0xFFFFFFFFu, kFlagCutout);
			} else {
				// The trident's item icon, whose diagonal runs handle (bottom-left) to tip (top-right).
				constexpr float h = 0.9f;
				for (const auto& q : fins) {
					float pts[4][3];
					at(0, q, h, nullptr, 0, pts[0]);
					at(h, q, 0, nullptr, 0, pts[1]);
					at(0, q, -h, nullptr, 0, pts[2]);
					at(-h, q, 0, nullptr, 0, pts[3]);
					quad(out, pts, uvSide, 0xFFFFFFFFu, kFlagCutout);
				}
			}
		}

		void buildEntities(const proto::WorldEntities& w, std::vector<proto::RenVertex>& tris, std::vector<proto::RenVertex>& lines)
		{
			for (std::uint32_t i = 0; i < w.count && i < proto::kMaxWorldEntities; ++i) {
				const auto& e = w.entities[i];
				const float pos[3] = { e.x, e.y, e.z };
				switch (e.kind) {
				case proto::kWeBlock: {  // a dropped block: a small cube spinning about its centre
					const float sz = e.scale;
					const float mn[3] = { e.x - sz * 0.5f, e.y - sz * 0.5f, e.z - sz * 0.5f };
					const float size[3] = { sz, sz, sz };
					box(tris, mn, size, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout);
					break;
				}
				case proto::kWeCrack: {  // block-breaking cracks over the block's box
					constexpr float g = 0.003f;
					const float mn[3] = { e.x - g, e.y - g, e.z - g };
					const float size[3] = { e.ext[0] + 2 * g, e.ext[1] + 2 * g, e.ext[2] + 2 * g };
					box(tris, mn, size, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent);
					break;
				}
				case proto::kWeArrow:
				case proto::kWeTrident: {  // Minecraft arrows face (sin yaw, sin pitch, cos yaw)
					const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
					const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
					arrow(tris, pos, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
					break;
				}
				case proto::kWeItem: {  // a flat sprite turning about the vertical
					const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
					const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
					const float pts[4][3] = {
						{ e.x - rx, e.y + half, e.z - rz },
						{ e.x + rx, e.y + half, e.z + rz },
						{ e.x + rx, e.y - half, e.z + rz },
						{ e.x - rx, e.y - half, e.z - rz },
					};
					quad(tris, pts, e.uv[0], 0xFFFFFFFFu, kFlagCutout);
					break;
				}
				default:
					break;  // shadows: not drawn yet
				}
			}
			if (w.hasSelection) {  // the targeted block: Minecraft's thin black outline
				constexpr float g = 0.002f;
				const float lo[3] = { w.selMin[0] - g, w.selMin[1] - g, w.selMin[2] - g };
				const float hi[3] = { w.selMax[0] + g, w.selMax[1] + g, w.selMax[2] + g };
				static constexpr int kEdges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
				constexpr std::uint32_t kOutline = 0x73000000u;  // black, 45%
				for (const auto& edge : kEdges) {
					for (int k : edge)
						lines.push_back({ (k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2], 0, 0, kOutline, kFullSkyLight,
							kFlagUntextured | kFlagTranslucent });
				}
			}
		}

		void uploadTexture(ID3D11Device* device, const PendingTexture& t)
		{
			GpuTexture& g = r_textures[t.id];
			release(g.srv);
			release(g.tex);
			D3D11_TEXTURE2D_DESC td{};
			td.Width = t.w;
			td.Height = t.h;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA init{ t.pixels.data(), t.w * 4, 0 };
			if (FAILED(device->CreateTexture2D(&td, &init, &g.tex)) || FAILED(device->CreateShaderResourceView(g.tex, nullptr, &g.srv)))
				r_textures.erase(t.id);
		}

		// One dynamic vertex buffer per frame for every entity vertex list, back to back.
		bool uploadDynamic(ID3D11Device* device, ID3D11DeviceContext* ctx, std::initializer_list<const std::vector<proto::RenVertex>*> parts)
		{
			std::size_t total = 0;
			for (const auto* v : parts)
				total += v ? v->size() : 0;
			if (!total)
				return false;
			const UINT bytes = UINT(total * sizeof(proto::RenVertex));
			if (bytes > r_dynCapacity) {
				release(r_dynVb);
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = std::max<UINT>(bytes * 2, 256 * 1024);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (FAILED(device->CreateBuffer(&bd, nullptr, &r_dynVb))) {
					r_dynCapacity = 0;
					return false;
				}
				r_dynCapacity = bd.ByteWidth;
			}
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(r_dynVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return false;
			auto* dst = static_cast<proto::RenVertex*>(m.pData);
			for (const auto* v : parts) {
				if (v && !v->empty()) {
					std::memcpy(dst, v->data(), v->size() * sizeof(proto::RenVertex));
					dst += v->size();
				}
			}
			ctx->Unmap(r_dynVb, 0);
			return true;
		}

		void setSection(ID3D11DeviceContext* ctx, const float origin[3], bool translucent)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(ctx->Map(r_sectionCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return;
			SectionCb sc{ { origin[0], origin[1], origin[2], 0 }, translucent ? 1.0f : 0.0f, {} };
			std::memcpy(m.pData, &sc, sizeof(sc));
			ctx->Unmap(r_sectionCb, 0);
		}

		// Minecraft's meshed entities: each batch with its own texture (0: the block/item atlas).
		void drawBatches(ID3D11DeviceContext* ctx, const Batches& b, const float origin[3], UINT base)
		{
			for (const proto::RenBatch& batch : b.batches) {
				if (!batch.count || batch.first + batch.count > b.verts.size())
					continue;
				ID3D11ShaderResourceView* srv = r_atlasSrv;
				if (batch.texture) {
					auto it = r_textures.find(batch.texture);
					if (it == r_textures.end())
						continue;  // not received yet
					srv = it->second.srv;
				}
				setSection(ctx, origin, (batch.flags & 1) != 0);
				ctx->PSSetShaderResources(0, 1, &srv);
				ctx->Draw(batch.count, base + batch.first);
			}
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
		case proto::kRenTexture: {
			if (bytes < sizeof(proto::RenTexture))
				return;
			proto::RenTexture t;
			std::memcpy(&t, p, sizeof(t));
			const std::size_t n = std::size_t(t.width) * t.height * 4;
			if (!t.id || bytes < sizeof(t) + n)
				return;
			std::lock_guard lock(g_mutex);
			g_textures.push_back({ t.id, t.width, t.height, std::vector<std::uint8_t>(p + sizeof(t), p + sizeof(t) + n) });
			break;
		}
		case proto::kRenScene:
		case proto::kRenAvatar: {
			auto b = std::make_shared<Batches>();
			std::uint32_t batchCount = 0, vertexCount = 0;
			std::size_t at = 0;
			if (type == proto::kRenScene) {
				if (bytes < sizeof(proto::RenScene))
					return;
				proto::RenScene h;
				std::memcpy(&h, p, sizeof(h));
				b->origin[0] = float(h.originX);
				b->origin[1] = float(h.originY);
				b->origin[2] = float(h.originZ);
				batchCount = h.batchCount;
				vertexCount = h.vertexCount;
				at = sizeof(h);
			} else {
				if (bytes < sizeof(proto::RenAvatar))
					return;
				proto::RenAvatar h;
				std::memcpy(&h, p, sizeof(h));
				batchCount = h.batchCount;
				vertexCount = h.vertexCount;
				at = sizeof(h);
			}
			if (bytes < at + std::size_t(batchCount) * sizeof(proto::RenBatch) + std::size_t(vertexCount) * sizeof(proto::RenVertex))
				return;
			b->batches.resize(batchCount);
			std::memcpy(b->batches.data(), p + at, std::size_t(batchCount) * sizeof(proto::RenBatch));
			at += std::size_t(batchCount) * sizeof(proto::RenBatch);
			b->verts.resize(vertexCount);
			std::memcpy(b->verts.data(), p + at, std::size_t(vertexCount) * sizeof(proto::RenVertex));
			std::lock_guard lock(g_mutex);
			(type == proto::kRenScene ? g_scene : g_avatar) = std::move(b);
			break;
		}
		default:
			break;  // lights, ragdoll, dug blocks: later phases
		}
	}

	void draw(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, unsigned width, unsigned height, const game::CameraState& cam,
		const float feet[3], ID3D11ShaderResourceView* sceneDepth)
	{
		if (!initPipeline(device))
			return;

		// Take what the worker received.
		bool clear = false;
		std::shared_ptr<std::vector<std::uint8_t>> atlas;
		UINT atlasW = 0, atlasH = 0;
		std::vector<Region> regions;
		std::unordered_map<long long, std::shared_ptr<SectionMesh>> sections;
		std::vector<PendingTexture> textures;
		proto::WorldEntities entities;
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
			textures.swap(g_textures);
			if (g_scene)
				r_scene = std::move(g_scene);
			if (g_avatar)
				r_avatar = std::move(g_avatar);
			entities = g_entities;
		}
		for (const PendingTexture& t : textures)
			uploadTexture(device, t);
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

		// This frame's entity vertices, in one dynamic buffer: scene | avatar | built | outline lines.
		std::vector<proto::RenVertex> dyn;
		std::vector<proto::RenVertex> lines;
		buildEntities(entities, dyn, lines);
		const UINT sceneFirst = 0, sceneCount = r_scene ? UINT(r_scene->verts.size()) : 0;
		const UINT avatarFirst = sceneCount, avatarCount = r_avatar ? UINT(r_avatar->verts.size()) : 0;
		const UINT builtFirst = avatarFirst + avatarCount, builtCount = UINT(dyn.size());
		const UINT linesFirst = builtFirst + builtCount, linesCount = UINT(lines.size());
		const bool haveDyn = uploadDynamic(device, ctx, { r_scene ? &r_scene->verts : nullptr, r_avatar ? &r_avatar->verts : nullptr, &dyn, &lines });

		if (!r_atlasSrv || (r_sections.empty() && !haveDyn) || !ensureDepth(device, width, height))
			return;

		const bool haveScene = g_sceneLight && captureScene(device, ctx, rtv);

		FrameCb frame{};
		viewProjection(cam, float(width) / float(height), frame.viewProj);
		frame.sceneLight = haveScene ? 1.0f : 0.0f;
		frame.localMip = 5.0f;  // 1/32 of the screen: the light around a block, not its own outline
		frame.globalMip = r_sceneMips ? float(r_sceneMips - 1) : 0.0f;
		frame.viewport[0] = float(width);
		frame.viewport[1] = float(height);
		frame.midGrey = g_midGrey;

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
		if (haveScene)
			ctx->PSSetShaderResources(2, 1, &r_sceneColorSrv);
		ID3D11SamplerState* samplers[2] = { r_sampler, r_samplerLinear };
		ctx->PSSetSamplers(0, 2, samplers);
		ID3D11Buffer* cbs[2] = { r_frameCb, r_sectionCb };
		ctx->VSSetConstantBuffers(0, 2, cbs);
		ctx->PSSetConstantBuffers(0, 2, cbs);

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
				const float origin[3] = { float(s.sx * 16), float(s.sy * 16), float(s.sz * 16) };
				setSection(ctx, origin, false);
				const UINT stride = sizeof(proto::RenVertex), offset = 0;
				ctx->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
				ctx->PSSetShaderResources(0, 1, &r_atlasSrv);
				ctx->Draw(s.count, 0);
				if (pass == 0) {
					++drawnSections;
					drawnVerts += s.count;
				}
			}
			if (haveDyn) {
				const UINT stride = sizeof(proto::RenVertex), offset = 0;
				ctx->IASetVertexBuffers(0, 1, &r_dynVb, &stride, &offset);
				// Minecraft's meshed entities and particles, then the player's own body (third person).
				if (r_scene)
					drawBatches(ctx, *r_scene, r_scene->origin, sceneFirst);
				if (r_avatar)
					drawBatches(ctx, *r_avatar, feet, avatarFirst);
				// Dropped items, arrows, cracks (atlas, Minecraft coords).
				const float zero[3] = {};
				setSection(ctx, zero, false);
				ctx->PSSetShaderResources(0, 1, &r_atlasSrv);
				if (builtCount)
					ctx->Draw(builtCount, builtFirst);
				if (pass == 1 && linesCount) {
					ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
					ctx->Draw(linesCount, linesFirst);
					ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				}
			}
		}
		ID3D11ShaderResourceView* none = nullptr;
		ctx->PSSetShaderResources(2, 1, &none);  // it's a render target again for next frame's mips
		g_drawnSections = drawnSections;
		g_drawnVerts = drawnVerts;
	}

	void setWorldEntities(const proto::WorldEntities& entities)
	{
		std::lock_guard lock(g_mutex);
		g_entities = entities;
	}

	namespace
	{
		std::mutex g_feetMutex;
		float      g_driveFeet[3] = {};
	}

	void setDriveFeet(const float feet[3])
	{
		std::lock_guard lock(g_feetMutex);
		std::memcpy(g_driveFeet, feet, sizeof(g_driveFeet));
	}

	void driveFeet(float out[3])
	{
		std::lock_guard lock(g_feetMutex);
		std::memcpy(out, g_driveFeet, sizeof(g_driveFeet));
	}

	void setSceneLighting(bool on, float midGrey)
	{
		g_sceneLight = on ? 1 : 0;
		g_midGrey = midGrey;
	}

	void releaseGpu()
	{
		for (auto& [id, t] : r_textures) {
			release(t.srv);
			release(t.tex);
		}
		r_textures.clear();
		release(r_dynVb);
		r_dynCapacity = 0;
		r_scene.reset();
		r_avatar.reset();
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
		release(r_samplerLinear);
		release(r_sceneColorSrv);
		release(r_sceneColor);
		r_sceneW = r_sceneH = r_sceneMips = 0;
		r_sceneFormat = DXGI_FORMAT_UNKNOWN;
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
