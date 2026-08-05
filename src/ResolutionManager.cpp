#include "ResolutionManager.h"
#include "Diagnostics.h"
#include "Frontend.h"
#include "Hooks.h"
#include "W2App.h"
#include <algorithm>
#include <ddraw.h>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
	constexpr size_t DirectDraw2SetDisplayModeIndex = 0x54 / sizeof(void *);
	constexpr DWORD MinimumWidth = 640;
	constexpr DWORD MinimumHeight = 480;
	constexpr GUID IID_IDirectDraw2Local = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
	constexpr DWORD WaDisplayLegacyDirectDrawOffset = 0x48;
	constexpr char WaOptionsRegistryPath[] = "Software\\Team17SoftwareLTD\\WormsArmageddon\\Options";
	constexpr char WaDisplayWidthValue[] = "DisplayXSize";
	constexpr char WaDisplayHeightValue[] = "DisplayYSize";
	constexpr char WaWindowedModeValue[] = "WindowedMode";
	constexpr char WaFrontendUseDesktopWindowValue[] = "FrontendUseDesktopWindow";
	constexpr DWORD WkSuperFrontendReloadConfigOffset = 0x49B2;
	constexpr DWORD WkSuperFrontendTargetWidthOffset = 0x2697C;
	constexpr DWORD WkSuperFrontendDesktopWidthOffset = 0x2698C;
	constexpr DWORD WkSuperFrontendDesktopHeightOffset = 0x26B6C;
	constexpr DWORD WkSuperFrontendTargetHeightOffset = 0x26B80;
	constexpr DWORD WkSuperFrontendWidthScaleOffset = 0x26968;
	constexpr DWORD WkSuperFrontendHeightScaleOffset = 0x26938;
	constexpr DWORD WkSuperFrontendEffectiveScaleOffset = 0x26940;
	constexpr DWORD WkSuperFrontendRuntimeScaleOffset = 0x26AC8;
	constexpr DWORD WaMainObjectGlobalRva = 0x2B3908;
	constexpr DWORD WaMainObjectVtableRva = 0x262D80;
	constexpr DWORD WaStaticOptionsRva = 0x48E388;
	constexpr DWORD WaOptionsStructOffset = 0x0CDFB8;
	constexpr DWORD WaOptionsWidthOffset = 0x30;
	constexpr DWORD WaOptionsHeightOffset = 0x34;
	constexpr DWORD WaOptionsWindowedModeOffset = 0xA4;
	constexpr DWORD WaOptionsDirtyOffset = 0xE0;
	constexpr double BaseWidth = 640.0;
	constexpr double BaseHeight = 480.0;
	constexpr double BaseAspectRatio = 4.0 / 3.0;

	using DirectDrawCreateFn = HRESULT(WINAPI *)(GUID *, LPDIRECTDRAW *, IUnknown *);
	using DirectDrawCreateExFn = HRESULT(WINAPI *)(GUID *, LPVOID *, REFIID, IUnknown *);
	using LegacySetDisplayModeFn = HRESULT(WINAPI *)(LPDIRECTDRAW, DWORD, DWORD, DWORD);
	using SetDisplayModeFn = HRESULT(WINAPI *)(LPDIRECTDRAW2, DWORD, DWORD, DWORD, DWORD, DWORD);
	using ReloadSuperFrontendConfigFn = BOOL(__cdecl *)();

	DirectDrawCreateFn origDirectDrawCreate = nullptr;
	DirectDrawCreateExFn origDirectDrawCreateEx = nullptr;
	LegacySetDisplayModeFn origLegacySetDisplayMode = nullptr;
	SetDisplayModeFn origSetDisplayMode = nullptr;
	DWORD addrSetScreenPalette = 0;
	DWORD addrCommitScreenPalette = 0;
	DWORD addrScreenPalette = 0;

	HRESULT WINAPI hookLegacySetDisplayMode(LPDIRECTDRAW self, DWORD dwWidth, DWORD dwHeight, DWORD dwBpp);
	HRESULT WINAPI hookSetDisplayMode(LPDIRECTDRAW2 self, DWORD dwWidth, DWORD dwHeight, DWORD dwBpp, DWORD dwRefreshRate, DWORD dwFlags);
	LPDIRECTDRAW2 queryDirectDraw2(IUnknown *unknown);
	void installLegacySetDisplayModeHook(LPDIRECTDRAW dd);
	void installSetDisplayModeHook(LPDIRECTDRAW2 dd2);
	void installGlobalSetDisplayModeHook();
	HRESULT callLegacySetDisplayMode(LPDIRECTDRAW self, DWORD width, DWORD height, DWORD bpp);
	std::string formatHRESULT(HRESULT hr);
	std::optional<std::string> getCurrentMonitorDeviceName();
	std::vector<DisplayResolution> enumerateMonitorResolutions(const std::optional<std::string> &deviceName);
	std::optional<DisplayResolution> queryCurrentMonitorResolution();
	bool writeConfiguredResolution(const DisplayResolution &resolution, std::string *errorMessage);
	std::optional<DisplayResolution> readConfiguredResolution();

	std::mutex stateMutex;
	LPDIRECTDRAW2 directDraw2 = nullptr;
	bool directDraw2IsFallback = false;
	std::optional<DisplayResolution> requestedResolution;
	std::optional<DisplayResolution> lastAppliedResolution;
	std::optional<DisplayResolution> pseudoFullscreenResolution;
	std::optional<DisplayResolution> originalConfiguredResolution;
	std::optional<DWORD> originalWindowedModeValue;
	std::optional<DWORD> originalFrontendUseDesktopWindowValue;
	bool pseudoFullscreenPending = false;
	bool pseudoFullscreenActive = false;
	bool windowedModeForcedByModule = false;
	bool frontendDesktopWindowForcedByModule = false;
	LONG originalGameWindowStyle = 0;
	LONG originalGameWindowExStyle = 0;
	bool originalGameWindowStyleCaptured = false;
	bool legacySetDisplayModeHookInstalled = false;
	bool setDisplayModeHookInstalled = false;
	DWORD legacySetDisplayModeHookAddress = 0;
	DWORD setDisplayModeHookAddress = 0;
	bool globalSetDisplayModeHookAttempted = false;
	bool paletteRefreshSupportAttempted = false;

	bool readDwordSafely(DWORD address, DWORD *value) {
		if (address == 0 || value == nullptr) {
			return false;
		}

		__try {
			*value = *reinterpret_cast<DWORD *>(address);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool readWordSafely(DWORD address, WORD *value) {
		if (address == 0 || value == nullptr) {
			return false;
		}

		__try {
			*value = *reinterpret_cast<WORD *>(address);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool writeDwordSafely(DWORD address, DWORD value) {
		if (address == 0) {
			return false;
		}

		__try {
			DWORD oldProtect = 0;
			if (VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(DWORD), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				*reinterpret_cast<DWORD *>(address) = value;
				DWORD ignored = 0;
				VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(DWORD), oldProtect, &ignored);
				return true;
			}

			*reinterpret_cast<DWORD *>(address) = value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool writeByteSafely(DWORD address, BYTE value) {
		if (address == 0) {
			return false;
		}

		__try {
			DWORD oldProtect = 0;
			if (VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(BYTE), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				*reinterpret_cast<BYTE *>(address) = value;
				DWORD ignored = 0;
				VirtualProtect(reinterpret_cast<LPVOID>(address), sizeof(BYTE), oldProtect, &ignored);
				return true;
			}

			*reinterpret_cast<BYTE *>(address) = value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool writeWordSafely(DWORD address, WORD value) {
		if (address == 0) {
			return false;
		}

		__try {
			*reinterpret_cast<WORD *>(address) = value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool writeDoubleSafely(DWORD address, double value) {
		if (address == 0) {
			return false;
		}

		__try {
			*reinterpret_cast<double *>(address) = value;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool isPointerInsideModule(DWORD address, const char *expectedModuleName) {
		if (address == 0 || expectedModuleName == nullptr) {
			return false;
		}

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0 || mbi.AllocationBase == nullptr) {
			return false;
		}

		char modulePath[MAX_PATH] = {};
		if (GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), modulePath, sizeof(modulePath)) == 0) {
			return false;
		}

		char *fileName = std::strrchr(modulePath, '\\');
		fileName = fileName != nullptr ? fileName + 1 : modulePath;
		return _stricmp(fileName, expectedModuleName) == 0;
	}

	bool looksLikeDirectDrawObject(DWORD objectAddress) {
		DWORD vtable = 0;
		if (!readDwordSafely(objectAddress, &vtable) || !isPointerInsideModule(vtable, "ddraw.dll")) {
			return false;
		}

		DWORD setDisplayModeAddress = 0;
		DWORD slotAddress = vtable + static_cast<DWORD>(DirectDraw2SetDisplayModeIndex * sizeof(void *));
		return readDwordSafely(slotAddress, &setDisplayModeAddress) && isPointerInsideModule(setDisplayModeAddress, "ddraw.dll");
	}

	BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
		auto *result = reinterpret_cast<HWND *>(lParam);
		if (*result != nullptr) {
			return FALSE;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid != GetCurrentProcessId()) {
			return TRUE;
		}

		if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
			return TRUE;
		}

		*result = hwnd;
		return FALSE;
	}

	HWND findGameWindow() {
		HWND foreground = GetForegroundWindow();
		if (foreground != nullptr) {
			DWORD pid = 0;
			GetWindowThreadProcessId(foreground, &pid);
			if (pid == GetCurrentProcessId()) {
				HWND root = GetAncestor(foreground, GA_ROOT);
				return root != nullptr ? root : foreground;
			}
		}

		HWND result = nullptr;
		EnumWindows(&enumWindowsProc, reinterpret_cast<LPARAM>(&result));
		return result;
	}

	bool isUnsafeFallbackRefreshScreen(int screen) {
		switch (screen) {
			case 32:
			case 39:
				return true;
			default:
				return false;
		}
	}

	bool isUnsafeSuperFrontendLiveRefreshScreen(int screen) {
		switch (screen) {
			case 1706:
				return true;
			default:
				return false;
		}
	}

	bool requestFrontendRefresh(DWORD frontendContext, std::string *errorMessage) {
		std::string frontendError;
		DWORD currentFrontendContext = Frontend::getCurrentContext();
		int currentScreen = Frontend::getCurrentScreen();

		if (frontendContext != 0 && frontendContext != currentFrontendContext) {
			if (isUnsafeFallbackRefreshScreen(currentScreen)) {
				Diagnostics::log(
					"Skipping direct lobby-context frontend refresh on unsafe screen=%d lobby=0x%X storedContext=0x%X",
					currentScreen,
					frontendContext,
					currentFrontendContext);
			} else {
				Diagnostics::log(
					"Trying frontend refresh with lobby context=0x%X screen=%d storedContext=0x%X",
					frontendContext,
					currentScreen,
					currentFrontendContext);
				if (Frontend::refreshScreen(frontendContext, currentScreen, &frontendError)) {
					return true;
				}

				if (!frontendError.empty()) {
					Diagnostics::log("Lobby-context frontend refresh failed: %s", frontendError.c_str());
				}
			}
		}

		if (currentFrontendContext != 0) {
			if (frontendContext != 0 && frontendContext != currentFrontendContext) {
				Diagnostics::log(
					"Ignoring mismatched lobby refresh context: lobby=0x%X stored=0x%X screen=%d",
					frontendContext,
					currentFrontendContext,
					currentScreen);
			}

			Diagnostics::log(
				"Trying frontend refresh with stored context=0x%X screen=%d lobbyContext=0x%X",
				currentFrontendContext,
				currentScreen,
				frontendContext);
			if (Frontend::refreshScreen(currentFrontendContext, currentScreen, &frontendError)) {
				return true;
			}

			if (!frontendError.empty()) {
				Diagnostics::log("Stored-context frontend refresh failed: %s", frontendError.c_str());
			}
		}

		if (currentFrontendContext == 0 && frontendContext != 0) {
			if (isUnsafeFallbackRefreshScreen(currentScreen)) {
				frontendError = "frontend refresh fallback was skipped on this WA screen to avoid a black screen.";
				Diagnostics::log(
					"Skipping fallback lobby-context frontend refresh: lobby=0x%X screen=%d storedContext=0x%X",
					frontendContext,
					currentScreen,
					currentFrontendContext);
			} else {
				Diagnostics::log(
					"Trying frontend refresh with fallback lobby context=0x%X screen=%d",
					frontendContext,
					currentScreen);
				if (Frontend::refreshScreen(frontendContext, currentScreen, &frontendError)) {
					return true;
				}

				if (!frontendError.empty()) {
					Diagnostics::log("Fallback lobby-context frontend refresh failed: %s", frontendError.c_str());
				}
			}
		}

		if (errorMessage != nullptr) {
			*errorMessage = frontendError.empty() ? "frontend refresh is not available yet." : frontendError;
		}
		return false;
	}

	bool ensurePaletteRefreshSupport(std::string *errorMessage) {
		if (paletteRefreshSupportAttempted) {
			if (addrSetScreenPalette == 0 || addrCommitScreenPalette == 0 || addrScreenPalette == 0) {
				if (errorMessage != nullptr) {
					*errorMessage = "WA palette refresh support is not available.";
				}
				return false;
			}
			return true;
		}

		paletteRefreshSupportAttempted = true;
		try {
			addrSetScreenPalette = _ScanPattern(
				"SetScreenPalette",
				"\x53\x8B\x5C\x24\x08\x55\xBA\x00\x00\x00\x00\x8B\xC7\xB9\x00\x00\x00\x00\x56\x8B\x31\x3B\x30\x75\x12\x83\xEA\x04\x83\xC0\x04\x83\xC1\x04\x83\xFA\x04\x73\xEC",
				"??????x????xxx????xxxxxxxxxxxxxxxxxxxxx");
			addrCommitScreenPalette = _ScanPattern(
				"CommitScreenPalette",
				"\x83\xEC\x34\x83\x3D\x00\x00\x00\x00\x00\x0F\x84\x00\x00\x00\x00\x53\x55\x56\x57\xBF\x00\x00\x00\x00\x89\x7C\x24\x34",
				"xxxxx????xxx????xxxxx????xxxx");
		} catch (const std::exception &ex) {
			Diagnostics::log("Palette refresh support discovery failed: %s", ex.what());
			if (errorMessage != nullptr) {
				*errorMessage = "failed to locate WA palette refresh routines.";
			}
			return false;
		}

		if (!readDwordSafely(addrSetScreenPalette + 0xE, &addrScreenPalette) || addrScreenPalette == 0) {
			Diagnostics::log("Palette refresh support discovery failed: screen palette address is not readable");
			if (errorMessage != nullptr) {
				*errorMessage = "failed to locate the WA screen palette buffer.";
			}
			return false;
		}

		Diagnostics::log(
			"Palette refresh support ready: set=0x%X commit=0x%X screenPalette=0x%X",
			addrSetScreenPalette,
			addrCommitScreenPalette,
			addrScreenPalette);
		return true;
	}

	bool callSetScreenPaletteSafely(DWORD *paletteState) {
		if (paletteState == nullptr) {
			return false;
		}

		DWORD savedEdi = 0;
		__try {
			_asm mov savedEdi, edi
			_asm mov edi, addrScreenPalette
			_asm mov eax, paletteState
			_asm push eax
			_asm call addrSetScreenPalette
			_asm mov edi, savedEdi
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			_asm mov edi, savedEdi
			return false;
		}
	}

	bool callCommitScreenPaletteSafely() {
		__try {
			_asm call addrCommitScreenPalette
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool refreshScreenPalette(std::string *errorMessage) {
		if (W2App::getAddrDdDisplay() == 0) {
			if (errorMessage != nullptr) {
				*errorMessage = "DD_Display is not ready yet.";
			}
			return false;
		}

		std::string discoveryError;
		if (!ensurePaletteRefreshSupport(&discoveryError)) {
			if (errorMessage != nullptr) {
				*errorMessage = discoveryError;
			}
			return false;
		}

		DWORD paletteState = 0;
		if (!callSetScreenPaletteSafely(&paletteState)) {
			Diagnostics::log("Screen palette refresh failed while preparing the palette buffer");
			if (errorMessage != nullptr) {
				*errorMessage = "WA palette prepare step raised an exception.";
			}
			return false;
		}

		if (!callCommitScreenPaletteSafely()) {
			Diagnostics::log("Screen palette refresh failed while committing the palette");
			if (errorMessage != nullptr) {
				*errorMessage = "WA palette commit step raised an exception.";
			}
			return false;
		}

		Diagnostics::log("WA screen palette refreshed: palette=0x%X state=0x%X", addrScreenPalette, paletteState);
		return true;
	}

	void refreshScreenPaletteAfterLiveChange(const char *reason) {
		std::string paletteError;
		if (refreshScreenPalette(&paletteError)) {
			Diagnostics::log("Screen palette refresh completed after %s", reason);
		} else if (!paletteError.empty()) {
			Diagnostics::log("Screen palette refresh skipped after %s: %s", reason, paletteError.c_str());
		}
	}

	bool writeOptionDwordValue(const char *valueName, DWORD value, std::string *errorMessage) {
		if (valueName == nullptr || *valueName == '\0') {
			if (errorMessage != nullptr) {
				*errorMessage = "WA option name is missing.";
			}
			return false;
		}

		HKEY key = nullptr;
		LSTATUS status = RegCreateKeyExA(
			HKEY_CURRENT_USER,
			WaOptionsRegistryPath,
			0,
			nullptr,
			0,
			KEY_SET_VALUE,
			nullptr,
			&key,
			nullptr);
		if (status != ERROR_SUCCESS || key == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "Failed to open the WA options registry key.";
			}
			return false;
		}

		status = RegSetValueExA(
			key,
			valueName,
			0,
			REG_DWORD,
			reinterpret_cast<const BYTE *>(&value),
			sizeof(value));
		RegCloseKey(key);
		if (status != ERROR_SUCCESS) {
			if (errorMessage != nullptr) {
				*errorMessage = std::string("Failed to store ") + valueName + " in the WA options.";
			}
			return false;
		}

		Diagnostics::log("Stored WA option in registry: %s=%lu", valueName, value);
		return true;
	}

	std::optional<DWORD> readOptionDwordValue(const char *valueName) {
		if (valueName == nullptr || *valueName == '\0') {
			return std::nullopt;
		}

		HKEY key = nullptr;
		LSTATUS status = RegOpenKeyExA(HKEY_CURRENT_USER, WaOptionsRegistryPath, 0, KEY_QUERY_VALUE, &key);
		if (status != ERROR_SUCCESS || key == nullptr) {
			return std::nullopt;
		}

		DWORD value = 0;
		DWORD valueSize = sizeof(value);
		DWORD valueType = 0;
		status = RegQueryValueExA(key, valueName, nullptr, &valueType, reinterpret_cast<BYTE *>(&value), &valueSize);
		RegCloseKey(key);
		if (status != ERROR_SUCCESS || valueType != REG_DWORD || valueSize < sizeof(value)) {
			return std::nullopt;
		}

		return value;
	}

	bool isResolutionListedForCurrentMonitor(const DisplayResolution &resolution) {
		const auto deviceName = getCurrentMonitorDeviceName();
		auto resolutions = enumerateMonitorResolutions(deviceName);
		if (resolutions.empty()) {
			resolutions = enumerateMonitorResolutions(std::nullopt);
		}

		return std::find(resolutions.begin(), resolutions.end(), resolution) != resolutions.end();
	}

	void clearPseudoFullscreenState(bool keepWindowedModeOverride) {
		std::lock_guard<std::mutex> lock(stateMutex);
		pseudoFullscreenResolution.reset();
		pseudoFullscreenPending = false;
		pseudoFullscreenActive = false;
		if (!keepWindowedModeOverride) {
			originalConfiguredResolution.reset();
			originalWindowedModeValue.reset();
			windowedModeForcedByModule = false;
			originalFrontendUseDesktopWindowValue.reset();
			frontendDesktopWindowForcedByModule = false;
			originalGameWindowStyleCaptured = false;
		}
	}

	bool isPseudoFullscreenPendingFor(const DisplayResolution &resolution) {
		std::lock_guard<std::mutex> lock(stateMutex);
		return pseudoFullscreenPending && pseudoFullscreenResolution.has_value() && *pseudoFullscreenResolution == resolution;
	}

	bool isPseudoFullscreenManagedFor(const DisplayResolution &resolution) {
		std::lock_guard<std::mutex> lock(stateMutex);
		return windowedModeForcedByModule && pseudoFullscreenResolution.has_value() && *pseudoFullscreenResolution == resolution;
	}

	void markPseudoFullscreenPending(const DisplayResolution &resolution) {
		std::lock_guard<std::mutex> lock(stateMutex);
		pseudoFullscreenResolution = resolution;
		pseudoFullscreenPending = true;
		pseudoFullscreenActive = false;
		windowedModeForcedByModule = true;
	}

	void markPseudoFullscreenApplied() {
		std::lock_guard<std::mutex> lock(stateMutex);
		if (pseudoFullscreenResolution.has_value()) {
			pseudoFullscreenPending = false;
			pseudoFullscreenActive = true;
		}
	}

	bool isPseudoFullscreenForcedByModule() {
		std::lock_guard<std::mutex> lock(stateMutex);
		return windowedModeForcedByModule;
	}

	bool isFrontendDesktopWindowForcedByModule() {
		std::lock_guard<std::mutex> lock(stateMutex);
		return frontendDesktopWindowForcedByModule;
	}

	bool shouldOverrideSetDisplayModeRequest(DWORD width, DWORD height, DWORD bpp, DisplayResolution *resolution) {
		if (width == 0 || height == 0 || resolution == nullptr || (bpp != 0 && bpp != 8)) {
			return false;
		}

		std::lock_guard<std::mutex> lock(stateMutex);
		if (!requestedResolution.has_value()) {
			return false;
		}

		// Unsupported custom resolutions must stay on the windowed refresh path.
		if (windowedModeForcedByModule && pseudoFullscreenResolution.has_value() && *pseudoFullscreenResolution == *requestedResolution) {
			return false;
		}

		*resolution = *requestedResolution;
		return true;
	}

	void captureGameWindowStyleForPseudoFullscreen(HWND hwnd) {
		if (hwnd == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(stateMutex);
		if (originalGameWindowStyleCaptured) {
			return;
		}

		originalGameWindowStyle = GetWindowLongA(hwnd, GWL_STYLE);
		originalGameWindowExStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
		originalGameWindowStyleCaptured = true;
		Diagnostics::log(
			"Captured game window styles for pseudo-fullscreen restore: style=0x%X exStyle=0x%X",
			static_cast<unsigned int>(originalGameWindowStyle),
			static_cast<unsigned int>(originalGameWindowExStyle));
	}

	bool restoreGameWindowStyleAfterPseudoFullscreen(std::string *errorMessage) {
		LONG style = 0;
		LONG exStyle = 0;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!originalGameWindowStyleCaptured) {
				return true;
			}

			style = originalGameWindowStyle;
			exStyle = originalGameWindowExStyle;
		}

		HWND hwnd = findGameWindow();
		if (hwnd == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "game window not found while restoring the original layout.";
			}
			return false;
		}

		SetWindowLongA(hwnd, GWL_STYLE, style);
		SetWindowLongA(hwnd, GWL_EXSTYLE, exStyle);
		if (!SetWindowPos(
				hwnd,
				nullptr,
				0,
				0,
				0,
				0,
				SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER)) {
			if (errorMessage != nullptr) {
				*errorMessage = "SetWindowPos failed while restoring the original layout.";
			}
			return false;
		}

		Diagnostics::log(
			"Restored game window styles after pseudo-fullscreen: style=0x%X exStyle=0x%X",
			static_cast<unsigned int>(style),
			static_cast<unsigned int>(exStyle));
		return true;
	}

	bool disablePseudoFullscreenMode(std::string *errorMessage) {
		DWORD restoreWindowedModeValue = 0;
		DWORD restoreFrontendUseDesktopWindowValue = 0;
		bool shouldRestoreFrontendUseDesktopWindow = false;
		bool recoveringPersistedPseudoFullscreenState = false;
		if (!isPseudoFullscreenForcedByModule()) {
			const auto configuredResolution = readConfiguredResolution();
			const auto currentWindowedMode = readOptionDwordValue(WaWindowedModeValue);
			const auto currentFrontendUseDesktopWindow = readOptionDwordValue(WaFrontendUseDesktopWindowValue);
			recoveringPersistedPseudoFullscreenState =
				configuredResolution.has_value() &&
				!isResolutionListedForCurrentMonitor(*configuredResolution) &&
				(currentWindowedMode.value_or(0) != 0 || currentFrontendUseDesktopWindow.value_or(0) != 0);
			if (!recoveringPersistedPseudoFullscreenState) {
				clearPseudoFullscreenState(false);
				return true;
			}

			restoreWindowedModeValue = 0;
			restoreFrontendUseDesktopWindowValue = 0;
			shouldRestoreFrontendUseDesktopWindow = currentFrontendUseDesktopWindow.value_or(0) != 0;
			Diagnostics::log(
				"Recovering persisted pseudo-fullscreen state before applying a supported resolution: configured=%s currentWindowed=%lu currentFrontendUseDesktopWindow=%lu",
				ResolutionManager::formatResolution(*configuredResolution).c_str(),
				currentWindowedMode.value_or(0),
				currentFrontendUseDesktopWindow.value_or(0));
		} else {
			std::lock_guard<std::mutex> lock(stateMutex);
			restoreWindowedModeValue = originalWindowedModeValue.value_or(0);
			restoreFrontendUseDesktopWindowValue = originalFrontendUseDesktopWindowValue.value_or(0);
			shouldRestoreFrontendUseDesktopWindow = frontendDesktopWindowForcedByModule;
		}

		std::string writeError;
		if (!writeOptionDwordValue(WaWindowedModeValue, restoreWindowedModeValue, &writeError)) {
			if (errorMessage != nullptr) {
				*errorMessage = writeError;
			}
			return false;
		}

		if (shouldRestoreFrontendUseDesktopWindow &&
			!writeOptionDwordValue(WaFrontendUseDesktopWindowValue, restoreFrontendUseDesktopWindowValue, &writeError)) {
			if (errorMessage != nullptr) {
				*errorMessage = writeError;
			}
			return false;
		}

		std::string restoreLayoutError;
		if (!restoreGameWindowStyleAfterPseudoFullscreen(&restoreLayoutError) && !restoreLayoutError.empty()) {
			Diagnostics::log("Pseudo-fullscreen style restore skipped: %s", restoreLayoutError.c_str());
		}

		Diagnostics::log(
			recoveringPersistedPseudoFullscreenState
				? "Pseudo-fullscreen cleanup after restart: restoring WindowedMode=%lu FrontendUseDesktopWindow=%lu"
				: "Pseudo-fullscreen disabled: restoring WindowedMode=%lu FrontendUseDesktopWindow=%lu",
			restoreWindowedModeValue,
			restoreFrontendUseDesktopWindowValue);
		clearPseudoFullscreenState(false);
		return true;
	}

	bool preparePseudoFullscreenMode(const DisplayResolution &resolution, std::string *errorMessage) {
		auto currentWindowedMode = readOptionDwordValue(WaWindowedModeValue);
		auto currentFrontendUseDesktopWindow = readOptionDwordValue(WaFrontendUseDesktopWindowValue);
		bool shouldForceFrontendDesktopWindow = currentWindowedMode.value_or(0) == 0;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (!windowedModeForcedByModule) {
				originalConfiguredResolution = readConfiguredResolution();
				originalWindowedModeValue = currentWindowedMode.value_or(0);
				originalFrontendUseDesktopWindowValue = currentFrontendUseDesktopWindow.value_or(0);
			}
			frontendDesktopWindowForcedByModule = shouldForceFrontendDesktopWindow;
		}

		std::string writeError;
		if (!writeOptionDwordValue(WaWindowedModeValue, 1, &writeError)) {
			if (errorMessage != nullptr) {
				*errorMessage = writeError;
			}
			return false;
		}

		if (shouldForceFrontendDesktopWindow &&
			!writeOptionDwordValue(WaFrontendUseDesktopWindowValue, 1, &writeError)) {
			if (errorMessage != nullptr) {
				*errorMessage = writeError;
			}
			return false;
		}

		Diagnostics::log(
			"Pseudo-fullscreen prepared for unsupported custom resolution %lux%lu (previous WindowedMode=%lu previous FrontendUseDesktopWindow=%lu forceDesktopWindow=%s)",
			resolution.width,
			resolution.height,
			currentWindowedMode.value_or(0),
			currentFrontendUseDesktopWindow.value_or(0),
			shouldForceFrontendDesktopWindow ? "true" : "false");
		markPseudoFullscreenPending(resolution);
		return true;
	}

	std::string formatDisplayChangeResult(LONG result) {
		switch (result) {
			case DISP_CHANGE_SUCCESSFUL:
				return "DISP_CHANGE_SUCCESSFUL";
			case DISP_CHANGE_BADDUALVIEW:
				return "DISP_CHANGE_BADDUALVIEW";
			case DISP_CHANGE_BADFLAGS:
				return "DISP_CHANGE_BADFLAGS";
			case DISP_CHANGE_BADMODE:
				return "DISP_CHANGE_BADMODE";
			case DISP_CHANGE_BADPARAM:
				return "DISP_CHANGE_BADPARAM";
			case DISP_CHANGE_FAILED:
				return "DISP_CHANGE_FAILED";
			case DISP_CHANGE_NOTUPDATED:
				return "DISP_CHANGE_NOTUPDATED";
			case DISP_CHANGE_RESTART:
				return "DISP_CHANGE_RESTART";
			default:
				return std::to_string(result);
		}
	}

	bool tryReadMonitorRegistryMode(const char *deviceName, DEVMODEA *mode) {
		if (mode == nullptr) {
			return false;
		}

		std::memset(mode, 0, sizeof(*mode));
		mode->dmSize = sizeof(*mode);
		return EnumDisplaySettingsExA(deviceName, ENUM_REGISTRY_SETTINGS, mode, 0) != FALSE;
	}

	bool restoreMonitorDesktopDisplayMode(const char *deviceName, std::string *errorMessage) {
		DEVMODEA desktopMode{};
		bool haveDesktopMode = tryReadMonitorRegistryMode(deviceName, &desktopMode);

		LONG result = ChangeDisplaySettingsExA(deviceName, nullptr, nullptr, 0, nullptr);
		Diagnostics::log(
			"Restoring desktop display mode for pseudo-fullscreen: device=%s registry=%s result=%s",
			deviceName != nullptr ? deviceName : "<default>",
			haveDesktopMode ? ResolutionManager::formatResolution(DisplayResolution{desktopMode.dmPelsWidth, desktopMode.dmPelsHeight}).c_str() : "<unknown>",
			formatDisplayChangeResult(result).c_str());

		if (result != DISP_CHANGE_SUCCESSFUL) {
			if (errorMessage != nullptr) {
				*errorMessage = "failed to restore the desktop display mode (" + formatDisplayChangeResult(result) + ").";
			}
			return false;
		}

		return true;
	}

	bool applyPseudoFullscreenWindowLayout(std::string *errorMessage) {
		const bool desktopWindowModeManagedByModule = isPseudoFullscreenForcedByModule();
		if (desktopWindowModeManagedByModule) {
			std::string restoreModeError;
			const auto deviceName = getCurrentMonitorDeviceName();
			if (!restoreMonitorDesktopDisplayMode(
					deviceName && !deviceName->empty() ? deviceName->c_str() : nullptr,
					&restoreModeError) &&
				!restoreModeError.empty()) {
				Diagnostics::log("Desktop display mode restore skipped before WA desktop-window apply: %s", restoreModeError.c_str());
			}

			Diagnostics::log(
				"Pseudo-fullscreen live apply is relying on WA windowed/desktop-window mode (desktopWindowForced=%s)",
				isFrontendDesktopWindowForcedByModule() ? "true" : "false");
			markPseudoFullscreenApplied();
			return true;
		}

		HWND hwnd = findGameWindow();
		if (hwnd == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "game window not found for pseudo-fullscreen.";
			}
			return false;
		}

		captureGameWindowStyleForPseudoFullscreen(hwnd);

		HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFOEXA monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (!GetMonitorInfoA(monitor, &monitorInfo)) {
			if (errorMessage != nullptr) {
				*errorMessage = "failed to query the target monitor for pseudo-fullscreen.";
			}
			return false;
		}

		std::string restoreModeError;
		if (!restoreMonitorDesktopDisplayMode(monitorInfo.szDevice, &restoreModeError) && !restoreModeError.empty()) {
			Diagnostics::log("Desktop display mode restore skipped before pseudo-fullscreen layout: %s", restoreModeError.c_str());
		}

		std::memset(&monitorInfo, 0, sizeof(monitorInfo));
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (!GetMonitorInfoA(monitor, &monitorInfo)) {
			if (errorMessage != nullptr) {
				*errorMessage = "failed to query the target monitor for pseudo-fullscreen.";
			}
			return false;
		}

		DEVMODEA desktopMode{};
		bool haveDesktopMode = tryReadMonitorRegistryMode(monitorInfo.szDevice, &desktopMode);

		LONG style = GetWindowLongA(hwnd, GWL_STYLE);
		LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
		LONG newStyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZE | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME)) | WS_POPUP;
		LONG newExStyle = exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
		SetWindowLongA(hwnd, GWL_STYLE, newStyle);
		SetWindowLongA(hwnd, GWL_EXSTYLE, newExStyle);

		RECT rect = monitorInfo.rcMonitor;
		int width = rect.right - rect.left;
		int height = rect.bottom - rect.top;
		if (haveDesktopMode && desktopMode.dmPelsWidth >= MinimumWidth && desktopMode.dmPelsHeight >= MinimumHeight) {
			width = static_cast<int>(desktopMode.dmPelsWidth);
			height = static_cast<int>(desktopMode.dmPelsHeight);
			rect.right = rect.left + width;
			rect.bottom = rect.top + height;
		}
		Diagnostics::log(
			"Applying pseudo-fullscreen layout: hwnd=0x%X device=%s monitorRect=(%ld,%ld)-(%ld,%ld) outer=%dx%d oldStyle=0x%X newStyle=0x%X",
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(hwnd)),
			monitorInfo.szDevice,
			rect.left,
			rect.top,
			rect.right,
			rect.bottom,
			width,
			height,
			static_cast<unsigned int>(style),
			static_cast<unsigned int>(newStyle));

		if (!SetWindowPos(
				hwnd,
				HWND_TOP,
				rect.left,
				rect.top,
				width,
				height,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER)) {
			if (errorMessage != nullptr) {
				*errorMessage = "SetWindowPos failed while applying pseudo-fullscreen.";
			}
			return false;
		}

		markPseudoFullscreenApplied();
		return true;
	}

	bool resizeGameWindowClientArea(const DisplayResolution &resolution, std::string *errorMessage) {
		HWND hwnd = findGameWindow();
		if (hwnd == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "game window not found.";
			}
			return false;
		}

		LONG style = GetWindowLongA(hwnd, GWL_STYLE);
		LONG exStyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
		BOOL hasMenu = GetMenu(hwnd) != nullptr ? TRUE : FALSE;

		RECT targetRect{};
		targetRect.left = 0;
		targetRect.top = 0;
		targetRect.right = static_cast<LONG>(resolution.width);
		targetRect.bottom = static_cast<LONG>(resolution.height);
		if (!AdjustWindowRectEx(&targetRect, static_cast<DWORD>(style), hasMenu, static_cast<DWORD>(exStyle))) {
			if (errorMessage != nullptr) {
				*errorMessage = "AdjustWindowRectEx failed for the game window.";
			}
			return false;
		}

		RECT rect{};
		if (!GetWindowRect(hwnd, &rect)) {
			if (errorMessage != nullptr) {
				*errorMessage = "failed to query the game window.";
			}
			return false;
		}

		int width = targetRect.right - targetRect.left;
		int height = targetRect.bottom - targetRect.top;
		int left = rect.left;
		int top = rect.top;

		HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (GetMonitorInfoA(monitor, &monitorInfo)) {
			left = monitorInfo.rcWork.left + ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - width) / 2;
			top = monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - height) / 2;
		}

		Diagnostics::log(
			"Resizing game window: hwnd=0x%X targetClient=%lux%lu outer=%dx%d pos=%d,%d oldRect=(%ld,%ld)-(%ld,%ld)",
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(hwnd)),
			resolution.width,
			resolution.height,
			width,
			height,
			left,
			top,
			rect.left,
			rect.top,
			rect.right,
			rect.bottom);

		if (!MoveWindow(hwnd, left, top, width, height, TRUE)) {
			if (errorMessage != nullptr) {
				*errorMessage = "MoveWindow failed while resizing the game window.";
			}
			return false;
		}

		return true;
	}

	DWORD getMainModuleBase() {
		HMODULE mainModule = GetModuleHandleA(nullptr);
		return mainModule != nullptr ? static_cast<DWORD>(reinterpret_cast<uintptr_t>(mainModule)) : 0;
	}

	DWORD getMainModuleAddress(DWORD rva) {
		DWORD base = getMainModuleBase();
		return base != 0 ? base + rva : 0;
	}

	bool isPlausibleResolutionValue(DWORD width, DWORD height) {
		return width >= MinimumWidth && height >= MinimumHeight && width <= 10000 && height <= 10000;
	}

	bool tryReadWaOptionsResolutionAt(DWORD optionsAddress, DisplayResolution *resolution) {
		if (optionsAddress == 0 || resolution == nullptr) {
			return false;
		}

		DWORD width = 0;
		DWORD height = 0;
		if (!readDwordSafely(optionsAddress + WaOptionsWidthOffset, &width) ||
			!readDwordSafely(optionsAddress + WaOptionsHeightOffset, &height) ||
			!isPlausibleResolutionValue(width, height)) {
			return false;
		}

		resolution->width = width;
		resolution->height = height;
		return true;
	}

	bool tryReadWaOptionsWindowedModeAt(DWORD optionsAddress, DWORD *windowedMode) {
		if (optionsAddress == 0 || windowedMode == nullptr) {
			return false;
		}

		DWORD value = 0;
		if (!readDwordSafely(optionsAddress + WaOptionsWindowedModeOffset, &value)) {
			return false;
		}

		*windowedMode = value;
		return true;
	}

	bool tryGetWaStaticOptionsAddress(DWORD *optionsAddress) {
		if (optionsAddress == nullptr) {
			return false;
		}

		DWORD address = getMainModuleAddress(WaStaticOptionsRva);
		if (address == 0) {
			return false;
		}

		DisplayResolution resolution{};
		if (!tryReadWaOptionsResolutionAt(address, &resolution)) {
			return false;
		}

		*optionsAddress = address;
		return true;
	}

	bool tryReadWaStaticOptionsResolution(DisplayResolution *resolution) {
		if (resolution == nullptr) {
			return false;
		}

		DWORD optionsAddress = 0;
		if (!tryGetWaStaticOptionsAddress(&optionsAddress)) {
			return false;
		}

		return tryReadWaOptionsResolutionAt(optionsAddress, resolution);
	}

	bool applyResolutionToWaStaticOptions(const DisplayResolution &resolution, DWORD desiredWindowedMode, std::string *errorMessage) {
		DWORD optionsAddress = 0;
		if (!tryGetWaStaticOptionsAddress(&optionsAddress)) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA static options block is not available.";
			}
			return false;
		}

		DisplayResolution currentResolution{};
		tryReadWaOptionsResolutionAt(optionsAddress, &currentResolution);
		DWORD currentWindowedMode = 0;
		bool currentWindowedModeOk = tryReadWaOptionsWindowedModeAt(optionsAddress, &currentWindowedMode);

		bool wroteWidth = writeDwordSafely(optionsAddress + WaOptionsWidthOffset, resolution.width);
		bool wroteHeight = writeDwordSafely(optionsAddress + WaOptionsHeightOffset, resolution.height);
		bool wroteWindowedMode = writeDwordSafely(optionsAddress + WaOptionsWindowedModeOffset, desiredWindowedMode);
		bool wroteDirtyFlag = writeByteSafely(optionsAddress + WaOptionsDirtyOffset, 1);
		if (!wroteWidth || !wroteHeight || !wroteWindowedMode) {
			Diagnostics::log(
				"WA static options write failed: options=0x%X current=%lux%lu currentWindowed=%s target=%lux%lu targetWindowed=%lu wroteWidth=%s wroteHeight=%s wroteWindowed=%s wroteDirty=%s",
				optionsAddress,
				currentResolution.width,
				currentResolution.height,
				currentWindowedModeOk ? std::to_string(currentWindowedMode).c_str() : "?",
				resolution.width,
				resolution.height,
				desiredWindowedMode,
				wroteWidth ? "true" : "false",
				wroteHeight ? "true" : "false",
				wroteWindowedMode ? "true" : "false",
				wroteDirtyFlag ? "true" : "false");
			if (errorMessage != nullptr) {
				*errorMessage = "failed to update WA static options state.";
			}
			return false;
		}

		DisplayResolution readBackResolution{};
		bool readBackOk = tryReadWaOptionsResolutionAt(optionsAddress, &readBackResolution);
		DWORD readBackWindowedMode = 0;
		bool readBackWindowedModeOk = tryReadWaOptionsWindowedModeAt(optionsAddress, &readBackWindowedMode);
		Diagnostics::log(
			"WA static options updated: options=0x%X old=%s oldWindowed=%s new=%lux%lu newWindowed=%lu readBack=%s readBackWindowed=%s dirty=%s",
			optionsAddress,
			ResolutionManager::formatResolution(currentResolution).c_str(),
			currentWindowedModeOk ? std::to_string(currentWindowedMode).c_str() : "?",
			resolution.width,
			resolution.height,
			desiredWindowedMode,
			readBackOk ? ResolutionManager::formatResolution(readBackResolution).c_str() : "?",
			readBackWindowedModeOk ? std::to_string(readBackWindowedMode).c_str() : "?",
			wroteDirtyFlag ? "true" : "false");
		return true;
	}

	bool tryResolveWaLiveOptionsFromObject(
		DWORD mainObject,
		DWORD *optionsAddress,
		DWORD *objectAddress,
		DWORD *vtableAddress,
		DisplayResolution *currentResolution,
		std::string *errorMessage) {
		if (mainObject == 0) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA main frontend object is not ready yet.";
			}
			return false;
		}

		DWORD vtable = 0;
		if (!readDwordSafely(mainObject, &vtable) || vtable == 0) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA main frontend object vtable is not readable.";
			}
			return false;
		}

		DWORD expectedVtable = getMainModuleAddress(WaMainObjectVtableRva);
		if (expectedVtable != 0 && vtable != expectedVtable) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA main frontend object did not match the expected type.";
			}
			return false;
		}

		DWORD resolvedOptionsAddress = mainObject + WaOptionsStructOffset;
		DisplayResolution resolvedResolution{};
		if (!tryReadWaOptionsResolutionAt(resolvedOptionsAddress, &resolvedResolution)) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA live options block is not readable.";
			}
			return false;
		}

		if (optionsAddress != nullptr) {
			*optionsAddress = resolvedOptionsAddress;
		}
		if (objectAddress != nullptr) {
			*objectAddress = mainObject;
		}
		if (vtableAddress != nullptr) {
			*vtableAddress = vtable;
		}
		if (currentResolution != nullptr) {
			*currentResolution = resolvedResolution;
		}
		return true;
	}

	bool tryFindWaLiveOptionsByScanningModule(
		DWORD *optionsAddress,
		DWORD *objectAddress,
		DWORD *vtableAddress,
		DisplayResolution *currentResolution) {
		DWORD base = getMainModuleBase();
		if (base == 0) {
			return false;
		}

		auto *dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
			return false;
		}

		auto *ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
			return false;
		}

		DWORD expectedVtable = getMainModuleAddress(WaMainObjectVtableRva);
		if (expectedVtable == 0) {
			return false;
		}

		DWORD end = base + ntHeaders->OptionalHeader.SizeOfImage;
		for (DWORD slot = base; slot + sizeof(DWORD) <= end; slot += sizeof(DWORD)) {
			DWORD candidateObject = 0;
			if (!readDwordSafely(slot, &candidateObject) || candidateObject == 0) {
				continue;
			}

			DWORD candidateVtable = 0;
			if (!readDwordSafely(candidateObject, &candidateVtable) || candidateVtable != expectedVtable) {
				continue;
			}

			if (tryResolveWaLiveOptionsFromObject(
					candidateObject,
					optionsAddress,
					objectAddress,
					vtableAddress,
					currentResolution,
					nullptr)) {
				Diagnostics::log(
					"Resolved WA live options via module scan: slot=0x%X object=0x%X options=0x%X current=%lux%lu",
					slot,
					objectAddress != nullptr ? *objectAddress : candidateObject,
					optionsAddress != nullptr ? *optionsAddress : (candidateObject + WaOptionsStructOffset),
					currentResolution != nullptr ? currentResolution->width : 0,
					currentResolution != nullptr ? currentResolution->height : 0);
				return true;
			}
		}

		return false;
	}

	bool tryGetWaLiveOptionsAddress(DWORD *optionsAddress, DWORD *objectAddress, DWORD *vtableAddress, DisplayResolution *currentResolution, std::string *errorMessage) {
		if (optionsAddress == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA live options output pointer is missing.";
			}
			return false;
		}

		DWORD mainObjectGlobalAddress = getMainModuleAddress(WaMainObjectGlobalRva);
		if (mainObjectGlobalAddress == 0) {
			if (errorMessage != nullptr) {
				*errorMessage = "WA main module base is not available.";
			}
			return false;
		}

		DWORD mainObject = 0;
		if (readDwordSafely(mainObjectGlobalAddress, &mainObject) &&
			tryResolveWaLiveOptionsFromObject(
				mainObject,
				optionsAddress,
				objectAddress,
				vtableAddress,
				currentResolution,
				nullptr)) {
			return true;
		}

		if (tryFindWaLiveOptionsByScanningModule(optionsAddress, objectAddress, vtableAddress, currentResolution)) {
			return true;
		}

		if (errorMessage != nullptr) {
			*errorMessage = "WA live options object was not found.";
		}
		return false;
	}

	bool tryReadLiveWaOptionsResolution(DisplayResolution *resolution) {
		if (resolution == nullptr) {
			return false;
		}

		DWORD optionsAddress = 0;
		DisplayResolution liveResolution{};
		if (!tryGetWaLiveOptionsAddress(&optionsAddress, nullptr, nullptr, &liveResolution, nullptr)) {
			return false;
		}
		*resolution = liveResolution;
		return true;
	}

	bool applyResolutionToWaLiveOptions(const DisplayResolution &resolution, DWORD desiredWindowedMode, std::string *errorMessage) {
		DWORD optionsAddress = 0;
		DWORD objectAddress = 0;
		DWORD vtableAddress = 0;
		DisplayResolution currentResolution{};
		std::string localError;
		if (!tryGetWaLiveOptionsAddress(&optionsAddress, &objectAddress, &vtableAddress, &currentResolution, &localError)) {
			if (errorMessage != nullptr) {
				*errorMessage = localError;
			}
			return false;
		}

		DWORD currentWindowedMode = 0;
		bool currentWindowedModeOk = tryReadWaOptionsWindowedModeAt(optionsAddress, &currentWindowedMode);
		bool wroteWidth = writeDwordSafely(optionsAddress + WaOptionsWidthOffset, resolution.width);
		bool wroteHeight = writeDwordSafely(optionsAddress + WaOptionsHeightOffset, resolution.height);
		bool wroteWindowedMode = writeDwordSafely(optionsAddress + WaOptionsWindowedModeOffset, desiredWindowedMode);
		if (!wroteWidth || !wroteHeight || !wroteWindowedMode) {
			Diagnostics::log(
				"WA live options write failed: object=0x%X vtable=0x%X options=0x%X current=%lux%lu currentWindowed=%s target=%lux%lu targetWindowed=%lu wroteWidth=%s wroteHeight=%s wroteWindowed=%s",
				objectAddress,
				vtableAddress,
				optionsAddress,
				currentResolution.width,
				currentResolution.height,
				currentWindowedModeOk ? std::to_string(currentWindowedMode).c_str() : "?",
				resolution.width,
				resolution.height,
				desiredWindowedMode,
				wroteWidth ? "true" : "false",
				wroteHeight ? "true" : "false",
				wroteWindowedMode ? "true" : "false");
			if (errorMessage != nullptr) {
				*errorMessage = "failed to update WA live options state.";
			}
			return false;
		}

		DWORD confirmedWidth = 0;
		DWORD confirmedHeight = 0;
		DWORD confirmedWindowedMode = 0;
		bool readBackWidth = readDwordSafely(optionsAddress + WaOptionsWidthOffset, &confirmedWidth);
		bool readBackHeight = readDwordSafely(optionsAddress + WaOptionsHeightOffset, &confirmedHeight);
		bool readBackWindowedMode = readDwordSafely(optionsAddress + WaOptionsWindowedModeOffset, &confirmedWindowedMode);
		std::string readBackResolutionText = (readBackWidth ? std::to_string(confirmedWidth) : std::string("?")) +
			"x" +
			(readBackHeight ? std::to_string(confirmedHeight) : std::string("?"));

		Diagnostics::log(
			"WA live options updated: object=0x%X vtable=0x%X options=0x%X old=%s oldWindowed=%s new=%lux%lu newWindowed=%lu readBack=%s readBackWindowed=%s",
			objectAddress,
			vtableAddress,
			optionsAddress,
			ResolutionManager::formatResolution(currentResolution).c_str(),
			currentWindowedModeOk ? std::to_string(currentWindowedMode).c_str() : "?",
			resolution.width,
			resolution.height,
			desiredWindowedMode,
			readBackResolutionText.c_str(),
			readBackWindowedMode ? std::to_string(confirmedWindowedMode).c_str() : "?");
		return true;
	}

	bool pokeGameWindowThroughSuperFrontendHook(std::string *errorMessage) {
		HWND hwnd = findGameWindow();
		if (hwnd == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "game window not found.";
			}
			return false;
		}

		Diagnostics::log(
			"Poking MoveWindow hook with frontend baseline call: hwnd=0x%X",
			static_cast<unsigned int>(reinterpret_cast<uintptr_t>(hwnd)));

		if (!MoveWindow(hwnd, 0, 0, static_cast<int>(MinimumWidth), static_cast<int>(MinimumHeight), TRUE)) {
			if (errorMessage != nullptr) {
				*errorMessage = "baseline MoveWindow poke failed.";
			}
			return false;
		}

		return true;
	}

	bool tryReadSuperFrontendResolution(DWORD moduleBase, DWORD widthOffset, DWORD heightOffset, DisplayResolution *resolution) {
		if (moduleBase == 0 || resolution == nullptr) {
			return false;
		}

		WORD width = 0;
		WORD height = 0;
		if (!readWordSafely(moduleBase + widthOffset, &width) || !readWordSafely(moduleBase + heightOffset, &height)) {
			return false;
		}

		resolution->width = width;
		resolution->height = height;
		return true;
	}

	bool callReloadSuperFrontendConfigSafely(ReloadSuperFrontendConfigFn reloadConfig, bool *reloadSucceeded) {
		if (reloadConfig == nullptr || reloadSucceeded == nullptr) {
			return false;
		}

		BOOL reloadResult = FALSE;
		__try {
			reloadResult = reloadConfig();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}

		*reloadSucceeded = reloadResult != FALSE;
		return true;
	}

	bool applyResolutionToSuperFrontendState(DWORD moduleBase, const DisplayResolution &resolution, DisplayResolution *effectiveResolution, std::string *errorMessage) {
		if (moduleBase == 0 || effectiveResolution == nullptr) {
			return false;
		}

		DisplayResolution desktopResolution{};
		if (!tryReadSuperFrontendResolution(
				moduleBase,
				WkSuperFrontendDesktopWidthOffset,
				WkSuperFrontendDesktopHeightOffset,
				&desktopResolution)) {
			desktopResolution.width = static_cast<DWORD>(GetSystemMetrics(SM_CXSCREEN));
			desktopResolution.height = static_cast<DWORD>(GetSystemMetrics(SM_CYSCREEN));
		}

		DisplayResolution clampedResolution = resolution;
		if (desktopResolution.width >= MinimumWidth && desktopResolution.height >= MinimumHeight &&
			(clampedResolution.width > desktopResolution.width || clampedResolution.height > desktopResolution.height)) {
			clampedResolution = desktopResolution;
		}

		double widthScale = static_cast<double>(clampedResolution.width) / BaseWidth;
		double heightScale = static_cast<double>(clampedResolution.height) / BaseHeight;
		double effectiveScale = (static_cast<double>(clampedResolution.width) / static_cast<double>(clampedResolution.height) > BaseAspectRatio)
			? heightScale
			: widthScale;

		bool ok = true;
		ok = writeWordSafely(moduleBase + WkSuperFrontendTargetWidthOffset, static_cast<WORD>(clampedResolution.width)) && ok;
		ok = writeWordSafely(moduleBase + WkSuperFrontendTargetHeightOffset, static_cast<WORD>(clampedResolution.height)) && ok;
		ok = writeDoubleSafely(moduleBase + WkSuperFrontendWidthScaleOffset, widthScale) && ok;
		ok = writeDoubleSafely(moduleBase + WkSuperFrontendHeightScaleOffset, heightScale) && ok;
		ok = writeDoubleSafely(moduleBase + WkSuperFrontendEffectiveScaleOffset, effectiveScale) && ok;
		ok = writeDoubleSafely(moduleBase + WkSuperFrontendRuntimeScaleOffset, effectiveScale) && ok;
		if (!ok) {
			if (errorMessage != nullptr) {
				*errorMessage = "failed to update wkSuperFrontend live state.";
			}
			return false;
		}

		*effectiveResolution = clampedResolution;
		Diagnostics::log(
			"wkSuperFrontend live state updated: requested=%lux%lu applied=%lux%lu desktop=%lux%lu scales=(%.3f, %.3f, %.3f)",
			resolution.width,
			resolution.height,
			clampedResolution.width,
			clampedResolution.height,
			desktopResolution.width,
			desktopResolution.height,
			widthScale,
			heightScale,
			effectiveScale);
		return true;
	}

	bool applyResolutionViaSuperFrontend(const DisplayResolution &resolution, DWORD frontendContext, std::string *errorMessage) {
		HMODULE superFrontendModule = GetModuleHandleA("wkSuperFrontend.dll");
		if (superFrontendModule == nullptr) {
			if (errorMessage != nullptr) {
				*errorMessage = "wkSuperFrontend.dll is not loaded.";
			}
			return false;
		}

		DWORD moduleBase = static_cast<DWORD>(reinterpret_cast<uintptr_t>(superFrontendModule));
		Diagnostics::log("wkSuperFrontend module found at 0x%X", moduleBase);

		int currentScreen = Frontend::getCurrentScreen();
		if (isUnsafeSuperFrontendLiveRefreshScreen(currentScreen)) {
			Diagnostics::log(
				"Skipping wkSuperFrontend live refresh on unsafe screen=%d lobbyContext=0x%X",
				currentScreen,
				frontendContext);
			if (errorMessage != nullptr) {
				*errorMessage = "wkSuperFrontend live refresh is disabled in guest lobbies to avoid disconnects.";
			}
			return false;
		}

		auto reloadConfig = reinterpret_cast<ReloadSuperFrontendConfigFn>(moduleBase + WkSuperFrontendReloadConfigOffset);
		bool reloadSucceeded = false;
		if (callReloadSuperFrontendConfigSafely(reloadConfig, &reloadSucceeded)) {
			Diagnostics::log("wkSuperFrontend config reload result: %s", reloadSucceeded ? "true" : "false");
		} else {
			Diagnostics::log("wkSuperFrontend config reload raised an exception");
		}

		DisplayResolution effectiveResolution{};
		if (!applyResolutionToSuperFrontendState(moduleBase, resolution, &effectiveResolution, errorMessage)) {
			return false;
		}

		std::string frontendError;
		bool refreshed = requestFrontendRefresh(frontendContext, &frontendError);
		if (refreshed) {
			Diagnostics::log("Frontend refresh requested after wkSuperFrontend reload");
		}

		std::string resizeError;
		bool resized = resizeGameWindowClientArea(effectiveResolution, &resizeError);
		if (resized) {
			Diagnostics::log("Game window resized after wkSuperFrontend live update");
		}

		std::string pokeError;
		bool poked = pokeGameWindowThroughSuperFrontendHook(&pokeError);
		if (poked) {
			Diagnostics::log("MoveWindow hook poke completed after wkSuperFrontend live update");
		}

		if (refreshed || resized || poked) {
			return true;
		}

		if (errorMessage != nullptr) {
			if (!frontendError.empty() && !resizeError.empty()) {
				*errorMessage = frontendError + " Also: " + resizeError;
			} else if (!frontendError.empty()) {
				*errorMessage = frontendError;
			} else if (!resizeError.empty()) {
				*errorMessage = resizeError;
			} else if (!pokeError.empty()) {
				*errorMessage = pokeError;
			} else {
				*errorMessage = "wkSuperFrontend live state was updated, but no live refresh path succeeded.";
			}
		}
		return false;
	}

	std::optional<std::string> getCurrentMonitorDeviceName() {
		HWND hwnd = findGameWindow();
		HMONITOR monitor = hwnd != nullptr
			? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
			: MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);

		MONITORINFOEXA monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (!GetMonitorInfoA(monitor, &monitorInfo)) {
			return std::nullopt;
		}

		return std::string(monitorInfo.szDevice);
	}

	HRESULT callSetDisplayMode(LPDIRECTDRAW2 self, DWORD width, DWORD height, DWORD bpp, DWORD refreshRate, DWORD flags) {
		auto **vtable = reinterpret_cast<void ***>(self);
		auto fn = reinterpret_cast<SetDisplayModeFn>((*vtable)[DirectDraw2SetDisplayModeIndex]);
		return fn(self, width, height, bpp, refreshRate, flags);
	}

	HRESULT callLegacySetDisplayMode(LPDIRECTDRAW self, DWORD width, DWORD height, DWORD bpp) {
		auto **vtable = reinterpret_cast<void ***>(self);
		auto fn = reinterpret_cast<LegacySetDisplayModeFn>((*vtable)[DirectDraw2SetDisplayModeIndex]);
		return fn(self, width, height, bpp);
	}

	std::vector<DisplayResolution> enumerateMonitorResolutions(const std::optional<std::string> &deviceName) {
		std::vector<DisplayResolution> resolutions;
		DEVMODEA mode{};
		mode.dmSize = sizeof(mode);
		const char *device = deviceName && !deviceName->empty() ? deviceName->c_str() : nullptr;

		for (DWORD index = 0; EnumDisplaySettingsExA(device, index, &mode, 0); ++index) {
			if (mode.dmPelsWidth < MinimumWidth || mode.dmPelsHeight < MinimumHeight) {
				continue;
			}
			DisplayResolution resolution;
			resolution.width = mode.dmPelsWidth;
			resolution.height = mode.dmPelsHeight;
			resolutions.push_back(resolution);
		}

		std::sort(resolutions.begin(), resolutions.end(), [](const DisplayResolution &lhs, const DisplayResolution &rhs) {
			unsigned long long lhsPixels = static_cast<unsigned long long>(lhs.width) * lhs.height;
			unsigned long long rhsPixels = static_cast<unsigned long long>(rhs.width) * rhs.height;
			if (lhsPixels != rhsPixels) {
				return lhsPixels < rhsPixels;
			}
			if (lhs.width != rhs.width) {
				return lhs.width < rhs.width;
			}
			return lhs.height < rhs.height;
		});

		resolutions.erase(std::unique(resolutions.begin(), resolutions.end()), resolutions.end());
		return resolutions;
	}

	std::optional<DisplayResolution> queryCurrentMonitorResolution() {
		DEVMODEA mode{};
		mode.dmSize = sizeof(mode);
		const auto deviceName = getCurrentMonitorDeviceName();
		const char *device = deviceName && !deviceName->empty() ? deviceName->c_str() : nullptr;
		if (!EnumDisplaySettingsExA(device, ENUM_CURRENT_SETTINGS, &mode, 0)) {
			return std::nullopt;
		}
		DisplayResolution resolution;
		resolution.width = mode.dmPelsWidth;
		resolution.height = mode.dmPelsHeight;
		return resolution;
	}

	bool writeConfiguredResolution(const DisplayResolution &resolution, std::string *errorMessage) {
		HKEY key = nullptr;
		LSTATUS status = RegCreateKeyExA(
			HKEY_CURRENT_USER,
			WaOptionsRegistryPath,
			0,
			nullptr,
			0,
			KEY_SET_VALUE,
			nullptr,
			&key,
			nullptr);
		if (status != ERROR_SUCCESS) {
			if (errorMessage != nullptr) {
				*errorMessage = "Failed to open the WA options registry key.";
			}
			return false;
		}

		const auto closeKey = [&]() {
			if (key != nullptr) {
				RegCloseKey(key);
				key = nullptr;
			}
		};

		status = RegSetValueExA(
			key,
			WaDisplayWidthValue,
			0,
			REG_DWORD,
			reinterpret_cast<const BYTE *>(&resolution.width),
			sizeof(resolution.width));
		if (status != ERROR_SUCCESS) {
			closeKey();
			if (errorMessage != nullptr) {
				*errorMessage = "Failed to store DisplayXSize in the WA options.";
			}
			return false;
		}

		status = RegSetValueExA(
			key,
			WaDisplayHeightValue,
			0,
			REG_DWORD,
			reinterpret_cast<const BYTE *>(&resolution.height),
			sizeof(resolution.height));
		closeKey();
		if (status != ERROR_SUCCESS) {
			if (errorMessage != nullptr) {
				*errorMessage = "Failed to store DisplayYSize in the WA options.";
			}
			return false;
		}

		Diagnostics::log("Stored resolution in registry: %lux%lu", resolution.width, resolution.height);
		return true;
	}

	std::optional<DisplayResolution> readConfiguredResolution() {
		HKEY key = nullptr;
		LSTATUS status = RegOpenKeyExA(HKEY_CURRENT_USER, WaOptionsRegistryPath, 0, KEY_QUERY_VALUE, &key);
		if (status != ERROR_SUCCESS || key == nullptr) {
			return std::nullopt;
		}

		DWORD width = 0;
		DWORD height = 0;
		DWORD widthSize = sizeof(width);
		DWORD heightSize = sizeof(height);
		DWORD widthType = 0;
		DWORD heightType = 0;

		status = RegQueryValueExA(key, WaDisplayWidthValue, nullptr, &widthType, reinterpret_cast<BYTE *>(&width), &widthSize);
		if (status != ERROR_SUCCESS || widthType != REG_DWORD) {
			RegCloseKey(key);
			return std::nullopt;
		}

		status = RegQueryValueExA(key, WaDisplayHeightValue, nullptr, &heightType, reinterpret_cast<BYTE *>(&height), &heightSize);
		RegCloseKey(key);
		if (status != ERROR_SUCCESS || heightType != REG_DWORD) {
			return std::nullopt;
		}

		if (width < MinimumWidth || height < MinimumHeight) {
			return std::nullopt;
		}

		DisplayResolution resolution;
		resolution.width = width;
		resolution.height = height;
		return resolution;
	}

	std::optional<DisplayResolution> tryGetLiveSuperFrontendResolution() {
		HMODULE superFrontendModule = GetModuleHandleA("wkSuperFrontend.dll");
		if (superFrontendModule == nullptr) {
			return std::nullopt;
		}

		DisplayResolution resolution{};
		DWORD moduleBase = static_cast<DWORD>(reinterpret_cast<uintptr_t>(superFrontendModule));
		if (!tryReadSuperFrontendResolution(moduleBase, WkSuperFrontendTargetWidthOffset, WkSuperFrontendTargetHeightOffset, &resolution)) {
			return std::nullopt;
		}

		if (resolution.width < MinimumWidth || resolution.height < MinimumHeight) {
			return std::nullopt;
		}

		return resolution;
	}

	bool lastAppliedMatchesResolution(const DisplayResolution &resolution) {
		std::lock_guard<std::mutex> lock(stateMutex);
		return lastAppliedResolution.has_value() && *lastAppliedResolution == resolution;
	}

	void releaseStoredDirectDraw() {
		std::lock_guard<std::mutex> lock(stateMutex);
		if (directDraw2 != nullptr) {
			directDraw2->Release();
			directDraw2 = nullptr;
		}
		directDraw2IsFallback = false;
	}

	void attachDirectDraw2(LPDIRECTDRAW2 dd2, bool isFallback = false) {
		if (dd2 == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(stateMutex);
		if (directDraw2 == dd2) {
			directDraw2IsFallback = directDraw2IsFallback && isFallback;
			dd2->Release();
			return;
		}

		if (directDraw2 != nullptr && !directDraw2IsFallback && isFallback) {
			dd2->Release();
			return;
		}

		if (directDraw2 != nullptr) {
			directDraw2->Release();
		}
		directDraw2 = dd2;
		directDraw2IsFallback = isFallback;
	}

	LPDIRECTDRAW tryGetLegacyDirectDrawFromDisplayStruct(DWORD displayAddress, const char *sourceName) {
		if (displayAddress == 0) {
			return nullptr;
		}

		DWORD candidate = 0;
		if (!readDwordSafely(displayAddress + WaDisplayLegacyDirectDrawOffset, &candidate) ||
			candidate == 0 ||
			!looksLikeDirectDrawObject(candidate)) {
			return nullptr;
		}

		LPDIRECTDRAW dd = reinterpret_cast<LPDIRECTDRAW>(candidate);
		__try {
			dd->AddRef();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}

		Diagnostics::log(
			"Recovered legacy DirectDraw from %s+0x%X: source=0x%X object=0x%X",
			sourceName,
			WaDisplayLegacyDirectDrawOffset,
			displayAddress,
			candidate);
		return dd;
	}

	std::optional<DisplayResolution> getRequestedResolutionCopy() {
		std::lock_guard<std::mutex> lock(stateMutex);
		return requestedResolution;
	}

	bool tryAttachDirectDrawCandidate(DWORD sourceAddress, DWORD candidate, const char *sourceName, DWORD offset) {
		if (candidate == 0) {
			return false;
		}

		LPDIRECTDRAW2 dd2 = nullptr;
		if (looksLikeDirectDrawObject(candidate)) {
			dd2 = queryDirectDraw2(reinterpret_cast<IUnknown *>(candidate));
		} else {
			DWORD nestedCandidate = 0;
			if (readDwordSafely(candidate, &nestedCandidate) && nestedCandidate != 0 && looksLikeDirectDrawObject(nestedCandidate)) {
				dd2 = queryDirectDraw2(reinterpret_cast<IUnknown *>(nestedCandidate));
				candidate = nestedCandidate;
			}
		}

		if (dd2 == nullptr) {
			return false;
		}

		Diagnostics::log(
			"Recovered IDirectDraw2 from %s+0x%X: source=0x%X object=0x%X dd2=0x%X",
			sourceName,
			offset,
			sourceAddress,
			candidate,
			static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)));
		attachDirectDraw2(dd2, false);
		installSetDisplayModeHook(dd2);
		return true;
	}

	DWORD getReadableScanLength(DWORD address, DWORD maxLength) {
		if (address == 0 || maxLength == 0) {
			return 0;
		}

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
			return 0;
		}

		DWORD regionStart = static_cast<DWORD>(reinterpret_cast<uintptr_t>(mbi.BaseAddress));
		DWORD regionEnd = regionStart + static_cast<DWORD>(mbi.RegionSize);
		DWORD offsetIntoRegion = address - regionStart;
		DWORD remaining = regionEnd > address ? regionEnd - address : 0;
		DWORD length = (std::min)(maxLength, remaining);
		if (offsetIntoRegion >= static_cast<DWORD>(mbi.RegionSize)) {
			return 0;
		}
		return length;
	}

	void tryAttachExistingDirectDrawFromDisplayStruct() {
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (directDraw2 != nullptr && !directDraw2IsFallback) {
				return;
			}
		}

		struct SearchTarget {
			DWORD address;
			const char *name;
		};

		SearchTarget targets[] = {
			{W2App::getAddrDdDisplay(), "ddDisplay"},
			{W2App::getAddrDdGame(), "ddGame"},
			{W2App::getAddrDdWrapper(), "ddWrapper"},
		};

		for (const auto &target : targets) {
			if (target.address == 0) {
				continue;
			}

			DWORD scanLength = getReadableScanLength(target.address, 0x20000);
			if (scanLength == 0) {
				scanLength = 0x2000;
			}

			for (DWORD offset = 0; offset + sizeof(DWORD) <= scanLength; offset += sizeof(DWORD)) {
				DWORD candidate = 0;
				if (!readDwordSafely(target.address + offset, &candidate)) {
					continue;
				}
				if (tryAttachDirectDrawCandidate(target.address, candidate, target.name, offset)) {
					return;
				}
			}
		}
	}

	void tryAttachDirectDrawFromMainModuleMemory() {
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (directDraw2 != nullptr && !directDraw2IsFallback) {
				return;
			}
		}

		HMODULE mainModule = GetModuleHandleA(nullptr);
		if (mainModule == nullptr) {
			return;
		}

		auto *dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(mainModule);
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
			return;
		}

		auto *ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE *>(mainModule) + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
			return;
		}

		DWORD base = static_cast<DWORD>(reinterpret_cast<uintptr_t>(mainModule));
		DWORD end = base + ntHeaders->OptionalHeader.SizeOfImage;
		std::set<DWORD> seenCandidates;
		for (DWORD address = base; address + sizeof(DWORD) <= end; address += sizeof(DWORD)) {
			DWORD candidate = 0;
			if (!readDwordSafely(address, &candidate) || candidate == 0) {
				continue;
			}
			if (!seenCandidates.insert(candidate).second || !looksLikeDirectDrawObject(candidate)) {
				continue;
			}

			LPDIRECTDRAW2 dd2 = queryDirectDraw2(reinterpret_cast<IUnknown *>(candidate));
			if (dd2 == nullptr) {
				continue;
			}

			Diagnostics::log(
				"Recovered IDirectDraw2 from WA memory: source=0x%X object=0x%X dd2=0x%X",
				address,
				candidate,
				static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)));
			attachDirectDraw2(dd2, false);
			installSetDisplayModeHook(dd2);
			return;
		}
	}

	void installSetDisplayModeHook(LPDIRECTDRAW2 dd2) {
		if (dd2 == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(stateMutex);
		if (setDisplayModeHookInstalled) {
			return;
		}

		auto **vtable = reinterpret_cast<void ***>(dd2);
		DWORD addrSetDisplayMode = static_cast<DWORD>(reinterpret_cast<uintptr_t>((*vtable)[DirectDraw2SetDisplayModeIndex]));
		if (addrSetDisplayMode == legacySetDisplayModeHookAddress && legacySetDisplayModeHookInstalled) {
			Diagnostics::log("DirectDraw2 SetDisplayMode shares the legacy hook at 0x%X", addrSetDisplayMode);
			setDisplayModeHookInstalled = true;
			setDisplayModeHookAddress = addrSetDisplayMode;
			return;
		}
		Diagnostics::log("Installing SetDisplayMode hook at 0x%X", addrSetDisplayMode);
		Hooks::hook("DirectDraw2::SetDisplayMode", addrSetDisplayMode, (DWORD *)&hookSetDisplayMode, (DWORD *)&origSetDisplayMode, __CALLPOSITION__);
		setDisplayModeHookInstalled = true;
		setDisplayModeHookAddress = addrSetDisplayMode;
	}

	void installLegacySetDisplayModeHook(LPDIRECTDRAW dd) {
		if (dd == nullptr) {
			return;
		}

		std::lock_guard<std::mutex> lock(stateMutex);
		if (legacySetDisplayModeHookInstalled) {
			return;
		}

		auto **vtable = reinterpret_cast<void ***>(dd);
		DWORD addrLegacySetDisplayMode = static_cast<DWORD>(reinterpret_cast<uintptr_t>((*vtable)[DirectDraw2SetDisplayModeIndex]));
		if (addrLegacySetDisplayMode == setDisplayModeHookAddress && setDisplayModeHookInstalled) {
			Diagnostics::log("Legacy SetDisplayMode shares the DirectDraw2 hook at 0x%X", addrLegacySetDisplayMode);
			legacySetDisplayModeHookInstalled = true;
			legacySetDisplayModeHookAddress = addrLegacySetDisplayMode;
			return;
		}

		Diagnostics::log("Installing legacy SetDisplayMode hook at 0x%X", addrLegacySetDisplayMode);
		Hooks::hook("DirectDraw::SetDisplayMode", addrLegacySetDisplayMode, (DWORD *)&hookLegacySetDisplayMode, (DWORD *)&origLegacySetDisplayMode, __CALLPOSITION__);
		legacySetDisplayModeHookInstalled = true;
		legacySetDisplayModeHookAddress = addrLegacySetDisplayMode;
	}

	void installGlobalSetDisplayModeHook() {
		if (origDirectDrawCreate == nullptr) {
			return;
		}

		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if ((legacySetDisplayModeHookInstalled && setDisplayModeHookInstalled) || globalSetDisplayModeHookAttempted) {
				return;
			}
			globalSetDisplayModeHookAttempted = true;
		}

		LPDIRECTDRAW dd = nullptr;
		HRESULT hr = origDirectDrawCreate(nullptr, &dd, nullptr);
		Diagnostics::log("Temporary DirectDrawCreate for global hook -> %s", formatHRESULT(hr).c_str());
		if (FAILED(hr) || dd == nullptr) {
			return;
		}

		installLegacySetDisplayModeHook(dd);
		LPDIRECTDRAW2 dd2 = queryDirectDraw2(dd);
		if (dd2 != nullptr) {
			Diagnostics::log("Temporary IDirectDraw2 for global hook: 0x%X", static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)));
			attachDirectDraw2(dd2, true);
			Diagnostics::log("Temporary IDirectDraw2 bound as fallback for live apply");
			installSetDisplayModeHook(dd2);
		}
		dd->Release();
	}

	LPDIRECTDRAW2 queryDirectDraw2(IUnknown *unknown) {
		if (unknown == nullptr) {
			return nullptr;
		}

		LPDIRECTDRAW2 dd2 = nullptr;
		__try {
			if (FAILED(unknown->QueryInterface(IID_IDirectDraw2Local, reinterpret_cast<void **>(&dd2)))) {
				return nullptr;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
		return dd2;
	}

	std::string formatHRESULT(HRESULT hr) {
		std::ostringstream stream;
		stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
		return stream.str();
	}

	HRESULT WINAPI hookDirectDrawCreate(GUID *lpGUID, LPDIRECTDRAW *lplpDD, IUnknown *pUnkOuter) {
		HRESULT hr = origDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
		Diagnostics::log("DirectDrawCreate -> %s", formatHRESULT(hr).c_str());
		if (SUCCEEDED(hr) && lplpDD != nullptr && *lplpDD != nullptr) {
			installLegacySetDisplayModeHook(*lplpDD);
			LPDIRECTDRAW2 dd2 = queryDirectDraw2(*lplpDD);
			if (dd2 != nullptr) {
				Diagnostics::log("Captured IDirectDraw2 via DirectDrawCreate: 0x%X", static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)));
				attachDirectDraw2(dd2, false);
				installSetDisplayModeHook(dd2);
			}
		}
		return hr;
	}

	HRESULT WINAPI hookDirectDrawCreateEx(GUID *lpGUID, LPVOID *lplpDD, REFIID iid, IUnknown *pUnkOuter) {
		HRESULT hr = origDirectDrawCreateEx(lpGUID, lplpDD, iid, pUnkOuter);
		Diagnostics::log("DirectDrawCreateEx -> %s", formatHRESULT(hr).c_str());
		if (SUCCEEDED(hr) && lplpDD != nullptr && *lplpDD != nullptr) {
			LPDIRECTDRAW2 dd2 = queryDirectDraw2(reinterpret_cast<IUnknown *>(*lplpDD));
			if (dd2 != nullptr) {
				Diagnostics::log("Captured IDirectDraw2 via DirectDrawCreateEx: 0x%X", static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)));
				attachDirectDraw2(dd2, false);
				installSetDisplayModeHook(dd2);
			}
		}
		return hr;
	}

	HRESULT WINAPI hookLegacySetDisplayMode(LPDIRECTDRAW self, DWORD dwWidth, DWORD dwHeight, DWORD dwBpp) {
		DWORD requestedWidth = dwWidth;
		DWORD requestedHeight = dwHeight;

		DisplayResolution overrideResolution{};
		if (shouldOverrideSetDisplayModeRequest(dwWidth, dwHeight, dwBpp, &overrideResolution)) {
			dwWidth = overrideResolution.width;
			dwHeight = overrideResolution.height;
		}

		HRESULT hr = origLegacySetDisplayMode(self, dwWidth, dwHeight, dwBpp);
		Diagnostics::log(
			"Legacy SetDisplayMode request=%lux%lu applied=%lux%lu bpp=%lu result=%s",
			requestedWidth,
			requestedHeight,
			dwWidth,
			dwHeight,
			dwBpp,
			formatHRESULT(hr).c_str());

		if (SUCCEEDED(hr) && dwWidth > 0 && dwHeight > 0) {
			std::lock_guard<std::mutex> lock(stateMutex);
			DisplayResolution resolution;
			resolution.width = dwWidth;
			resolution.height = dwHeight;
			lastAppliedResolution = resolution;
		}
		return hr;
	}

	HRESULT WINAPI hookSetDisplayMode(LPDIRECTDRAW2 self, DWORD dwWidth, DWORD dwHeight, DWORD dwBpp, DWORD dwRefreshRate, DWORD dwFlags) {
		DWORD requestedWidth = dwWidth;
		DWORD requestedHeight = dwHeight;

		DisplayResolution overrideResolution{};
		if (shouldOverrideSetDisplayModeRequest(dwWidth, dwHeight, dwBpp, &overrideResolution)) {
			dwWidth = overrideResolution.width;
			dwHeight = overrideResolution.height;
		}

		HRESULT hr = origSetDisplayMode(self, dwWidth, dwHeight, dwBpp, dwRefreshRate, dwFlags);
		Diagnostics::log(
			"SetDisplayMode request=%lux%lu applied=%lux%lu bpp=%lu flags=%lu result=%s",
			requestedWidth,
			requestedHeight,
			dwWidth,
			dwHeight,
			dwBpp,
			dwFlags,
			formatHRESULT(hr).c_str());

		if (SUCCEEDED(hr) && dwWidth > 0 && dwHeight > 0) {
			std::lock_guard<std::mutex> lock(stateMutex);
			DisplayResolution resolution;
			resolution.width = dwWidth;
			resolution.height = dwHeight;
			lastAppliedResolution = resolution;
		}
		return hr;
	}

	bool applyResolutionViaDirectDraw(const DisplayResolution &resolution, DisplayResolution *appliedResolution, std::string *errorMessage) {
		tryAttachExistingDirectDrawFromDisplayStruct();
		tryAttachDirectDrawFromMainModuleMemory();

		LPDIRECTDRAW legacyDd = tryGetLegacyDirectDrawFromDisplayStruct(W2App::getAddrDdDisplay(), "ddDisplay");
		if (legacyDd != nullptr) {
			Diagnostics::log(
				"Calling legacy DirectDraw SetDisplayMode on 0x%X for target=%lux%lu",
				static_cast<DWORD>(reinterpret_cast<uintptr_t>(legacyDd)),
				resolution.width,
				resolution.height);
			HRESULT legacyHr = callLegacySetDisplayMode(legacyDd, resolution.width, resolution.height, 0);
			if (FAILED(legacyHr)) {
				Diagnostics::log(
					"Legacy DirectDraw SetDisplayMode returned %s for target=%lux%lu with bpp=0; retrying with bpp=8",
					formatHRESULT(legacyHr).c_str(),
					resolution.width,
					resolution.height);
				legacyHr = callLegacySetDisplayMode(legacyDd, resolution.width, resolution.height, 8);
			}
			legacyDd->Release();
			Diagnostics::log(
				"Legacy DirectDraw SetDisplayMode returned %s for target=%lux%lu",
				formatHRESULT(legacyHr).c_str(),
				resolution.width,
				resolution.height);
			if (SUCCEEDED(legacyHr)) {
				if (appliedResolution != nullptr) {
					*appliedResolution = resolution;
				}
				return true;
			}
		}

		LPDIRECTDRAW2 dd2 = nullptr;
		bool usingFallbackBinding = false;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			if (directDraw2 != nullptr && !directDraw2IsFallback) {
				directDraw2->AddRef();
				dd2 = directDraw2;
			} else if (directDraw2 != nullptr) {
				directDraw2->AddRef();
				dd2 = directDraw2;
				usingFallbackBinding = true;
			}
		}

		if (dd2 == nullptr) {
			Diagnostics::log(
				"DirectDraw apply skipped: no live IDirectDraw2 binding for target=%lux%lu",
				resolution.width,
				resolution.height);
			if (errorMessage != nullptr) {
				*errorMessage = "DirectDraw is not ready yet; retry from the lobby.";
			}
			return false;
		}

		if (usingFallbackBinding) {
			Diagnostics::log(
				"Using fallback IDirectDraw2 binding for target=%lux%lu because no live binding was found yet",
				resolution.width,
				resolution.height);
		}

		Diagnostics::log(
			"Calling DirectDraw SetDisplayMode on 0x%X for target=%lux%lu",
			static_cast<DWORD>(reinterpret_cast<uintptr_t>(dd2)),
			resolution.width,
			resolution.height);
		HRESULT hr = callSetDisplayMode(dd2, resolution.width, resolution.height, 8, 0, 0);
		dd2->Release();
		Diagnostics::log(
			"DirectDraw SetDisplayMode returned %s for target=%lux%lu",
			formatHRESULT(hr).c_str(),
			resolution.width,
			resolution.height);
		if (FAILED(hr)) {
			if (errorMessage != nullptr) {
				*errorMessage = "Failed to apply " + ResolutionManager::formatResolution(resolution) + " (" + formatHRESULT(hr) + ").";
			}
			return false;
		}

		if (appliedResolution != nullptr) {
			*appliedResolution = resolution;
		}
		return true;
	}

	bool applyResolution(const DisplayResolution &resolution, DisplayResolution *appliedResolution, std::string *errorMessage, DWORD frontendContext) {
		Diagnostics::log(
			"Applying resolution request: target=%lux%lu frontendContext=0x%X",
			resolution.width,
			resolution.height,
			frontendContext);

		bool usePseudoFullscreen = !isResolutionListedForCurrentMonitor(resolution);
		std::string pseudoModeError;
		if (usePseudoFullscreen) {
			if (!preparePseudoFullscreenMode(resolution, &pseudoModeError)) {
				if (errorMessage != nullptr) {
					*errorMessage = pseudoModeError;
				}
				return false;
			}
		} else {
			if (!disablePseudoFullscreenMode(&pseudoModeError)) {
				if (errorMessage != nullptr) {
					*errorMessage = pseudoModeError;
				}
				return false;
			}
		}

		{
			std::lock_guard<std::mutex> lock(stateMutex);
			requestedResolution = resolution;
		}

		std::string registryError;
		if (!writeConfiguredResolution(resolution, &registryError)) {
			if (errorMessage != nullptr) {
				*errorMessage = registryError;
			}
			return false;
		}

		DWORD desiredWindowedMode = readOptionDwordValue(WaWindowedModeValue).value_or(usePseudoFullscreen ? 1 : 0);
		Diagnostics::log(
			"Prepared WA windowed mode state for apply: target=%lux%lu desiredWindowed=%lu pseudo=%s",
			resolution.width,
			resolution.height,
			desiredWindowedMode,
			usePseudoFullscreen ? "true" : "false");

		std::string staticOptionsError;
		bool staticOptionsApplied = applyResolutionToWaStaticOptions(resolution, desiredWindowedMode, &staticOptionsError);
		if (staticOptionsApplied) {
			Diagnostics::log("WA static options prepared for %lux%lu", resolution.width, resolution.height);
		} else if (!staticOptionsError.empty()) {
			Diagnostics::log("WA static options update skipped: %s", staticOptionsError.c_str());
		}

		std::string liveOptionsError;
		bool liveOptionsApplied = applyResolutionToWaLiveOptions(resolution, desiredWindowedMode, &liveOptionsError);
		if (liveOptionsApplied) {
			Diagnostics::log("WA live options prepared for %lux%lu", resolution.width, resolution.height);
		} else if (!liveOptionsError.empty()) {
			Diagnostics::log("WA live options update skipped: %s", liveOptionsError.c_str());
		}

		std::string directDrawError;
		bool directDrawApplied = false;
		if (!usePseudoFullscreen) {
			directDrawApplied = applyResolutionViaDirectDraw(resolution, appliedResolution, &directDrawError);
			if (directDrawApplied) {
				Diagnostics::log("Applied resolution through DirectDraw: %lux%lu", resolution.width, resolution.height);
				refreshScreenPaletteAfterLiveChange("DirectDraw mode switch");
			}
		} else {
			Diagnostics::log(
				"Skipping DirectDraw mode switch for unsupported custom resolution %lux%lu and using pseudo-fullscreen instead",
				resolution.width,
				resolution.height);
		}

		std::string superFrontendError;
		bool superFrontendApplied = applyResolutionViaSuperFrontend(resolution, frontendContext, &superFrontendError);
		if (superFrontendApplied) {
			Diagnostics::log("wkSuperFrontend live refresh requested for %lux%lu", resolution.width, resolution.height);
			refreshScreenPaletteAfterLiveChange("wkSuperFrontend live refresh");
		}

		if (lastAppliedMatchesResolution(resolution)) {
			if (appliedResolution != nullptr) {
				*appliedResolution = resolution;
			}
			return true;
		}

		std::string frontendError;
		bool refreshed = false;
		if (!superFrontendApplied) {
			refreshed = requestFrontendRefresh(frontendContext, &frontendError);
		}
		if (refreshed) {
			Diagnostics::log("Frontend refresh requested for %lux%lu", resolution.width, resolution.height);
			refreshScreenPaletteAfterLiveChange("frontend refresh");
		}

		std::string pseudoLayoutError;
		bool pseudoFullscreenApplied = false;
		if (usePseudoFullscreen && (refreshed || isPseudoFullscreenManagedFor(resolution) || isPseudoFullscreenPendingFor(resolution))) {
			pseudoFullscreenApplied = applyPseudoFullscreenWindowLayout(&pseudoLayoutError);
			if (pseudoFullscreenApplied) {
				Diagnostics::log("Pseudo-fullscreen layout applied for %lux%lu", resolution.width, resolution.height);
			} else if (!pseudoLayoutError.empty()) {
				Diagnostics::log("Pseudo-fullscreen layout skipped: %s", pseudoLayoutError.c_str());
			}
		}

		if (!directDrawApplied && !superFrontendApplied && !refreshed && !liveOptionsApplied && !staticOptionsApplied && !pseudoFullscreenApplied) {
			if (errorMessage != nullptr) {
				if (!pseudoModeError.empty() && !frontendError.empty()) {
					*errorMessage = pseudoModeError + " Also: " + frontendError;
				} else if (!pseudoModeError.empty() && !pseudoLayoutError.empty()) {
					*errorMessage = pseudoModeError + " Also: " + pseudoLayoutError;
				} else if (!staticOptionsError.empty() && !liveOptionsError.empty()) {
					*errorMessage = staticOptionsError + " Also: " + liveOptionsError;
				} else if (!staticOptionsError.empty() && !directDrawError.empty()) {
					*errorMessage = staticOptionsError + " Also: " + directDrawError;
				} else if (!staticOptionsError.empty() && !frontendError.empty()) {
					*errorMessage = staticOptionsError + " Also: " + frontendError;
				} else if (!liveOptionsError.empty() && !directDrawError.empty()) {
					*errorMessage = liveOptionsError + " Also: " + directDrawError;
				} else if (!liveOptionsError.empty() && !frontendError.empty()) {
					*errorMessage = liveOptionsError + " Also: " + frontendError;
				} else if (!directDrawError.empty() && !superFrontendError.empty()) {
					*errorMessage = directDrawError + " Also: " + superFrontendError;
				} else if (!superFrontendError.empty() && !frontendError.empty()) {
					*errorMessage = superFrontendError + " Also: " + frontendError;
				} else if (!directDrawError.empty() && !frontendError.empty()) {
					*errorMessage = directDrawError + " Also: " + frontendError;
				} else if (!pseudoModeError.empty()) {
					*errorMessage = pseudoModeError;
				} else if (!staticOptionsError.empty()) {
					*errorMessage = staticOptionsError;
				} else if (!liveOptionsError.empty()) {
					*errorMessage = liveOptionsError;
				} else if (!superFrontendError.empty()) {
					*errorMessage = superFrontendError;
				} else if (!frontendError.empty()) {
					*errorMessage = frontendError;
				} else {
					*errorMessage = directDrawError;
				}
			}
			return false;
		}

		std::string directDrawRetryError;
		if (!usePseudoFullscreen && !directDrawApplied && applyResolutionViaDirectDraw(resolution, appliedResolution, &directDrawRetryError)) {
			Diagnostics::log("Applied resolution through DirectDraw after frontend refresh: %lux%lu", resolution.width, resolution.height);
			refreshScreenPaletteAfterLiveChange("DirectDraw retry");
			return true;
		}

		if (pseudoFullscreenApplied) {
			if (appliedResolution != nullptr) {
				*appliedResolution = resolution;
			}
			return true;
		}

		if (lastAppliedMatchesResolution(resolution)) {
			if (appliedResolution != nullptr) {
				*appliedResolution = resolution;
			}
			return true;
		}

		if (appliedResolution != nullptr) {
			*appliedResolution = resolution;
		}
		if (errorMessage != nullptr) {
			*errorMessage = "resolution refresh requested, but WA did not switch immediately.";
		}
		Diagnostics::log("Resolution refresh requested without immediate confirmation: %lux%lu", resolution.width, resolution.height);
		return true;
	}
}

