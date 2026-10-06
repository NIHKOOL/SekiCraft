// sekicraft.dll: the small part me3 loads into sekiro.exe. It hot-reloads sekicraft_core.dll.
//
// The core holds all the logic. The loader runs a shadow copy of it (so the build can overwrite
// the original while the game runs) and swaps in a fresh copy whenever the core is rebuilt:
// sekicraft_stop() on the old one, FreeLibrary, load the new copy, sekicraft_start().
#include "log.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace
{
	HMODULE           g_self = nullptr;
	std::atomic<bool> g_stop{ false };

	using StartFn = bool (*)(const wchar_t* dir);
	using StopFn = void (*)();

	std::wstring moduleDir()
	{
		wchar_t path[MAX_PATH];
		const DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
		std::wstring s(path, n);
		return s.substr(0, s.find_last_of(L"\\/") + 1);
	}

	bool lastWrite(const std::wstring& path, FILETIME& out)
	{
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
			return false;
		out = a.ftLastWriteTime;
		return true;
	}

	// True once the linker has finished with the file (we can open it without sharing writes).
	bool settled(const std::wstring& path)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		CloseHandle(h);
		return true;
	}

	struct Core
	{
		HMODULE      module = nullptr;
		StopFn       stop = nullptr;
		std::wstring copy;
	};

	void unload(Core& core)
	{
		if (!core.module)
			return;
		if (core.stop)
			core.stop();
		FreeLibrary(core.module);
		DeleteFileW(core.copy.c_str());
		core = {};
	}

	bool load(Core& core, const std::wstring& dir, int generation)
	{
		const std::wstring src = dir + L"sekicraft_core.dll";
		core.copy = dir + L"sekicraft_core.loaded" + std::to_wstring(generation) + L".dll";
		if (!CopyFileW(src.c_str(), core.copy.c_str(), FALSE)) {
			sekicraft::logf("loader: couldn't copy the core (Windows error %lu)", GetLastError());
			return false;
		}
		core.module = LoadLibraryW(core.copy.c_str());
		if (!core.module) {
			sekicraft::logf("loader: LoadLibrary failed (Windows error %lu)", GetLastError());
			DeleteFileW(core.copy.c_str());
			return false;
		}
		auto start = reinterpret_cast<StartFn>(GetProcAddress(core.module, "sekicraft_start"));
		core.stop = reinterpret_cast<StopFn>(GetProcAddress(core.module, "sekicraft_stop"));
		if (!start || !core.stop || !start(dir.c_str())) {
			sekicraft::logf("loader: core didn't start");
			core.stop = nullptr;
			unload(core);
			return false;
		}
		sekicraft::logf("loader: core generation %d running", generation);
		return true;
	}

	DWORD WINAPI watcher(void*)
	{
		const std::wstring dir = moduleDir();
		const std::wstring src = dir + L"sekicraft_core.dll";
		Core core;
		FILETIME loaded{};
		int generation = 0;
		while (!g_stop) {
			FILETIME now;
			if (lastWrite(src, now) && CompareFileTime(&now, &loaded) != 0 && settled(src)) {
				Sleep(300);  // let the build finish touching it
				if (lastWrite(src, now) && settled(src)) {
					if (core.module)
						sekicraft::logf("loader: core rebuilt; reloading");
					unload(core);
					load(core, dir, ++generation);
					loaded = now;
				}
			}
			Sleep(500);
		}
		unload(core);
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = module;
		DisableThreadLibraryCalls(module);
		sekicraft::openLog(moduleDir() + L"sekicraft.log", true);
		sekicraft::logf("loader: SekiCraft loaded into pid %lu", GetCurrentProcessId());
		if (HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr))
			CloseHandle(t);
	} else if (reason == DLL_PROCESS_DETACH) {
		g_stop = true;
	}
	return TRUE;
}
