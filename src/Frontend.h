#ifndef WKSETRES_FRONTEND_H
#define WKSETRES_FRONTEND_H

#include <windows.h>
#include <string>

class Frontend {
private:
	static inline DWORD currentContext = 0;
	static inline int currentScreen = 0;
	static void __stdcall hookFrontendChangeScreen(int screen);

public:
	static void install();
	static int getCurrentScreen();
	static DWORD getCurrentContext();
	static bool refreshScreen(DWORD context, int screen, std::string *errorMessage);
	static bool refreshCurrentScreen(std::string *errorMessage);
};

#endif // WKSETRES_FRONTEND_H
