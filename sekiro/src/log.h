// Tiny shared log: sekicraft.log next to the DLLs, readable while the game runs.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>

namespace sekicraft
{
	inline std::FILE*& logFile()
	{
		static std::FILE* f = nullptr;
		return f;
	}

	inline std::mutex& logMutex()
	{
		static std::mutex m;
		return m;
	}

	// truncate: start a fresh log (the loader does this once per game launch; the core appends).
	inline void openLog(const std::wstring& path, bool truncate)
	{
		// Always write in append mode: the loader and the core each hold their own handle, and
		// a "w" handle would write at its own offset over the other's lines.
		if (truncate) {
			if (std::FILE* f = _wfsopen(path.c_str(), L"w", _SH_DENYNO))
				std::fclose(f);
		}
		logFile() = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
	}

	inline void closeLog()
	{
		if (logFile())
			std::fclose(logFile());
		logFile() = nullptr;
	}

	inline void logf(const char* fmt, ...)
	{
		std::lock_guard lock(logMutex());
		std::FILE* f = logFile();
		if (!f)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(f, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		std::vfprintf(f, fmt, args);
		va_end(args);
		std::fputc('\n', f);
		std::fflush(f);
	}
}
