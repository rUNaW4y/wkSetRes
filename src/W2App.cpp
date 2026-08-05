#include "W2App.h"
#include "Diagnostics.h"
#include "Hooks.h"
#include "ResolutionManager.h"

DWORD origInitializeW2App = 0;

DWORD __stdcall W2App::hookInitializeW2App(DWORD ddGame, DWORD ddDisplay, DWORD dsSound, DWORD ddKeyboard, DWORD ddMouse, DWORD wavCdRom, DWORD wsGameNet) {
	DWORD ddW2Wrapper = 0;
	DWORD retv = 0;
	_asm mov ddW2Wrapper, edi

	addrDDGame = ddGame;
	addrDDDisplay = ddDisplay;
	addrDDWrapper = ddW2Wrapper;
	Diagnostics::log("InitializeW2App: ddGame=0x%X ddDisplay=0x%X wrapper=0x%X", ddGame, ddDisplay, ddW2Wrapper);

	_asm push wsGameNet
	_asm push wavCdRom
	_asm push ddMouse
	_asm push ddKeyboard
	_asm push dsSound
	_asm push ddDisplay
	_asm push ddGame
	_asm mov edi, ddW2Wrapper
	_asm call origInitializeW2App
	_asm mov retv, eax

	ResolutionManager::onW2AppInitialized();
	Diagnostics::log("InitializeW2App complete: result=0x%X", retv);

	return retv;
}

void W2App::install() {
	DWORD addrInitializeW2App = _ScanPattern("InitializeW2App", "\x6A\xFF\x64\xA1\x00\x00\x00\x00\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x53\x8B\x5C\x24\x1C\x55\x8B\x6C\x24\x1C\x56\x8B\x74\x24\x1C\x33\xC0\x89\x86\x00\x00\x00\x00\x89\x44\x24\x14\x89\x86\x00\x00\x00\x00\x8B\xC7\xC7\x06\x00\x00\x00\x00\x89\x9E\x00\x00\x00\x00\x89\xAE\x00\x00\x00\x00\xE8\x00\x00\x00\x00", "????????x????xxxx????xxxxxxxxxxxxxxxxxxx????xxxxxx????xxxx????xx????xx????x????");
	Diagnostics::log("W2App install: init=0x%X", addrInitializeW2App);
	_HookDefault(InitializeW2App);
}

DWORD W2App::getAddrDdGame() {
	return addrDDGame;
}

DWORD W2App::getAddrDdDisplay() {
	return addrDDDisplay;
}

DWORD W2App::getAddrDdWrapper() {
	return addrDDWrapper;
}
