#ifndef WKSETRES_W2APP_H
#define WKSETRES_W2APP_H

typedef unsigned long DWORD;

class W2App {
private:
	static inline DWORD addrDDGame = 0;
	static inline DWORD addrDDDisplay = 0;
	static inline DWORD addrDDWrapper = 0;
	static DWORD __stdcall hookInitializeW2App(DWORD ddGame, DWORD ddDisplay, DWORD dsSound, DWORD ddKeyboard, DWORD ddMouse, DWORD wavCdRom, DWORD wsGameNet);

public:
	static void install();
	static DWORD getAddrDdGame();
	static DWORD getAddrDdDisplay();
	static DWORD getAddrDdWrapper();
};

#endif // WKSETRES_W2APP_H