void ResolutionManager::install() {
	HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
	if (ddrawModule == nullptr) {
		ddrawModule = LoadLibraryA("ddraw.dll");
	}
	if (ddrawModule == nullptr) {
		throw std::runtime_error("Failed to load ddraw.dll");
	}

	DWORD addrDirectDrawCreate = static_cast<DWORD>(reinterpret_cast<uintptr_t>(GetProcAddress(ddrawModule, "DirectDrawCreate")));
	DWORD addrDirectDrawCreateEx = static_cast<DWORD>(reinterpret_cast<uintptr_t>(GetProcAddress(ddrawModule, "DirectDrawCreateEx")));
	Diagnostics::log("ResolutionManager install: DirectDrawCreate=0x%X DirectDrawCreateEx=0x%X", addrDirectDrawCreate, addrDirectDrawCreateEx);

	auto currentWindowedMode = readOptionDwordValue(WaWindowedModeValue);
	auto currentFrontendUseDesktopWindow = readOptionDwordValue(WaFrontendUseDesktopWindowValue);
	if (currentWindowedMode.value_or(0) == 0 && currentFrontendUseDesktopWindow.value_or(0) != 0) {
		std::string writeError;
		if (writeOptionDwordValue(WaFrontendUseDesktopWindowValue, 0, &writeError)) {
			Diagnostics::log(
				"Normalized stale WA frontend desktop-window state on install: WindowedMode=%lu FrontendUseDesktopWindow %lu -> 0",
				currentWindowedMode.value_or(0),
				currentFrontendUseDesktopWindow.value_or(0));
		} else if (!writeError.empty()) {
			Diagnostics::log("Failed to normalize stale WA frontend desktop-window state on install: %s", writeError.c_str());
		}
	}

	Hooks::hook("DirectDrawCreate", addrDirectDrawCreate, (DWORD *)&hookDirectDrawCreate, (DWORD *)&origDirectDrawCreate, __CALLPOSITION__);
	Hooks::hook("DirectDrawCreateEx", addrDirectDrawCreateEx, (DWORD *)&hookDirectDrawCreateEx, (DWORD *)&origDirectDrawCreateEx, __CALLPOSITION__);
	Diagnostics::log("ResolutionManager install: global SetDisplayMode hook deferred until runtime");
}

