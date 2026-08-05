#include "Config.h"
#include "Debugf.h"
#include <windows.h>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {
	using QWORD = unsigned long long;

#define MAKELONGLONG(lo, hi) ((LONGLONG(DWORD(lo) & 0xffffffff)) | LONGLONG(DWORD(hi) & 0xffffffff) << 32)
#define MAKEQWORD(LO2, HI2, LO1, HI1) MAKELONGLONG(MAKELONG(LO2, HI2), MAKELONG(LO1, HI1))
#define QV(V1, V2, V3, V4) MAKEQWORD(V4, V3, V2, V1)

	QWORD getModuleVersion(HMODULE hModule) {
		char waPath[MAX_PATH] = {};
		DWORD handle = 0;
		GetModuleFileNameA(hModule, waPath, MAX_PATH);
		DWORD size = GetFileVersionInfoSizeA(waPath, &handle);
		if (!size) {
			return 0;
		}

		void *buffer = std::malloc(size);
		if (!buffer) {
			return 0;
		}

		QWORD version = 0;
		if (GetFileVersionInfoA(waPath, handle, size, buffer)) {
			VS_FIXEDFILEINFO *info = nullptr;
			DWORD infoSize = 0;
			if (VerQueryValueA(buffer, "\\", reinterpret_cast<LPVOID *>(&info), reinterpret_cast<PUINT>(&infoSize))
				&& info
				&& info->dwSignature == 0xFEEF04BD) {
				version = MAKELONGLONG(info->dwFileVersionLS, info->dwFileVersionMS);
			}
		}

		std::free(buffer);
		return version;
	}
}

void Config::initialize() {
	char waBuff[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, waBuff, sizeof(waBuff));
	waDir = fs::path(waBuff).parent_path();
}

int Config::waVersionCheck() {
	const auto version = getModuleVersion(nullptr);
	char versionStr[64] = {};
	_snprintf_s(versionStr, _TRUNCATE, "Detected game version: %u.%u.%u.%u", PWORD(&version)[3], PWORD(&version)[2], PWORD(&version)[1], PWORD(&version)[0]);
	debugf("%s\n", versionStr);

	const std::string title = getFullStr();
	char buff[512] = {};
	if (version < QV(3, 8, 0, 0)) {
		_snprintf_s(buff, _TRUNCATE, "wkSetResCustom is not compatible with WA versions older than 3.8.0.0.\n\n%s", versionStr);
		MessageBoxA(nullptr, buff, title.c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}
	if (version >= QV(3, 9, 0, 0)) {
		_snprintf_s(buff, _TRUNCATE, "wkSetResCustom is not compatible with WA versions 3.9.x.x and newer.\n\n%s", versionStr);
		MessageBoxA(nullptr, buff, title.c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}
	return 1;
}

const std::filesystem::path &Config::getWaDir() {
	return waDir;
}

std::string Config::getVersionStr() {
	return "v0.1.0";
}

std::string Config::getBuildStr() {
	return __DATE__ " " __TIME__;
}

std::string Config::getModuleStr() {
	return moduleName;
}

std::string Config::getFullStr() {
	return getModuleStr() + " " + getVersionStr() + " (build: " + getBuildStr() + ")";
}
