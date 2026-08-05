#ifndef WKSETRES_CHAT_H
#define WKSETRES_CHAT_H

#include <string>
#include <windows.h>

class Chat {
private:
	static int __fastcall hookLobbyClientCommands(void *self, void *edx, char **commandStrPtr, char **argStrPtr);
	static int __fastcall hookLobbyHostCommands(void *self, void *edx, char **commandStrPtr, char **argStrPtr);
	static int __fastcall hookLobbyCommand(DWORD self, DWORD edx, DWORD a1, DWORD a2);

public:
	static int onChatInput(DWORD self, const std::string &msg, int a3);
	static void callShowChatMessage(const std::string &msg, int color, DWORD resourceContext = 0);
	static void install();
};

#endif // WKSETRES_CHAT_H
