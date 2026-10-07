// sekicraft_core.dll: SekiCraft's logic inside sekiro.exe, hot-reloaded by sekicraft.dll.
//
// Two control modes, switched with hotkeys while Sekiro has focus:
//   F9 / Esc  Sekiro controls (default): you play Wolf normally. Minecraft follows him, standing
//             on a temporary flat floor at his height.
//   F10       Minecraft controls: keyboard and mouse go to Minecraft (Sekiro sees an idle
//             keyboard and mouse), Minecraft's player position drives Wolf, and Sekiro's camera
//             becomes a first-person camera at Minecraft's eye.
#include "collision.h"
#include "game.h"
#include "input.h"
#include "log.h"
#include "overlay.h"
#include "scenedepth.h"
#include "world.h"
#include "sekicraft_flatfloor.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

namespace proto = sekicraft::proto;
using namespace sekicraft;

namespace
{
	std::atomic<bool> g_stop{ false };
	HANDLE            g_thread = nullptr;
	bool              g_inputHooked = false;
	bool              g_overlayHooked = false;

	enum class Mode { kSekiro, kMinecraft };

	bool gameHasFocus()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	// Edge-triggered hotkey (only while Sekiro is the foreground window).
	bool pressed(int vk, bool& wasDown)
	{
		const bool down = gameHasFocus() && (GetAsyncKeyState(vk) & 0x8000);
		const bool edge = down && !wasDown;
		wasDown = down;
		return edge;
	}

	double horizDist(double ax, double az, double bx, double bz) { return std::hypot(ax - bx, az - bz); }

	// ---- F8 ray probe (runs on the game thread) ----
	// Casts down, then up, through Wolf's column, continuing past each hit, and logs every surface:
	// tells us whether ray casts hit surfaces from behind and whether Wolf's own body is hit.
	std::atomic<bool> g_probeRequested{ false };

	void probeColumn(game::Vec3 at, float dirY)
	{
		const float span = 30.0f;
		game::Vec3 start{ at.x, at.y - dirY * span, at.z };
		float remaining = 2 * span;
		for (int i = 0; i < 16 && remaining > 0.05f; ++i) {
			game::RayHit hit{};
			if (!game::castRay(start, { 0, dirY * remaining, 0 }, hit)) {
				logf("probe %s: hit %d: nothing more (%.1f m left)", dirY < 0 ? "down" : "up", i, remaining);
				return;
			}
			logf("probe %s: hit %d at y %.3f (%+.2f m from Wolf's feet) normal (%.2f, %.2f, %.2f) fraction %.3f", dirY < 0 ? "down" : "up", i,
				hit.pos.y, hit.pos.y - at.y, hit.normal.x, hit.normal.y, hit.normal.z, hit.fraction);
			const float moved = std::abs(hit.pos.y - start.y) + 0.02f;
			remaining -= moved;
			start = { at.x, hit.pos.y + dirY * 0.02f, at.z };
		}
	}

	// ---- Minecraft driving Wolf and the camera ----
	// The worker decides where Wolf and the camera go; the game thread writes them at the start of
	// each frame. Sekiro draws that frame and calls Present on the same thread, so the blocks we draw
	// in Present see exactly the camera Sekiro rendered with (no swimming).
	struct Drive
	{
		bool       active = false;
		game::Vec3 wolf{};    // Sekiro coords
		float      theta = 0;
		game::Vec3 eye{};     // Sekiro coords
		float      pitch = 0; // radians
		float      fov = 1.2f;
	};
	std::mutex             g_driveMutex;
	Drive                  g_drive;
	bool                   g_driveApplied = false;  // game thread only
	std::atomic<int>       g_camWrites{ 0 }, g_camFails{ 0 };
	std::atomic<float>     g_centreRay{ NAN };

	void applyDrive()
	{
		Drive d;
		{
			std::lock_guard lock(g_driveMutex);
			d = g_drive;
		}
		if (!d.active) {
			if (g_driveApplied) {
				g_driveApplied = false;
				game::setFreeCamera(false);
				game::setPlayerDrawn(true);
			}
			return;
		}
		if (!g_driveApplied) {
			g_driveApplied = true;
			game::setFreeCamera(true);
		}
		game::writePlayerPos(d.wolf);
		game::writePlayerTheta(d.theta);
		game::setPlayerDrawn(false);  // every frame: the game may turn it back on
		if (game::writeCamera(d.eye, d.theta, d.pitch, d.fov))
			++g_camWrites;
		else
			++g_camFails;

		// Diagnostics for the depth buffer: how far the camera's centre ray really goes.
		static int frame = 0;
		if (++frame % 10 == 0 && game::castRayReady()) {
			const float ct = std::cos(d.pitch);
			const game::Vec3 fwd{ -std::sin(d.theta) * ct, -std::sin(d.pitch), -std::cos(d.theta) * ct };
			game::RayHit hit{};
			g_centreRay = game::castRay(d.eye, { fwd.x * 500, fwd.y * 500, fwd.z * 500 }, hit) ? hit.fraction * 500.0f : NAN;
		}
	}

