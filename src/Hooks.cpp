#include <stdexcept>
#include <sstream>
#include <PatternScanner.h>
#include <polyhook2/CapstoneDisassembler.hpp>
#include "Hooks.h"
#include "Debugf.h"

void Hooks::hook(std::string name, DWORD pTarget, DWORD *pDetour, DWORD *ppOriginal, const char *line) {
	static PLH::CapstoneDisassembler dis(PLH::Mode::x86);
	if (!pTarget) {
		throw std::runtime_error("Hook address is null: " + name);
	}
	if (hookNameToAddr.find(name) != hookNameToAddr.end()) {
		throw std::runtime_error("Hook name reused: " + name);
	}
	if (hookAddrToName.find(pTarget) != hookAddrToName.end()) {
		std::stringstream ss;
		ss << "The specified address is already hooked: " << name << "(0x" << std::hex << pTarget << "), " << hookAddrToName[pTarget];
		throw std::runtime_error(ss.str());
	}

	uint64_t trampoline = 0;
	auto detour = std::make_unique<PLH::x86Detour>(pTarget, (const uint64_t)pDetour, &trampoline, dis);
	if (!detour->hook()) {
		throw std::runtime_error("Failed to create hook: " + name);
	}
	detours.push_back(std::move(detour));
	*ppOriginal = static_cast<DWORD>(trampoline);

	hookAddrToName[pTarget] = name;
	hookNameToAddr[name] = pTarget;
	if (!line) {
		debugf("%s 0x%X -> 0x%X\n", name.c_str(), pTarget, (DWORD)pDetour);
	} else {
		printf("%s: hook: %s 0x%X -> 0x%X\n", line, name.c_str(), pTarget, (DWORD)pDetour);
	}
}

DWORD Hooks::scanPattern(const char *name, const char *pattern, const char *mask, const char *line) {
	auto it = scanNameToAddr.find(name);
	uintptr_t ret = 0;
	if (it != scanNameToAddr.end()) {
		ret = it->second;
	} else {
		ret = hl::FindPatternMask(pattern, mask);
		if (ret) {
			scanNameToAddr[name] = static_cast<DWORD>(ret);
		}
	}

	if (!line) {
		debugf("%s = 0x%X\n", name, ret);
	} else {
		printf("%s scanPattern: %s = 0x%X\n", line, name, ret);
	}
	if (!ret) {
		throw std::runtime_error(std::string("scanPattern: failed to find memory pattern: ") + name);
	}
	return static_cast<DWORD>(ret);
}