void ResolutionManager::refreshBinding() {
	installGlobalSetDisplayModeHook();
	tryAttachExistingDirectDrawFromDisplayStruct();
	tryAttachDirectDrawFromMainModuleMemory();
}

void ResolutionManager::onW2AppInitialized() {
	refreshBinding();

	DisplayResolution requested{};
	if (!tryGetRequestedResolution(&requested)) {
		return;
	}

	DWORD desiredWindowedMode = readOptionDwordValue(WaWindowedModeValue).value_or(isPseudoFullscreenManagedFor(requested) ? 1 : 0);
	Diagnostics::log(
		"Re-applying WA options after InitializeW2App: target=%s desiredWindowed=%lu pseudo=%s",
		formatResolution(requested).c_str(),
		desiredWindowedMode,
		isPseudoFullscreenManagedFor(requested) ? "true" : "false");

	std::string staticOptionsError;
	if (applyResolutionToWaStaticOptions(requested, desiredWindowedMode, &staticOptionsError)) {
		Diagnostics::log("Re-applied WA static options after InitializeW2App: %s", formatResolution(requested).c_str());
	} else if (!staticOptionsError.empty()) {
		Diagnostics::log("Post-initialize WA static options update skipped: %s", staticOptionsError.c_str());
	}

	std::string liveOptionsError;
	if (applyResolutionToWaLiveOptions(requested, desiredWindowedMode, &liveOptionsError)) {
		Diagnostics::log("Re-applied WA live options after InitializeW2App: %s", formatResolution(requested).c_str());
	} else if (!liveOptionsError.empty()) {
		Diagnostics::log("Post-initialize WA live options update skipped: %s", liveOptionsError.c_str());
	}

	if (isPseudoFullscreenManagedFor(requested)) {
		std::string pseudoLayoutError;
		if (applyPseudoFullscreenWindowLayout(&pseudoLayoutError)) {
			Diagnostics::log("Re-applied pseudo-fullscreen layout after InitializeW2App: %s", formatResolution(requested).c_str());
			return;
		}

		if (!pseudoLayoutError.empty()) {
			Diagnostics::log("Post-initialize pseudo-fullscreen layout skipped: %s", pseudoLayoutError.c_str());
		}
		return;
	}

	DisplayResolution applied{};
	std::string errorMessage;
	if (applyResolutionViaDirectDraw(requested, &applied, &errorMessage)) {
		Diagnostics::log("Applied requested resolution after InitializeW2App: %s", formatResolution(applied).c_str());
		return;
	}

	if (!errorMessage.empty()) {
		Diagnostics::log("Post-initialize DirectDraw apply skipped: %s", errorMessage.c_str());
	}
}