	void onGameThreadTick()
	{
		applyDrive();
		collision::tickGameThread();
		if (!g_probeRequested.exchange(false))
			return;
		game::PlayerState wolf{};
		if (!game::readPlayer(wolf)) {
			logf("probe: no player");
			return;
		}
		logf("probe: Wolf at (%.2f, %.2f, %.2f); casting through his column", wolf.pos.x, wolf.pos.y, wolf.pos.z);
		probeColumn(wolf.pos, -1.0f);
		probeColumn(wolf.pos, 1.0f);
		// A sideways ray at chest height, toward where Wolf faces: walls.
		game::RayHit hit{};
		const game::Vec3 chest{ wolf.pos.x, wolf.pos.y + 1.2f, wolf.pos.z };
		const game::Vec3 fwd{ -std::sin(wolf.theta) * 20.0f, 0, -std::cos(wolf.theta) * 20.0f };
		if (game::castRay(chest, fwd, hit))
			logf("probe forward: hit at %.2f m, normal (%.2f, %.2f, %.2f)", hit.fraction * 20.0f, hit.normal.x, hit.normal.y, hit.normal.z);
		else
			logf("probe forward: nothing within 20 m");
	}

	DWORD WINAPI worker(void*)
	{
		GameLink link;
		for (;;) {
			const auto r = link.open();
			if (r == GameLink::OpenResult::kCreated || r == GameLink::OpenResult::kTookOver) {
				logf("core: shared memory %ls %s", proto::kMappingName, r == GameLink::OpenResult::kCreated ? "created" : "reopened");
				break;
			}
			logf(r == GameLink::OpenResult::kOtherGameLive ? "core: another game side owns the link; retrying" : "core: couldn't create shared memory; retrying");
			for (int i = 0; i < 50 && !g_stop; ++i)
				Sleep(100);
			if (g_stop)
				return 0;
		}

		overlay::setLink(&link);

		proto::SkyState st{};
		st.collisionEpoch = link.previousCollisionEpoch();
		st.teleportSeq = link.previousTeleportSeq();
		st.viewportW = 1920;
		st.viewportH = 1080;
		st.gameHour = 12.0f;

		Mode mode = Mode::kSekiro;
		bool f7 = false, f8 = false, f9 = false, f10 = false;
		int cameraLag = 1;  // F7 cycles 0-2 (debug); 1 matches Sekiro
		bool linked = false, wasInGame = false, haveFloor = false, haveFloorHeight = false;
		double floorX = 0, floorZ = 0, floorY = 0;
		game::Vec3 lastSent{ 1e9f, 1e9f, 1e9f };
		ULONGLONG lastTeleport = 0, lastLog = 0, lastStats = 0;
		std::uint64_t lastFrame = 0;
		float lookYaw = 0, lookPitch = 0, sensitivity = 0.5f;
		float cursorX = 0, cursorY = 0;
		bool screenWasOpen = false;

		auto toSekiro = [&](const char* why) {
			if (mode == Mode::kSekiro)
				return;
			mode = Mode::kSekiro;
			input::setRouting(false);
			{
				std::lock_guard lock(g_driveMutex);
				g_drive.active = false;
			}
			// Also undo it here: the game thread may not tick again (unloading, unfocused).
			game::setFreeCamera(false);
			game::setPlayerDrawn(true);
			lastSent = { 1e9f, 1e9f, 1e9f };  // teleport Minecraft back onto Wolf
			logf("core: %s -> Sekiro controls", why);
		};

		while (!g_stop) {
			const ULONGLONG now = GetTickCount64();
			link.heartbeat();
			link.drainRender(&world::consume);
			link.drainEvents([](const proto::McEvent&) {});

			const bool alive = link.minecraftAlive();
			if (alive != linked) {
				linked = alive;
				logf(alive ? "core: Minecraft linked (pid %u)" : "core: Minecraft heartbeat lost", link.minecraftPid());
				if (!alive)
					toSekiro("Minecraft gone");
			}

			game::PlayerState wolf{};
			const bool inGame = game::readPlayer(wolf);
			if (inGame != wasInGame) {
				wasInGame = inGame;
				haveFloor = haveFloorHeight = false;
				logf(inGame ? "core: Wolf is in the world" : "core: no player (title screen or loading)");
				if (!inGame) {
					toSekiro("no player");
					collision::reset();
				}
			}
			st.flags = inGame ? proto::kSkyInGame : 0;

			proto::McState mc{};
			const bool haveMc = linked && link.readMcState(mc) && (mc.flags & proto::kMcInWorld);
			if (haveMc && mc.sensitivity > 0)
				sensitivity = mc.sensitivity;

			// ---- mode switches ----
			if (pressed(VK_F8, f8)) {
				if (game::castRayReady())
					g_probeRequested = true;
				else
					logf("core: F8 ignored: ray casts unavailable");
			}
			if (pressed(VK_F7, f7)) {
				cameraLag = (cameraLag + 1) % 3;
				overlay::setCameraLag(cameraLag);
				logf("core: F7 -> blocks drawn with the camera from %d frame(s) ago", cameraLag);
			}
			if (pressed(VK_F9, f9))
				toSekiro("F9");
			if (pressed(VK_F10, f10) && mode == Mode::kSekiro) {
				if (!g_inputHooked)
					logf("core: F10 ignored: input hooks aren't installed");
				else if (!inGame || !haveMc)
					logf("core: F10 ignored: %s", !inGame ? "no player" : "Minecraft isn't in its world yet");
				else {
					mode = Mode::kMinecraft;
					lookYaw = game::yawToMc(wolf.theta);
					lookPitch = 0;
					g_camWrites = g_camFails = 0;
					input::setRouting(true);
					logf("core: F10 -> Minecraft controls");
				}
			}

			// Sekiro's real screen size (Minecraft sizes its overlay to match).
			std::uint32_t viewW = 0, viewH = 0;
			overlay::viewport(viewW, viewH);
			if (viewW && viewH) {
				st.viewportW = viewW;
				st.viewportH = viewH;
			}

			// ---- input (only gathered while routing) ----
			const input::Pending in = input::take();
			const bool screenOpen = mode == Mode::kMinecraft && haveMc && (mc.flags & proto::kMcScreenOpen);
			input::setEscapeToMinecraft(screenOpen);
			if (screenOpen) {
				// A Minecraft screen (inventory, chat, ...): the mouse moves a cursor, in overlay pixels.
				if (!screenWasOpen) {
					cursorX = st.viewportW * 0.5f;
					cursorY = st.viewportH * 0.5f;
				}
				const float nx = std::clamp(cursorX + in.lookDx, 0.0f, float(st.viewportW) - 1);
				const float ny = std::clamp(cursorY + in.lookDy, 0.0f, float(st.viewportH) - 1);
				if (nx != cursorX || ny != cursorY || !screenWasOpen) {
					cursorX = nx;
					cursorY = ny;
					link.sendInput(proto::kInCursor, 0, int(cursorX), int(cursorY));
				}
			}
			screenWasOpen = screenOpen;
			for (const auto& e : in.events)
				link.sendInput(e.type, e.code, e.a, e.b, e.c);
			if (in.escapePressed)
				toSekiro("Esc");
			{
				overlay::Settings ov;
				ov.show = mode == Mode::kMinecraft && haveMc;
				ov.world = ov.show;  // the free camera is what Sekiro renders only while Minecraft drives
				ov.cursor = screenOpen;
				ov.cursorX = cursorX;
				ov.cursorY = cursorY;
				ov.crosshair = ov.show && !screenOpen && mc.cameraMode == 0;
				ov.guiScale = int(mc.guiScale);
				overlay::update(ov);
			}
			if (mode == Mode::kMinecraft && !(mc.flags & proto::kMcScreenOpen)) {
				// Minecraft's own mouse-look formula, integrated here so the camera has no added latency.
				const float s = sensitivity * 0.6f + 0.2f;
				const float factor = s * s * s * 8.0f * 0.15f;
				lookYaw = std::fmod(lookYaw + in.lookDx * factor, 360.0f);
				lookPitch = std::clamp(lookPitch + in.lookDy * factor, -90.0f, 90.0f);
			}

			if (inGame) {
				const game::Vec3 m = game::toMc(wolf.pos);

				// Sekiro's real collision, measured with ray casts on the game thread. Without ray
				// casts: a temporary flat floor at Wolf's height (while Minecraft drives, Wolf's height
				// is Minecraft's, so the floor keeps its height then).
				if (game::castRayReady())
					collision::update(link, true, m, st.collisionEpoch);
				const bool followHeight = mode == Mode::kSekiro && std::abs(m.y - floorY) > 0.3;
				if (!game::castRayReady() && (!haveFloor || followHeight || horizDist(m.x, m.z, floorX, floorZ) > 24)) {
					haveFloor = true;
					floorX = m.x;
					floorZ = m.z;
					if (mode == Mode::kSekiro || !haveFloorHeight)
						floorY = m.y;
					haveFloorHeight = true;
					sekicraft::sendFlatFloor(link, ++st.collisionEpoch, floorX, floorZ, floorY);
				}

				if (mode == Mode::kSekiro) {
					// Minecraft follows Wolf; a hair above the floor so it lands on it.
					st.posX = m.x;
					st.posY = m.y + 0.02;
					st.posZ = m.z;
					st.yaw = game::yawToMc(wolf.theta);
					st.pitch = 0;
					const double moved = std::abs(m.x - lastSent.x) + std::abs(m.y - lastSent.y) + std::abs(m.z - lastSent.z);
					if (moved > 0.02 && now - lastTeleport >= 50) {
						++st.teleportSeq;
						lastTeleport = now;
						lastSent = m;
					}
				} else if (haveMc) {
					// Minecraft drives: Wolf stands where Minecraft's player is, facing its look.
					st.yaw = lookYaw;
					st.pitch = lookPitch;
					Drive d;
					d.active = true;
					d.wolf = game::fromMc({ float(mc.x), float(mc.y), float(mc.z) });
					d.theta = game::yawFromMc(lookYaw);
					d.eye = game::fromMc({ float(mc.eyeX), float(mc.eyeY), float(mc.eyeZ) });
					d.pitch = game::yawFromMc(lookPitch);
					// Minecraft's FOV changes while sprinting (and so around jumps); Sekiro eases into a new
					// FOV over a few frames while our blocks would use it at once, so they'd bob. Keep
					// Minecraft's normal 70 degrees instead.
					d.fov = 70.0f * float(3.14159265358979 / 180.0);
					std::lock_guard lock(g_driveMutex);
					g_drive = d;
				}
			}

			link.writeSkyState(st);

			if (now - lastStats >= 5000 && inGame && game::castRayReady()) {
				lastStats = now;
				logf("%s", collision::stats().c_str());
				logf("%s", world::stats().c_str());
				logf("%s", overlay::stats().c_str());
				logf("%s", scenedepth::stats().c_str());
			}
			if (now - lastLog >= 1000 && inGame) {
				lastLog = now;
				const std::uint64_t frames = mc.frameCounter - lastFrame;
				lastFrame = mc.frameCounter;
				if (mode == Mode::kSekiro) {
					logf("sekiro: wolf (%.2f, %.2f, %.2f) theta %.3f | mc (%.2f, %.2f, %.2f) flags 0x%02x %llu fps", wolf.pos.x, wolf.pos.y, wolf.pos.z, wolf.theta,
						mc.x, mc.y, mc.z, mc.flags, (unsigned long long)frames);
				} else {
					logf("depth probe: centre stored %.7f, centre ray %.2f m", scenedepth::centreDepth(), g_centreRay.load());
					logf("minecraft: mc (%.2f, %.2f, %.2f) yaw %.1f pitch %.1f flags 0x%02x %llu fps -> wolf (%.2f, %.2f, %.2f) | camera writes %d, failed %d | %s",
						mc.x, mc.y, mc.z, lookYaw, lookPitch, mc.flags, (unsigned long long)frames, wolf.pos.x, wolf.pos.y, wolf.pos.z, g_camWrites.load(), g_camFails.load(),
						input::callStats().c_str());
				}
			}
			Sleep(mode == Mode::kMinecraft ? 2 : 8);
		}

		toSekiro("unloading");
		overlay::setLink(nullptr);
		logf("core: stopped");
		return 0;
	}
}

extern "C" __declspec(dllexport) bool sekicraft_start(const wchar_t* dir)
{
	openLog(std::wstring(dir) + L"sekicraft.log", false);
	logf("core: starting (built " __DATE__ " " __TIME__ ")");
	if (!game::init())
		return false;
	g_inputHooked = input::install();
	input::setGameThreadTick(&onGameThreadTick);
	g_overlayHooked = g_inputHooked && overlay::install();  // MinHook is initialized by input::install
	g_stop = false;
	g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
	return g_thread != nullptr;
}

extern "C" __declspec(dllexport) void sekicraft_stop()
{
	// The overlay reads the link from the render thread: unhook it before the worker closes it.
	if (g_overlayHooked)
		overlay::uninstall();
	g_overlayHooked = false;
	g_stop = true;
	if (g_thread) {
		WaitForSingleObject(g_thread, 5000);
		CloseHandle(g_thread);
		g_thread = nullptr;
	}
	if (g_inputHooked)
		input::uninstall();
	collision::reset();
	g_inputHooked = false;
	closeLog();
}
