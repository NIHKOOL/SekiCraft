// sekicraft.dll: the small part me3 loads into sekiro.exe. It hot-reloads sekicraft_core.dll.
//
// The core holds all the logic. The loader runs a shadow copy of it (so the build can overwrite
// the original while the game runs) and swaps in a fresh copy whenever the core is rebuilt:
// sekicraft_stop() on the old one, FreeLibrary, load the new copy, sekicraft_start().
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
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

	// ---- starting Minecraft ------------------------------------------------------------------
	// Sekiro starts Minecraft (once per game session; the loader isn't reloaded like the core).
	// What to run comes from sekicraft.ini next to this DLL:
	//   minecraft_command = the program (and arguments) to run
	//   minecraft_dir     = the folder to run it in
	//   minecraft_hidden  = 1 to start it without a console window (the dev build's Gradle)
	// Without an ini, one is written: the dev build when this DLL sits in the source tree
	// (sekiro/build next to fabric/), else the release's bundled Prism Launcher instance.

	std::wstring trim(std::wstring s)
	{
		const auto b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
		return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
	}

	struct LaunchConfig
	{
		std::wstring command, dir;
		bool         hidden = false;
	};

	LaunchConfig readLaunchConfig(const std::wstring& dllDir)
	{
		const std::wstring ini = dllDir + L"sekicraft.ini";
		if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
			// Write the default for where this DLL is.
			const std::wstring fabric = dllDir + L"..\\..\\fabric\\";
			const bool devTree = GetFileAttributesW((fabric + L"gradlew.bat").c_str()) != INVALID_FILE_ATTRIBUTES;
			std::wstring text = L"; SekiCraft: what Sekiro starts for the Minecraft side (once per game session).\r\n"
			                    L"; Leave minecraft_command empty to start Minecraft yourself.\r\n";
			if (devTree) {
				// Gradle needs a Java 25 JDK; JAVA_HOME may not be set system-wide.
				std::wstring javaHome;
				WIN32_FIND_DATAW fd{};
				if (HANDLE h = FindFirstFileW(L"C:\\Program Files\\Eclipse Adoptium\\jdk-25*", &fd); h != INVALID_HANDLE_VALUE) {
					javaHome = std::wstring(L"C:\\Program Files\\Eclipse Adoptium\\") + fd.cFileName;
					FindClose(h);
				}
				// SEKICRAFT_QUIT_WITH_SEKIRO: Minecraft saves and quits when this Sekiro does.
				text += L"minecraft_command = cmd.exe /c " + (javaHome.empty() ? std::wstring() : L"set \"JAVA_HOME=" + javaHome + L"\" && ") +
				        L"set SEKICRAFT_QUIT_WITH_SEKIRO=true&& .\\gradlew.bat runClient\r\nminecraft_dir = " + fabric + L"\r\nminecraft_hidden = 1\r\n";
			} else {
				text += L"minecraft_command = \"" + dllDir + L"Minecraft\\PrismLauncher\\prismlauncher.exe\" --launch SekiCraft\r\nminecraft_dir = " + dllDir +
				        L"Minecraft\\PrismLauncher\\\r\nminecraft_hidden = 0\r\n";
			}
			if (std::FILE* f = _wfsopen(ini.c_str(), L"w, ccs=UTF-8", _SH_DENYNO)) {
				std::fputws(text.c_str(), f);
				std::fclose(f);
			}
		}
		LaunchConfig cfg;
		if (std::FILE* f = _wfsopen(ini.c_str(), L"r, ccs=UTF-8", _SH_DENYNO)) {
			wchar_t line[2048];
			while (std::fgetws(line, 2048, f)) {
				std::wstring l = trim(line);
				if (l.empty() || l[0] == L';' || l[0] == L'#')
					continue;
				const auto eq = l.find(L'=');
				if (eq == std::wstring::npos)
					continue;
				const std::wstring key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
				if (key == L"minecraft_command")
					cfg.command = value;
				else if (key == L"minecraft_dir")
					cfg.dir = value;
				else if (key == L"minecraft_hidden")
					cfg.hidden = value == L"1";
			}
			std::fclose(f);
		}
		return cfg;
	}

	void startMinecraft(const std::wstring& dllDir)
	{
		// Already running? The Minecraft side holds this mutex for as long as it runs.
		if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\SekiCraft_v1_minecraft")) {
			CloseHandle(m);
			sekicraft::logf("loader: Minecraft is already running");
			return;
		}
		const LaunchConfig cfg = readLaunchConfig(dllDir);
		if (cfg.command.empty()) {
			sekicraft::logf("loader: no minecraft_command in sekicraft.ini; start Minecraft yourself");
			return;
		}
		STARTUPINFOW si{ sizeof(si) };
		PROCESS_INFORMATION pi{};
		std::wstring cmd = cfg.command;  // CreateProcessW may write to it
		const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP | (cfg.hidden ? CREATE_NO_WINDOW : 0), nullptr,
			cfg.dir.empty() ? nullptr : cfg.dir.c_str(), &si, &pi);
		if (!ok) {
			sekicraft::logf("loader: couldn't start Minecraft (Windows error %lu); check sekicraft.ini", GetLastError());
			return;
		}
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		sekicraft::logf("loader: started Minecraft (pid %lu); it links up once it's loaded", pi.dwProcessId);
	}

	DWORD WINAPI watcher(void*)
	{
		const std::wstring dir = moduleDir();
		startMinecraft(dir);
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