void ResolutionManager::shutdown() {
	bool shouldRestorePseudoFullscreenState = false;
	DisplayResolution restoreResolution{};
	bool haveRestoreResolution = false;
	DWORD restoreWindowedModeValue = 0;
	DWORD restoreFrontendUseDesktopWindowValue = 0;
	bool restoreFrontendDesktopWindow = false;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		shouldRestorePseudoFullscreenState = windowedModeForcedByModule;
		if (shouldRestorePseudoFullscreenState) {
			if (originalConfiguredResolution.has_value()) {
				restoreResolution = *originalConfiguredResolution;
				haveRestoreResolution = true;
			}
			restoreWindowedModeValue = originalWindowedModeValue.value_or(0);
			restoreFrontendUseDesktopWindowValue = originalFrontendUseDesktopWindowValue.value_or(0);
			restoreFrontendDesktopWindow = frontendDesktopWindowForcedByModule;
		}
	}

	if (shouldRestorePseudoFullscreenState) {
		std::string writeError;
		if (haveRestoreResolution) {
			if (writeConfiguredResolution(restoreResolution, &writeError)) {
				Diagnostics::log(
					"Shutdown restored pre-custom configured resolution: %s",
					formatResolution(restoreResolution).c_str());
			} else if (!writeError.empty()) {
				Diagnostics::log("Shutdown failed to restore pre-custom configured resolution: %s", writeError.c_str());
			}
		}

		writeError.clear();
		if (writeOptionDwordValue(WaWindowedModeValue, restoreWindowedModeValue, &writeError)) {
			Diagnostics::log("Shutdown restored WindowedMode=%lu", restoreWindowedModeValue);
		} else if (!writeError.empty()) {
			Diagnostics::log("Shutdown failed to restore WindowedMode: %s", writeError.c_str());
		}

		if (restoreFrontendDesktopWindow) {
			writeError.clear();
			if (writeOptionDwordValue(WaFrontendUseDesktopWindowValue, restoreFrontendUseDesktopWindowValue, &writeError)) {
				Diagnostics::log("Shutdown restored FrontendUseDesktopWindow=%lu", restoreFrontendUseDesktopWindowValue);
			} else if (!writeError.empty()) {
				Diagnostics::log("Shutdown failed to restore FrontendUseDesktopWindow: %s", writeError.c_str());
			}
		}
	}

	releaseStoredDirectDraw();
	clearPseudoFullscreenState(false);
}

