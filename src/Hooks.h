#ifndef WKSETRES_HOOKS_H
#define WKSETRES_HOOKS_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <polyhook2/Detour/x86Detour.hpp>

#ifndef __CALLPOSITION__
#define STRINGIZE_DETAIL(x) #x
#define STRINGIZE(x) STRINGIZE_DETAIL(x)
#define __CALLPOSITION__ __FUNCTION__ ":" STRINGIZE(__LINE__)
#endif

class Hooks {
private:
	static inline std::map<std::string, DWORD> hookNameToAddr;
	static inline std::map<DWORD, std::string> hookAddrToName;
	static inline std::map<std::string, DWORD> scanNameToAddr;
	static inline std::vector<std::unique_ptr<PLH::x86Detour>> detours;

public:
	static void hook(std::string name, DWORD pTarget, DWORD *pDetour, DWORD *ppOriginal, const char *line = nullptr);
#define _Hook(name, pTarget, pDetour, ppOriginal) Hooks::hook(name, pTarget, pDetour, ppOriginal, __CALLPOSITION__)
#define _HookDefault(name) Hooks::hook(#name, addr##name, (DWORD *)&hook##name, (DWORD *)&orig##name, __CALLPOSITION__)

	static DWORD scanPattern(const char *name, const char *pattern, const char *mask, const char *line = nullptr);
#define _ScanPattern(name, pattern, mask) Hooks::scanPattern(name, pattern, mask, __CALLPOSITION__)
};

#endif // WKSETRES_HOOKS_H
