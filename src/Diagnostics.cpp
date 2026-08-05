#include "Diagnostics.h"
#include <windows.h>
#include <cstdio>

namespace {
	std::filesystem::path logPath;
	CRITICAL_SECTION logLock;
	bool logLockInitialized = false;
}

void Diagnostics::initialize(const std::filesystem::path &waDir) {
	if (!logLockInitialized) {
		InitializeCriticalSection(&logLock);
		logLockInitialized = true;
	}

	logPath = waDir / "wkSetResCustom.log";
	FILE *file = nullptr;
	if (fopen_s(&file, logPath.string().c_str(), "a") == 0 && file) {
		SYSTEMTIME localTime{};
		GetLocalTime(&localTime);
		fprintf(
			file,
			"\n==== wkSetResCustom session started %04u-%02u-%02u %02u:%02u:%02u pid=%lu ====\n",
			localTime.wYear,
			localTime.wMonth,
			localTime.wDay,
			localTime.wHour,
			localTime.wMinute,
			localTime.wSecond,
			GetCurrentProcessId());
		fclose(file);
	}
}

void Diagnostics::log(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vlog(fmt, args);
	va_end(args);
}

void Diagnostics::vlog(const char *fmt, va_list args) {
	if (logPath.empty()) {
		return;
	}

	if (logLockInitialized) {
		EnterCriticalSection(&logLock);
	}

	FILE *file = nullptr;
	if (fopen_s(&file, logPath.string().c_str(), "a") == 0 && file) {
		fprintf(file, "[%lu] ", GetTickCount());
		vfprintf(file, fmt, args);
		fprintf(file, "\n");
		fclose(file);
	}

	if (logLockInitialized) {
		LeaveCriticalSection(&logLock);
	}
}