std::vector<DisplayResolution> ResolutionManager::listAvailableResolutions() {
	const auto deviceName = getCurrentMonitorDeviceName();
	auto resolutions = enumerateMonitorResolutions(deviceName);
	if (!resolutions.empty()) {
		return resolutions;
	}
	return enumerateMonitorResolutions(std::nullopt);
}

std::optional<DisplayResolution> ResolutionManager::getCurrentResolution() {
	auto liveSuperFrontendResolution = tryGetLiveSuperFrontendResolution();

	DisplayResolution staticWaOptionsResolution{};
	if (tryReadWaStaticOptionsResolution(&staticWaOptionsResolution)) {
		if (liveSuperFrontendResolution.has_value() && *liveSuperFrontendResolution != staticWaOptionsResolution) {
			Diagnostics::log(
				"Current resolution source mismatch: preferring WA static options %s over wkSuperFrontend live %s",
				formatResolution(staticWaOptionsResolution).c_str(),
				formatResolution(*liveSuperFrontendResolution).c_str());
		}
		return staticWaOptionsResolution;
	}

	DisplayResolution liveWaOptionsResolution{};
	if (tryReadLiveWaOptionsResolution(&liveWaOptionsResolution)) {
		if (liveSuperFrontendResolution.has_value() && *liveSuperFrontendResolution != liveWaOptionsResolution) {
			Diagnostics::log(
				"Current resolution source mismatch: preferring WA live options %s over wkSuperFrontend live %s",
				formatResolution(liveWaOptionsResolution).c_str(),
				formatResolution(*liveSuperFrontendResolution).c_str());
		}
		return liveWaOptionsResolution;
	}

	if (auto configuredResolution = readConfiguredResolution(); configuredResolution.has_value()) {
		if (liveSuperFrontendResolution.has_value() && *liveSuperFrontendResolution != *configuredResolution) {
			Diagnostics::log(
				"Current resolution source mismatch: preferring configured WA resolution %s over wkSuperFrontend live %s",
				formatResolution(*configuredResolution).c_str(),
				formatResolution(*liveSuperFrontendResolution).c_str());
		}
		return configuredResolution;
	}

	if (auto currentRequestedResolution = getRequestedResolutionCopy(); currentRequestedResolution.has_value()) {
		if (liveSuperFrontendResolution.has_value() && *liveSuperFrontendResolution != *currentRequestedResolution) {
			Diagnostics::log(
				"Current resolution source mismatch: preferring requested resolution %s over wkSuperFrontend live %s",
				formatResolution(*currentRequestedResolution).c_str(),
				formatResolution(*liveSuperFrontendResolution).c_str());
		}
		return currentRequestedResolution;
	}

	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (lastAppliedResolution.has_value()) {
			if (liveSuperFrontendResolution.has_value() && *liveSuperFrontendResolution != *lastAppliedResolution) {
				Diagnostics::log(
					"Current resolution source mismatch: preferring last applied resolution %s over wkSuperFrontend live %s",
					formatResolution(*lastAppliedResolution).c_str(),
					formatResolution(*liveSuperFrontendResolution).c_str());
			}
			return lastAppliedResolution;
		}
	}

	if (liveSuperFrontendResolution.has_value()) {
		return liveSuperFrontendResolution;
	}

	return queryCurrentMonitorResolution();
}

