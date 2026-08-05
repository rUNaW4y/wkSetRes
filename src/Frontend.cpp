#include "Frontend.h"
#include "Diagnostics.h"
#include "Hooks.h"

namespace {
	void(__stdcall *origFrontendChangeScreen)(int screen) = nullptr;
}

void __stdcall Frontend::hookFrontendChangeScreen(int screen) {
	DWORD context = 0;
	_asm mov context, esi

	currentContext = context;
	currentScreen = screen;
	Diagnostics::log("FrontendChangeScreen seen: context=0x%X screen=%d", context, screen);

	_asm mov esi, context
	_asm push screen
	_asm call origFrontendChangeScreen
}

void Frontend::install() {
	DWORD addrFrontendChangeScreen = _ScanPattern(
		"FrontendChangeScreen",
		"\x83\x3D\x00\x00\x00\x00\x00\x53\x8B\x5C\x24\x08\x75\x14\x8B\x46\x3C\xA8\x18\x74\x59\x83\xE0\xEF\x89\x5E\x44\x89\x46\x3C\x5B\xC2\x04\x00\x6A\x00\x8B\xCE\xE8\x00\x00\x00\x00\x8B\x86\x00\x00\x00\x00\x50\x8B\x86\x00\x00\x00\x00\x68\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x33\xC0\x57",
		"???????xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx????xx????xxx????x????x????xxx");
	Hooks::hook(
		"FrontendChangeScreen",
		addrFrontendChangeScreen,
		(DWORD *)&hookFrontendChangeScreen,
		(DWORD *)&origFrontendChangeScreen,
		__CALLPOSITION__);
	Diagnostics::log("frontend installed: changeScreen=0x%X", addrFrontendChangeScreen);
}

int Frontend::getCurrentScreen() {
	return currentScreen;
}

DWORD Frontend::getCurrentContext() {
	return currentContext;
}

bool Frontend::refreshScreen(DWORD context, int screen, std::string *errorMessage) {
	if (origFrontendChangeScreen == nullptr) {
		if (errorMessage != nullptr) {
			*errorMessage = "frontend refresh is not available yet.";
		}
		return false;
	}

	if (context == 0 || screen == 0) {
		if (errorMessage != nullptr) {
			*errorMessage = "frontend context is not available yet.";
		}
		return false;
	}

	Diagnostics::log("FrontendChangeScreen refresh requested: context=0x%X screen=%d", context, screen);
	__try {
		DWORD savedEsi = 0;
		_asm mov savedEsi, esi
		_asm mov esi, context
		_asm push screen
		_asm call origFrontendChangeScreen
		_asm mov esi, savedEsi
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Diagnostics::log("FrontendChangeScreen refresh failed with an exception");
		if (errorMessage != nullptr) {
			*errorMessage = "frontend refresh failed with an exception.";
		}
		return false;
	}
}

bool Frontend::refreshCurrentScreen(std::string *errorMessage) {
	return refreshScreen(currentContext, currentScreen, errorMessage);
}
