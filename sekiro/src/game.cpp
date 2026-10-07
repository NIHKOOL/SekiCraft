#include "game.h"

#include "log.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace sekicraft::game
{
	namespace
	{
		// WorldChrMan: "mov [rip+rel32], rax" 3 bytes into this pattern stores the singleton.
		constexpr const char* kWorldChrManAob = "48 8B C6 48 89 05 ?? ?? ?? ?? 48 85 C0";
		constexpr std::uintptr_t kWorldChrManStatic106 = 0x3D7A1E0;

		// [[[[WorldChrMan] + 0x88] + 0x1FF8] + 0x68] = player physics module
		constexpr std::uintptr_t kWcmPlayer = 0x88;
		constexpr std::uintptr_t kChrModules = 0x1FF8;
		constexpr std::uintptr_t kModulesPhysics = 0x68;
		constexpr std::uintptr_t kChrDrawFlags = 0x1A11;  // byte; bit 3 = drawn
		constexpr std::uint8_t   kChrDrawBit = 1u << 3;
		// physics module fields
		constexpr std::uintptr_t kPhysTheta = 0x74;  // float, facing (radians)
		constexpr std::uintptr_t kPhysPos = 0x80;    // float[3]

		// FieldArea: "mov [rip+rel32], rax" 7 bytes into this pattern.
		constexpr const char* kFieldAreaAob = "48 3b c7 48 0f 44 c5 48 89 05";
		constexpr std::uintptr_t kFieldAreaStatic106 = 0x3D5C0A0;
		constexpr std::uintptr_t kFieldAreaCamMan = 0x20;
		constexpr std::uintptr_t kCamManFreeCamMode = 0xE0;  // byte
		constexpr std::uintptr_t kCamManFreeCam = 0xE8;      // pointer to the free camera
		constexpr std::uintptr_t kCamMatrix = 0x10;          // float[4][4]
		constexpr std::uintptr_t kCamFov = 0x50;             // float, vertical, radians

		// FrpgHavokMan: "mov rbx, [rip+rel32]" at the start of this pattern; physics world at +0x98.
		constexpr const char* kHavokManAob = "48 8B 1D ?? ?? ?? ?? F3 0F 10 4D";
		constexpr std::uintptr_t kHavokManStatic106 = 0x3D6D640;
		constexpr std::uintptr_t kHavokManWorld = 0x98;
		// A call to FrpgCastRay: "call rel32; test al, al; je +0x4F".
		constexpr const char* kCastRayCallAob = "E8 ?? ?? ?? ?? 84 C0 74 4F 0F";
		constexpr std::uintptr_t kCastRay106 = 0x94CC50;
		constexpr std::uint8_t kCastRayPrologue[16] = { 0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D };
		constexpr std::uint32_t kCastRayFilter = 0x4E;

		using CastRayFn = bool (*)(std::uintptr_t world, std::uint32_t filter, const float* start, const float* delta,
			float* hitPos, float* hitNormal, float* fraction, std::uintptr_t* hitObject);

		std::uintptr_t g_worldChrManStatic = 0;
		std::uintptr_t g_fieldAreaStatic = 0;
		std::uintptr_t g_havokManStatic = 0;
		CastRayFn      g_castRay = nullptr;

		std::optional<std::vector<int>> parsePattern(const char* text)
		{
			std::vector<int> out;
			for (const char* p = text; *p;) {
				if (*p == ' ') {
					++p;
					continue;
				}
				if (p[0] == '?') {
					out.push_back(-1);
					p += (p[1] == '?') ? 2 : 1;
					continue;
				}
				char hex[3] = { p[0], p[1], 0 };
				out.push_back(int(std::strtoul(hex, nullptr, 16)));
				p += 2;
			}
			return out;
		}

		// First match of the pattern in the executable sections of sekiro.exe.
		std::uintptr_t scan(const char* text)
		{
			const auto pattern = parsePattern(text);
			const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
				if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
					continue;
				const auto* begin = reinterpret_cast<const std::uint8_t*>(base + sec->VirtualAddress);
				const std::size_t size = sec->Misc.VirtualSize;
				std::vector<std::uint8_t> copy(size);
				if (!readBytes(reinterpret_cast<std::uintptr_t>(begin), copy.data(), size))
					continue;
				const auto& pat = *pattern;
				for (std::size_t at = 0; at + pat.size() <= size; ++at) {
					std::size_t k = 0;
					while (k < pat.size() && (pat[k] < 0 || copy[at + k] == pat[k]))
						++k;
					if (k == pat.size())
						return reinterpret_cast<std::uintptr_t>(begin) + at;
				}
			}
			return 0;
		}

		std::uintptr_t deref(std::uintptr_t addr)
		{
			std::uintptr_t v = 0;
			return read(addr, v) ? v : 0;
		}
	}

	bool readBytes(std::uintptr_t addr, void* out, std::size_t n)
	{
		if (!addr)
			return false;
		__try {
			std::memcpy(out, reinterpret_cast<const void*>(addr), n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool writeBytes(std::uintptr_t addr, const void* in, std::size_t n)
	{
		if (!addr)
			return false;
		__try {
			std::memcpy(reinterpret_cast<void*>(addr), in, n);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool init()
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (const std::uintptr_t hit = scan(kWorldChrManAob)) {
			const std::uintptr_t insn = hit + 3;  // mov [rip+rel32], rax: 48 89 05 rel32 (7 bytes)
			std::int32_t rel = 0;
			read(insn + 3, rel);
			g_worldChrManStatic = insn + 7 + rel;
		} else {
			logf("game: WorldChrMan pattern not found; using the 1.06 static offset");
			g_worldChrManStatic = base + kWorldChrManStatic106;
		}
		logf("game: WorldChrMan at sekiro.exe+0x%llX%s", (unsigned long long)(g_worldChrManStatic - base),
			g_worldChrManStatic - base == kWorldChrManStatic106 ? " (matches 1.06)" : " (differs from 1.06!)");

		if (const std::uintptr_t hit = scan(kFieldAreaAob)) {
			const std::uintptr_t insn = hit + 7;  // mov [rip+rel32], rax (7 bytes)
			std::int32_t rel = 0;
			read(insn + 3, rel);
			g_fieldAreaStatic = insn + 7 + rel;
		} else {
			logf("game: FieldArea pattern not found; using the 1.06 static offset");
			g_fieldAreaStatic = base + kFieldAreaStatic106;
		}
		logf("game: FieldArea at sekiro.exe+0x%llX%s", (unsigned long long)(g_fieldAreaStatic - base),
			g_fieldAreaStatic - base == kFieldAreaStatic106 ? " (matches 1.06)" : " (differs from 1.06!)");

		if (const std::uintptr_t hit = scan(kHavokManAob)) {
			std::int32_t rel = 0;
			read(hit + 3, rel);
			g_havokManStatic = hit + 7 + rel;
		} else {
			g_havokManStatic = base + kHavokManStatic106;
		}
		std::uintptr_t castRay = 0;
		if (const std::uintptr_t hit = scan(kCastRayCallAob)) {
			std::int32_t rel = 0;
			read(hit + 1, rel);
			castRay = hit + 5 + rel;
		} else {
			castRay = base + kCastRay106;
		}
		std::uint8_t prologue[16] = {};
		if (read(castRay, prologue) && std::memcmp(prologue, kCastRayPrologue, sizeof(prologue)) == 0)
			g_castRay = reinterpret_cast<CastRayFn>(castRay);
		logf("game: FrpgHavokMan at sekiro.exe+0x%llX%s; FrpgCastRay at sekiro.exe+0x%llX%s", (unsigned long long)(g_havokManStatic - base),
			g_havokManStatic - base == kHavokManStatic106 ? " (matches 1.06)" : " (differs from 1.06!)", (unsigned long long)(castRay - base),
			g_castRay ? (castRay - base == kCastRay106 ? " (matches 1.06)" : " (prologue ok, differs from 1.06)") : " (prologue mismatch: ray casts off)");
		return true;
	}

	bool castRayReady()
	{
		return g_castRay != nullptr;
	}

	namespace
	{
		bool callCastRay(std::uintptr_t world, const float* start, const float* delta, float* hit, float* normal, float* fraction, std::uintptr_t* object)
		{
			__try {
				return g_castRay(world, kCastRayFilter, start, delta, hit, normal, fraction, object);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool castRay(Vec3 start, Vec3 delta, RayHit& out)
	{
		if (!g_castRay)
			return false;
		const std::uintptr_t havokMan = deref(g_havokManStatic);
		const std::uintptr_t world = havokMan ? deref(havokMan + kHavokManWorld) : 0;
		if (world < 0x10000)
			return false;
		alignas(16) float s[4] = { start.x, start.y, start.z, 1.0f };
		alignas(16) float d[4] = { delta.x, delta.y, delta.z, 0.0f };
		alignas(16) float hit[4] = {};
		alignas(16) float normal[4] = {};
		float fraction = 0;
		std::uintptr_t object = 0;
		if (!callCastRay(world, s, d, hit, normal, &fraction, &object))
			return false;
		out.pos = { hit[0], hit[1], hit[2] };
		out.normal = { normal[0], normal[1], normal[2] };
		out.fraction = fraction;
		return std::isfinite(hit[0]) && std::isfinite(hit[1]) && std::isfinite(hit[2]) && std::isfinite(normal[1]);
	}

	namespace
	{
		std::uintptr_t cameraManager()
		{
			const std::uintptr_t fa = deref(g_fieldAreaStatic);
			return fa ? deref(fa + kFieldAreaCamMan) : 0;
		}
	}

	bool setFreeCamera(bool on)
	{
		const std::uintptr_t cm = cameraManager();
		const std::uint8_t mode = on ? 1 : 0;
		return cm && writeBytes(cm + kCamManFreeCamMode, &mode, 1);
	}

	bool readFreeCamera(CameraState& out)
	{
		const std::uintptr_t cm = cameraManager();
		const std::uintptr_t cam = cm ? deref(cm + kCamManFreeCam) : 0;
		float lens[4] = {};
		return cam && readBytes(cam + kCamMatrix, out.world, sizeof(out.world)) && readBytes(cam + kCamFov, lens, sizeof(lens)) &&
			((out.fov = lens[0]), (out.aspect = lens[1]), (out.nearZ = lens[2]), (out.farZ = lens[3]), true);
	}

	bool writeCamera(Vec3 pos, float theta, float pitch, float fovRadians)
	{
		const std::uintptr_t cm = cameraManager();
		const std::uintptr_t cam = cm ? deref(cm + kCamManFreeCam) : 0;
		if (!cam)
			return false;
		const float st = std::sin(theta), ct = std::cos(theta);
		const float sp = std::sin(pitch), cp = std::cos(pitch);
		// forward = horizontal facing tilted down by pitch; up tilts toward it; right = up x forward.
		const Vec3 fwd{ -st * cp, -sp, -ct * cp };
		const Vec3 up{ -st * sp, cp, -ct * sp };
		const Vec3 right{ up.y * fwd.z - up.z * fwd.y, up.z * fwd.x - up.x * fwd.z, up.x * fwd.y - up.y * fwd.x };
		const float m[16] = {
			right.x, right.y, right.z, 0.0f,
			up.x, up.y, up.z, 0.0f,
			fwd.x, fwd.y, fwd.z, 0.0f,
			pos.x, pos.y, pos.z, 1.0f,
		};
		return writeBytes(cam + kCamMatrix, m, sizeof(m)) && writeBytes(cam + kCamFov, &fovRadians, sizeof(float));
	}

	std::uintptr_t playerPhysics()
	{
		const std::uintptr_t wcm = deref(g_worldChrManStatic);
		const std::uintptr_t player = wcm ? deref(wcm + kWcmPlayer) : 0;
		const std::uintptr_t modules = player ? deref(player + kChrModules) : 0;
		return modules ? deref(modules + kModulesPhysics) : 0;
	}

	bool readPlayer(PlayerState& out)
	{
		const std::uintptr_t phys = playerPhysics();
		return phys && read(phys + kPhysPos, out.pos) && read(phys + kPhysTheta, out.theta);
	}

	bool writePlayerPos(Vec3 pos)
	{
		const std::uintptr_t phys = playerPhysics();
		return phys && writeBytes(phys + kPhysPos, &pos, sizeof(pos));
	}

	bool setPlayerDrawn(bool drawn)
	{
		const std::uintptr_t wcm = deref(g_worldChrManStatic);
		const std::uintptr_t player = wcm ? deref(wcm + kWcmPlayer) : 0;
		std::uint8_t flags = 0;
		if (!player || !read(player + kChrDrawFlags, flags))
			return false;
		const std::uint8_t want = drawn ? std::uint8_t(flags | kChrDrawBit) : std::uint8_t(flags & ~kChrDrawBit);
		return want == flags || writeBytes(player + kChrDrawFlags, &want, 1);
	}

	bool writePlayerTheta(float theta)
	{
		const std::uintptr_t phys = playerPhysics();
		return phys && writeBytes(phys + kPhysTheta, &theta, sizeof(theta));
	}
}