bool ResolutionManager::tryGetRequestedResolution(DisplayResolution *resolution) {
	if (resolution == nullptr) {
		return false;
	}

	auto currentRequested = getRequestedResolutionCopy();
	if (!currentRequested.has_value()) {
		return false;
	}

	*resolution = *currentRequested;
	return true;
}

bool ResolutionManager::applyResolutionByIndex(size_t zeroBasedIndex, DisplayResolution *appliedResolution, std::string *errorMessage, DWORD frontendContext) {
	auto resolutions = listAvailableResolutions();
	if (zeroBasedIndex >= resolutions.size()) {
		if (errorMessage != nullptr) {
			*errorMessage = "Resolution index out of range. Use /checkres first.";
		}
		return false;
	}

	return applyResolution(resolutions[zeroBasedIndex], appliedResolution, errorMessage, frontendContext);
}

bool ResolutionManager::applyResolutionValue(const DisplayResolution &resolution, DisplayResolution *appliedResolution, std::string *errorMessage, DWORD frontendContext) {
	if (!isPlausibleResolutionValue(resolution.width, resolution.height)) {
		if (errorMessage != nullptr) {
			*errorMessage = "Custom resolution is out of range. Use at least 640x480.";
		}
		return false;
	}

	return applyResolution(resolution, appliedResolution, errorMessage, frontendContext);
}

std::string ResolutionManager::formatResolution(const DisplayResolution &resolution) {
	return std::to_string(resolution.width) + "x" + std::to_string(resolution.height);
}
