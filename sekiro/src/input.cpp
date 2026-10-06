#include "input.h"

#include "log.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <windows.h>

#include <MinHook.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sekicraft::input
{
	namespace
	{
		// DirectInput scan code -> SDL scancode (USB HID usage), which is what Minecraft 26.x uses.
		// Table from SkyCraft (MIT), which converts Skyrim's DirectInput codes the same way.
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		// Keys SekiCraft keeps for itself (hotkeys) and never forwards to Minecraft.
		constexpr std::uint8_t kDikEscape = 0x01;
		constexpr std::uint8_t kDikF9 = 0x43;
		constexpr std::uint8_t kDikF10 = 0x44;

		using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
		using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

		GetDeviceStateFn  g_origGetDeviceState = nullptr;
		GetDeviceDataFn   g_origGetDeviceData = nullptr;
		void*             g_targetGetDeviceState = nullptr;
		void*             g_targetGetDeviceData = nullptr;
		std::atomic<bool> g_routing{ false };
		std::atomic<int>  g_inFlight{ 0 };

		std::mutex                                   g_mutex;  // guards everything below
		Pending                                      g_pending;
		std::array<bool, 256>                        g_keyDown{};    // as last forwarded to Minecraft
		std::array<bool, 8>                          g_buttonDown{};
		std::unordered_map<IDirectInputDevice8W*, BYTE> g_deviceType;  // DI8DEVTYPE_* per device

		struct InFlight
		{
			InFlight() { ++g_inFlight; }
			~InFlight() { --g_inFlight; }
		};

		BYTE deviceType(IDirectInputDevice8W* dev)
		{
			{
				std::lock_guard lock(g_mutex);
				if (auto it = g_deviceType.find(dev); it != g_deviceType.end())
					return it->second;
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			const BYTE type = SUCCEEDED(dev->GetCapabilities(&caps)) ? BYTE(caps.dwDevType & 0xFF) : 0;
			std::lock_guard lock(g_mutex);
			g_deviceType[dev] = type;
			logf("input: DirectInput device %p is %s", static_cast<void*>(dev),
				type == DI8DEVTYPE_KEYBOARD ? "the keyboard" : type == DI8DEVTYPE_MOUSE ? "the mouse" : "something else");
			return type;
		}

		void push(std::uint16_t type, std::uint16_t code, std::int32_t a)
		{
			g_pending.events.push_back({ type, code, a, 0, 0 });
		}

		// Caller holds g_mutex.
		void keyChanged(std::uint8_t dik, bool down)
		{
			if (g_keyDown[dik] == down)
				return;
			g_keyDown[dik] = down;
			if (dik == kDikF9 || dik == kDikF10)
				return;
			if (dik == kDikEscape) {
				if (down)
					g_pending.escapePressed = true;
				return;
			}
			if (const std::uint16_t sdl = kDikToSdl[dik])
				push(proto::kInKey, sdl, down ? 1 : 0);
		}

		// DirectInput mouse buttons 0..7 -> SDL buttons (1 left, 2 middle, 3 right, 4/5 side).
		void buttonChanged(int index, bool down)
		{
			static constexpr std::uint16_t kSdl[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
			if (index < 0 || index >= 8 || g_buttonDown[index] == down)
				return;
			g_buttonDown[index] = down;
			if (kSdl[index])
				push(proto::kInMouseButton, kSdl[index], down ? 1 : 0);
		}

		// ---- cursor-based mouse look ----
		// Sekiro has no DirectInput mouse; it imports GetCursorPos/SetCursorPos. While routing, each
		// GetCursorPos measures how far the real cursor moved from where it was parked, hands that
		// to Minecraft as look, parks the cursor again, and tells Sekiro it never moved.
		using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
		using SetCursorPosFn = BOOL(WINAPI*)(int, int);
		using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);

		GetCursorPosFn    g_origGetCursorPos = nullptr;
		SetCursorPosFn    g_origSetCursorPos = nullptr;
		GetRawInputDataFn g_origGetRawInputData = nullptr;
		void*             g_targetGetCursorPos = nullptr;
		void*             g_targetSetCursorPos = nullptr;
		void*             g_targetGetRawInputData = nullptr;
		std::atomic<int>  g_cntGetCursorPos{ 0 }, g_cntSetCursorPos{ 0 }, g_cntRawInput{ 0 }, g_cntDiState{ 0 }, g_cntDiData{ 0 };
		std::atomic<GameThreadTick> g_tick{ nullptr };
		std::atomic<std::int64_t>   g_lastTickQpc{ 0 };
		std::atomic<DWORD>          g_tickThread{ 0 };

		// Sekiro polls four keyboard devices per frame back to back; run the tick on the first.
		void maybeTick()
		{
			const GameThreadTick tick = g_tick.load();
			if (!tick)
				return;
			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const std::int64_t last = g_lastTickQpc.load();
			if (now.QuadPart - last < freq.QuadPart / 250)  // at most every 4 ms
				return;
			g_lastTickQpc = now.QuadPart;
			const DWORD thread = GetCurrentThreadId();
			if (g_tickThread.exchange(thread) != thread)
				logf("input: game-thread tick running on thread %lu", thread);
			tick();
		}

		POINT             g_parked{};  // guarded by g_mutex
		bool              g_haveParked = false;

		BOOL WINAPI hookGetCursorPos(LPPOINT pt)
		{
			InFlight guard;
			++g_cntGetCursorPos;
			const BOOL ok = g_origGetCursorPos(pt);
			if (!ok || !pt || !g_routing)
				return ok;
			std::lock_guard lock(g_mutex);
			if (!g_haveParked) {
				g_parked = *pt;
				g_haveParked = true;
			}
			const LONG dx = pt->x - g_parked.x, dy = pt->y - g_parked.y;
			if (dx || dy) {
				g_pending.lookDx += float(dx);
				g_pending.lookDy += float(dy);
				g_origSetCursorPos(g_parked.x, g_parked.y);
			}
			*pt = g_parked;
			return ok;
		}

		BOOL WINAPI hookSetCursorPos(int x, int y)
		{
			InFlight guard;
			++g_cntSetCursorPos;
			if (g_routing) {
				std::lock_guard lock(g_mutex);
				g_parked = { x, y };
				g_haveParked = true;
			}
			return g_origSetCursorPos(x, y);
		}

		UINT WINAPI hookGetRawInputData(HRAWINPUT raw, UINT command, LPVOID data, PUINT size, UINT header)
		{
			InFlight guard;
			++g_cntRawInput;
			return g_origGetRawInputData(raw, command, data, size, header);
		}

		HRESULT STDMETHODCALLTYPE hookGetDeviceState(IDirectInputDevice8W* self, DWORD size, LPVOID data)
		{
			InFlight guard;
			++g_cntDiState;
			maybeTick();
			const HRESULT hr = g_origGetDeviceState(self, size, data);
			if (FAILED(hr) || !g_routing || !data)
				return hr;
			const BYTE type = deviceType(self);
			if (type == DI8DEVTYPE_KEYBOARD && size >= 256) {
				auto* keys = static_cast<BYTE*>(data);
				{
					std::lock_guard lock(g_mutex);
					for (int i = 0; i < 256; ++i)
						keyChanged(std::uint8_t(i), (keys[i] & 0x80) != 0);
				}
				const BYTE esc = keys[kDikEscape];
				std::memset(keys, 0, size);
				keys[kDikEscape] = esc;  // Esc still reaches Sekiro (its menu); the core hands control back
			} else if (type == DI8DEVTYPE_MOUSE && size >= sizeof(DIMOUSESTATE)) {
				auto* m = static_cast<DIMOUSESTATE2*>(data);
				const int buttons = size >= sizeof(DIMOUSESTATE2) ? 8 : 4;
				{
					std::lock_guard lock(g_mutex);
					g_pending.lookDx += float(m->lX);
					g_pending.lookDy += float(m->lY);
					if (m->lZ)
						push(proto::kInScroll, 0, m->lZ);
					for (int i = 0; i < buttons; ++i)
						buttonChanged(i, (m->rgbButtons[i] & 0x80) != 0);
				}
				std::memset(data, 0, size);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE hookGetDeviceData(IDirectInputDevice8W* self, DWORD objSize, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD flags)
		{
			InFlight guard;
			++g_cntDiData;
			const HRESULT hr = g_origGetDeviceData(self, objSize, rgdod, inOut, flags);
			if (FAILED(hr) || !g_routing || !inOut)
				return hr;
			if (!rgdod || (flags & DIGDD_PEEK)) {  // counting or peeking: Sekiro just sees nothing waiting
				*inOut = 0;
				return hr;
			}
			const BYTE type = deviceType(self);
			{
				std::lock_guard lock(g_mutex);
				for (DWORD i = 0; i < *inOut; ++i) {
					const auto& d = *reinterpret_cast<const DIDEVICEOBJECTDATA*>(reinterpret_cast<const BYTE*>(rgdod) + std::size_t(i) * objSize);
					if (type == DI8DEVTYPE_KEYBOARD) {
						keyChanged(std::uint8_t(d.dwOfs), (d.dwData & 0x80) != 0);
					} else if (type == DI8DEVTYPE_MOUSE) {
						if (d.dwOfs == DIMOFS_X)
							g_pending.lookDx += float(int(d.dwData));
						else if (d.dwOfs == DIMOFS_Y)
							g_pending.lookDy += float(int(d.dwData));
						else if (d.dwOfs == DIMOFS_Z)
							push(proto::kInScroll, 0, int(d.dwData));
						else if (d.dwOfs >= DIMOFS_BUTTON0 && d.dwOfs <= DIMOFS_BUTTON7)
							buttonChanged(int(d.dwOfs - DIMOFS_BUTTON0), (d.dwData & 0x80) != 0);
					}
				}
			}
			*inOut = 0;  // Sekiro gets no events while Minecraft has control
			return hr;
		}
	}

	bool install()
	{
		// Every DirectInput device shares one implementation, so a throwaway keyboard device of our
		// own gives us the vtable the game's devices use.
		IDirectInput8W* di = nullptr;
		if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr))) {
			logf("input: DirectInput8Create failed");
			return false;
		}
		IDirectInputDevice8W* dev = nullptr;
		if (FAILED(di->CreateDevice(GUID_SysKeyboard, &dev, nullptr))) {
			logf("input: CreateDevice failed");
			di->Release();
			return false;
		}
		void** vtbl = *reinterpret_cast<void***>(dev);
		g_targetGetDeviceState = vtbl[9];   // IDirectInputDevice8W::GetDeviceState
		g_targetGetDeviceData = vtbl[10];   // IDirectInputDevice8W::GetDeviceData
		dev->Release();
		di->Release();

		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
			logf("input: MH_Initialize failed (%d)", int(init));
			return false;
		}
		if (MH_CreateHook(g_targetGetDeviceState, reinterpret_cast<void*>(&hookGetDeviceState), reinterpret_cast<void**>(&g_origGetDeviceState)) != MH_OK ||
			MH_CreateHook(g_targetGetDeviceData, reinterpret_cast<void*>(&hookGetDeviceData), reinterpret_cast<void**>(&g_origGetDeviceData)) != MH_OK ||
			MH_EnableHook(g_targetGetDeviceState) != MH_OK || MH_EnableHook(g_targetGetDeviceData) != MH_OK) {
			logf("input: couldn't hook DirectInput");
			return false;
		}
		logf("input: DirectInput hooked (GetDeviceState %p, GetDeviceData %p)", g_targetGetDeviceState, g_targetGetDeviceData);

		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		g_targetGetCursorPos = reinterpret_cast<void*>(GetProcAddress(user32, "GetCursorPos"));
		g_targetSetCursorPos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
		g_targetGetRawInputData = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));
		if (MH_CreateHook(g_targetGetCursorPos, reinterpret_cast<void*>(&hookGetCursorPos), reinterpret_cast<void**>(&g_origGetCursorPos)) != MH_OK ||
			MH_CreateHook(g_targetSetCursorPos, reinterpret_cast<void*>(&hookSetCursorPos), reinterpret_cast<void**>(&g_origSetCursorPos)) != MH_OK ||
			MH_CreateHook(g_targetGetRawInputData, reinterpret_cast<void*>(&hookGetRawInputData), reinterpret_cast<void**>(&g_origGetRawInputData)) != MH_OK ||
			MH_EnableHook(g_targetGetCursorPos) != MH_OK || MH_EnableHook(g_targetSetCursorPos) != MH_OK || MH_EnableHook(g_targetGetRawInputData) != MH_OK) {
			logf("input: couldn't hook the cursor functions (mouse look won't work)");
		} else {
			logf("input: cursor functions hooked");
		}
		return true;
	}

	std::string callStats()
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf), "GetCursorPos %d, SetCursorPos %d, GetRawInputData %d, DI GetDeviceState %d, DI GetDeviceData %d",
			g_cntGetCursorPos.exchange(0), g_cntSetCursorPos.exchange(0), g_cntRawInput.exchange(0), g_cntDiState.exchange(0), g_cntDiData.exchange(0));
		return buf;
	}

	void uninstall()
	{
		g_routing = false;
		g_tick = nullptr;
		if (g_targetGetDeviceState)
			MH_DisableHook(g_targetGetDeviceState);
		if (g_targetGetDeviceData)
			MH_DisableHook(g_targetGetDeviceData);
		for (void* t : { g_targetGetCursorPos, g_targetSetCursorPos, g_targetGetRawInputData })
			if (t)
				MH_DisableHook(t);
		// The game may be inside a hook right now; wait for it to leave before the DLL goes away.
		for (int i = 0; i < 200 && g_inFlight > 0; ++i)
			Sleep(5);
		Sleep(20);
		MH_Uninitialize();
		g_targetGetDeviceState = g_targetGetDeviceData = nullptr;
		g_targetGetCursorPos = g_targetSetCursorPos = g_targetGetRawInputData = nullptr;
	}

	void setRouting(bool toMinecraft)
	{
		std::lock_guard lock(g_mutex);
		if (g_routing == toMinecraft)
			return;
		g_routing = toMinecraft;
		// Start from "nothing held" either way; Minecraft gets a release-all when control leaves it.
		g_keyDown.fill(false);
		g_buttonDown.fill(false);
		g_haveParked = false;
		g_pending = {};
		if (!toMinecraft)
			push(proto::kInReleaseAll, 0, 0);
	}

	bool routing() { return g_routing; }

	void setGameThreadTick(GameThreadTick tick)
	{
		g_tick = tick;
	}

	Pending take()
	{
		std::lock_guard lock(g_mutex);
		Pending out = std::move(g_pending);
		g_pending = {};
		return out;
	}
}
