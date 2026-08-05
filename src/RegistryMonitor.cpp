#include "RegistryMonitor.h"
#include "Diagnostics.h"
#include "Hooks.h"
#include "ResolutionManager.h"
#include <intrin.h>
#include <windows.h>
#include <cstring>
#include <string>

namespace {
	LSTATUS(WINAPI *origRegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD) = nullptr;
	LSTATUS(WINAPI *origRegSetValueExW)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE *, DWORD) = nullptr;

	constexpr char DisplayWidthValueName[] = "DisplayXSize";
	constexpr char DisplayHeightValueName[] = "DisplayYSize";

	bool isTrackedValueNameA(LPCSTR valueName) {
		if (valueName == nullptr) {
			return false;
		}
		return std::strcmp(valueName, DisplayWidthValueName) == 0
			|| std::strcmp(valueName, DisplayHeightValueName) == 0;
	}

	bool isTrackedValueNameW(LPCWSTR valueName) {
		if (valueName == nullptr) {
			return false;
		}
		return std::wcscmp(valueName, L"DisplayXSize") == 0
			|| std::wcscmp(valueName, L"DisplayYSize") == 0;
	}

	std::string narrowFromWide(LPCWSTR text) {
		if (text == nullptr) {
			return {};
		}

		int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (size <= 1) {
			return {};
		}

		std::string result(static_cast<size_t>(size - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
		return result;
	}

	std::string describeCaller(const void *returnAddress) {
		if (returnAddress == nullptr) {
			return "unknown";
		}

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(returnAddress, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr) {
			return "unknown";
		}

		char modulePath[MAX_PATH] = {};
		if (GetModuleFileNameA(static_cast<HMODULE>(mbi.AllocationBase), modulePath, sizeof(modulePath)) == 0) {
			return "unknown";
		}

		const char *fileName = std::strrchr(modulePath, '\\');
		fileName = fileName != nullptr ? fileName + 1 : modulePath;
		DWORD base = static_cast<DWORD>(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
		DWORD caller = static_cast<DWORD>(reinterpret_cast<uintptr_t>(returnAddress));

		char buffer[256] = {};
		std::snprintf(buffer, sizeof(buffer), "%s+0x%X", fileName, caller - base);
		return buffer;
	}

	bool isCallerInMainModule(const void *returnAddress) {
		if (returnAddress == nullptr) {
			return false;
		}

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(returnAddress, &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr) {
			return false;
		}

		return mbi.AllocationBase == GetModuleHandleA(nullptr);
	}

	bool tryOverrideTrackedWriteA(LPCSTR valueName, DWORD type, const BYTE *data, DWORD dataSize, const void *returnAddress, DWORD *overrideValue) {
		if (!isTrackedValueNameA(valueName) || type != REG_DWORD || data == nullptr || dataSize < sizeof(DWORD) || overrideValue == nullptr) {
			return false;
		}
		if (!isCallerInMainModule(returnAddress)) {
			return false;
		}

		DisplayResolution requested{};
		if (!ResolutionManager::tryGetRequestedResolution(&requested)) {
			return false;
		}

		if (std::strcmp(valueName, DisplayWidthValueName) == 0) {
			*overrideValue = requested.width;
			return true;
		}
		if (std::strcmp(valueName, DisplayHeightValueName) == 0) {
			*overrideValue = requested.height;
			return true;
		}
		return false;
	}

	bool tryOverrideTrackedWriteW(LPCWSTR valueName, DWORD type, const BYTE *data, DWORD dataSize, const void *returnAddress, DWORD *overrideValue) {
		if (!isTrackedValueNameW(valueName) || type != REG_DWORD || data == nullptr || dataSize < sizeof(DWORD) || overrideValue == nullptr) {
			return false;
		}
		if (!isCallerInMainModule(returnAddress)) {
			return false;
		}

		DisplayResolution requested{};
		if (!ResolutionManager::tryGetRequestedResolution(&requested)) {
			return false;
		}

		if (std::wcscmp(valueName, L"DisplayXSize") == 0) {
			*overrideValue = requested.width;
			return true;
		}
		if (std::wcscmp(valueName, L"DisplayYSize") == 0) {
			*overrideValue = requested.height;
			return true;
		}
		return false;
	}

	void logTrackedWriteA(LPCSTR valueName, DWORD type, const BYTE *data, DWORD dataSize, const void *returnAddress) {
		if (!isTrackedValueNameA(valueName)) {
			return;
		}

		if (type == REG_DWORD && data != nullptr && dataSize >= sizeof(DWORD)) {
			DWORD value = *reinterpret_cast<const DWORD *>(data);
			Diagnostics::log(
				"Registry write A: %s=%lu caller=%s",
				valueName,
				value,
				describeCaller(returnAddress).c_str());
			return;
		}

		Diagnostics::log(
			"Registry write A: %s type=%lu size=%lu caller=%s",
			valueName,
			type,
			dataSize,
			describeCaller(returnAddress).c_str());
	}

	void logTrackedWriteW(LPCWSTR valueName, DWORD type, const BYTE *data, DWORD dataSize, const void *returnAddress) {
		if (!isTrackedValueNameW(valueName)) {
			return;
		}

		std::string narrowName = narrowFromWide(valueName);
		if (type == REG_DWORD && data != nullptr && dataSize >= sizeof(DWORD)) {
			DWORD value = *reinterpret_cast<const DWORD *>(data);
			Diagnostics::log(
				"Registry write W: %s=%lu caller=%s",
				narrowName.c_str(),
				value,
				describeCaller(returnAddress).c_str());
			return;
		}

		Diagnostics::log(
			"Registry write W: %s type=%lu size=%lu caller=%s",
			narrowName.c_str(),
			type,
			dataSize,
			describeCaller(returnAddress).c_str());
	}

	LSTATUS WINAPI hookRegSetValueExA(HKEY key, LPCSTR valueName, DWORD reserved, DWORD type, const BYTE *data, DWORD dataSize) {
		const void *returnAddress = _ReturnAddress();
		const BYTE *effectiveData = data;
		DWORD effectiveDataSize = dataSize;
		DWORD overrideValue = 0;
		if (tryOverrideTrackedWriteA(valueName, type, data, dataSize, returnAddress, &overrideValue)) {
			DWORD originalValue = *reinterpret_cast<const DWORD *>(data);
			effectiveData = reinterpret_cast<const BYTE *>(&overrideValue);
			effectiveDataSize = sizeof(overrideValue);
			Diagnostics::log(
				"Registry override A: %s %lu -> %lu caller=%s",
				valueName,
				originalValue,
				overrideValue,
				describeCaller(returnAddress).c_str());
		}

		logTrackedWriteA(valueName, type, effectiveData, effectiveDataSize, returnAddress);
		return origRegSetValueExA(key, valueName, reserved, type, effectiveData, effectiveDataSize);
	}

	LSTATUS WINAPI hookRegSetValueExW(HKEY key, LPCWSTR valueName, DWORD reserved, DWORD type, const BYTE *data, DWORD dataSize) {
		const void *returnAddress = _ReturnAddress();
		const BYTE *effectiveData = data;
		DWORD effectiveDataSize = dataSize;
		DWORD overrideValue = 0;
		if (tryOverrideTrackedWriteW(valueName, type, data, dataSize, returnAddress, &overrideValue)) {
			DWORD originalValue = *reinterpret_cast<const DWORD *>(data);
			effectiveData = reinterpret_cast<const BYTE *>(&overrideValue);
			effectiveDataSize = sizeof(overrideValue);
			Diagnostics::log(
				"Registry override W: %s %lu -> %lu caller=%s",
				narrowFromWide(valueName).c_str(),
				originalValue,
				overrideValue,
				describeCaller(returnAddress).c_str());
		}

		logTrackedWriteW(valueName, type, effectiveData, effectiveDataSize, returnAddress);
		return origRegSetValueExW(key, valueName, reserved, type, effectiveData, effectiveDataSize);
	}
}

void RegistryMonitor::install() {
	HMODULE advapi = GetModuleHandleA("advapi32.dll");
	if (advapi == nullptr) {
		advapi = LoadLibraryA("advapi32.dll");
	}
	if (advapi == nullptr) {
		Diagnostics::log("RegistryMonitor install skipped: advapi32.dll not available");
		return;
	}

	DWORD addrRegSetValueExA = static_cast<DWORD>(reinterpret_cast<uintptr_t>(GetProcAddress(advapi, "RegSetValueExA")));
	DWORD addrRegSetValueExW = static_cast<DWORD>(reinterpret_cast<uintptr_t>(GetProcAddress(advapi, "RegSetValueExW")));
	if (addrRegSetValueExA != 0) {
		Hooks::hook(
			"RegSetValueExA",
			addrRegSetValueExA,
			(DWORD *)&hookRegSetValueExA,
			(DWORD *)&origRegSetValueExA,
			__CALLPOSITION__);
	}
	if (addrRegSetValueExW != 0) {
		Hooks::hook(
			"RegSetValueExW",
			addrRegSetValueExW,
			(DWORD *)&hookRegSetValueExW,
			(DWORD *)&origRegSetValueExW,
			__CALLPOSITION__);
	}

	Diagnostics::log(
		"registry monitor installed: RegSetValueExA=0x%X RegSetValueExW=0x%X",
		addrRegSetValueExA,
		addrRegSetValueExW);
}
