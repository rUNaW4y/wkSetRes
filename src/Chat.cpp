#include "Chat.h"
#include "Diagnostics.h"
#include "Hooks.h"
#include "ResolutionManager.h"
#include "W2App.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {
	int (__fastcall *origLobbyClientCommands)(void *self, void *edx, char **commandStrPtr, char **argStrPtr) = nullptr;
	int (__fastcall *origLobbyHostCommands)(void *self, void *edx, char **commandStrPtr, char **argStrPtr) = nullptr;
	DWORD origLobbyCommand = 0;
	void(__stdcall *addrShowChatMessage)(DWORD addrResourceObject, int color, char *msg, int unk) = nullptr;
	int (__stdcall *addrLobbyDisplayMessage)(int target, char *msg) = nullptr;
	DWORD addrLobbyShowText = 0;
	DWORD pendingSelectionOwner = 0;
	bool pendingSelectionActive = false;

	constexpr int ChatColorNotice = 6;
	constexpr int ChatColorCurrent = 1;
	constexpr DWORD LobbyMessageOffset = 0x10318;
	constexpr DWORD LobbyInputOffset = 0x10520;
	constexpr DWORD LobbyStringLengthOffset = 0x0C;
	constexpr DWORD LobbyOutputTargetArg2 = 0;
	constexpr DWORD LobbyOutputTargetArg3 = 0;
	constexpr size_t MaximumLobbyInputLength = 1024;

	int __stdcall callOriginalLobbyCommandImpl(DWORD self, DWORD a1, DWORD a2) {
		int result = 0;
		_asm mov ecx, self
		_asm push a2
		_asm push a1
		_asm call origLobbyCommand
		_asm mov result, eax
		return result;
	}

	std::string trimAndCollapseWhitespace(const std::string &text) {
		std::string normalized;
		normalized.reserve(text.size());

		bool lastWasSpace = true;
		for (unsigned char ch : text) {
			char out = static_cast<char>(ch);
			if (out == '\0' || out == '\r' || out == '\n' || out == '\t') {
				out = ' ';
			}

			if (std::isspace(static_cast<unsigned char>(out)) != 0) {
				if (!lastWasSpace) {
					normalized.push_back(' ');
					lastWasSpace = true;
				}
				continue;
			}

			normalized.push_back(out);
			lastWasSpace = false;
		}

		if (!normalized.empty() && normalized.back() == ' ') {
			normalized.pop_back();
		}

		return normalized;
	}

	std::string escapeForLog(const std::string &text) {
		std::string escaped;
		escaped.reserve(text.size());

		for (unsigned char ch : text) {
			if (ch >= 0x20 && ch <= 0x7E && ch != '\\') {
				escaped.push_back(static_cast<char>(ch));
				continue;
			}
			if (ch == '\\') {
				escaped += "\\\\";
				continue;
			}

			char buffer[5] = {};
			std::snprintf(buffer, sizeof(buffer), "\\x%02X", static_cast<unsigned int>(ch));
			escaped += buffer;
		}

		return escaped;
	}

	void setPendingSelection(DWORD self, bool active) {
		pendingSelectionOwner = self;
		pendingSelectionActive = active;
	}

	bool isPendingSelection(DWORD self) {
		return pendingSelectionActive && pendingSelectionOwner == self && self != 0;
	}

	bool tryGetLobbyInputBuffer(DWORD self, char **text, int *rawLength) {
		if (self == 0 || text == nullptr || rawLength == nullptr) {
			return false;
		}

		__try {
			auto *slot = reinterpret_cast<char **>(self + LobbyInputOffset);
			if (slot == nullptr || *slot == nullptr) {
				return false;
			}

			char *rawText = *slot;
			int textLength = *reinterpret_cast<int *>(rawText - LobbyStringLengthOffset);
			if (textLength <= 0 || static_cast<size_t>(textLength) > MaximumLobbyInputLength) {
				return false;
			}

			*text = rawText;
			*rawLength = textLength;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool tryGetLobbyInputText(DWORD self, std::string *text) {
		if (text == nullptr) {
			return false;
		}

		char *rawText = nullptr;
		int rawLength = 0;
		if (!tryGetLobbyInputBuffer(self, &rawText, &rawLength)) {
			return false;
		}

		*text = std::string(rawText, static_cast<size_t>(rawLength));
		return true;
	}

	void clearLobbyInputText(DWORD self) {
		char *rawText = nullptr;
		int rawLength = 0;
		if (!tryGetLobbyInputBuffer(self, &rawText, &rawLength)) {
			return;
		}

		__try {
			*reinterpret_cast<int *>(rawText - LobbyStringLengthOffset) = 0;
			rawText[0] = '\0';
			Diagnostics::log("lobby input cleared");
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Diagnostics::log("lobby input clear failed");
		}
	}

	void callLobbyShowText(DWORD self, const std::string &msg) {
		if (self == 0 || addrLobbyShowText == 0 || msg.empty()) {
			return;
		}

		const char *text = msg.c_str();
		Diagnostics::log("lobby message: %s", text);
		_asm xor edx, edx
		_asm push LobbyOutputTargetArg3
		_asm push LobbyOutputTargetArg2
		_asm push self
		_asm mov ecx, text
		_asm call addrLobbyShowText
	}

	void callLobbyDisplayMessage(DWORD self, const std::string &msg) {
		if (self == 0 || addrLobbyDisplayMessage == nullptr || msg.empty()) {
			return;
		}

		std::string buffer = "SYS::ALL:";
		buffer += msg;
		Diagnostics::log("lobby local display: target=0x%X text=%s", self, msg.c_str());
		addrLobbyDisplayMessage(static_cast<int>(self + LobbyMessageOffset), buffer.data());
	}

	void showCommandMessage(DWORD self, const std::string &msg) {
		if (self != 0 && addrLobbyDisplayMessage != nullptr) {
			callLobbyDisplayMessage(self, msg);
			return;
		}

		Diagnostics::log("local-only command message: color=%d text=%s", ChatColorNotice, msg.c_str());
		Chat::callShowChatMessage(msg, ChatColorNotice, self);
	}

	void showCommandMessageColored(DWORD self, const std::string &msg, int color) {
		if (self != 0 && addrLobbyDisplayMessage != nullptr) {
			callLobbyDisplayMessage(self, msg);
			return;
		}

		Diagnostics::log("local-only command message: color=%d text=%s", color, msg.c_str());
		Chat::callShowChatMessage(msg, color, self);
	}

	std::vector<std::string> splitWords(const std::string &text) {
		std::vector<std::string> parts;
		size_t index = 0;
		while (index < text.size()) {
			while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) {
				++index;
			}
			if (index >= text.size()) {
				break;
			}

			size_t start = index;
			while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0) {
				++index;
			}
			parts.emplace_back(text.substr(start, index - start));
		}
		return parts;
	}

	std::string buildCommandMessage(const char *commandText, const char *argText) {
		std::string message = "/";
		if (commandText != nullptr) {
			message += commandText;
		}
		if (argText != nullptr && *argText != '\0') {
			message.push_back(' ');
			message += argText;
		}
		return message;
	}

	bool tryParsePositiveIndex(const std::string &text, size_t *zeroBasedIndex) {
		if (text.empty() || zeroBasedIndex == nullptr) {
			return false;
		}

		char *end = nullptr;
		long value = std::strtol(text.c_str(), &end, 10);
		if (end == text.c_str() || *end != '\0' || value <= 0) {
			return false;
		}

		*zeroBasedIndex = static_cast<size_t>(value - 1);
		return true;
	}

	bool tryParseResolutionText(const std::string &text, DisplayResolution *resolution) {
		if (text.empty() || resolution == nullptr) {
			return false;
		}

		size_t separator = text.find('x');
		if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
			return false;
		}

		std::string widthText = text.substr(0, separator);
		std::string heightText = text.substr(separator + 1);

		char *widthEnd = nullptr;
		char *heightEnd = nullptr;
		unsigned long width = std::strtoul(widthText.c_str(), &widthEnd, 10);
		unsigned long height = std::strtoul(heightText.c_str(), &heightEnd, 10);
		if (widthEnd == widthText.c_str() || *widthEnd != '\0' || heightEnd == heightText.c_str() || *heightEnd != '\0') {
			return false;
		}

		resolution->width = static_cast<DWORD>(width);
		resolution->height = static_cast<DWORD>(height);
		return true;
	}

	void showUsage(DWORD self) {
		showCommandMessage(self, "wkSetResCustom: use /checkres to list the resolutions.");
		showCommandMessage(self, "wkSetResCustom: then type /setres 1, or simply 1 after the list.");
		showCommandMessage(self, "wkSetResCustom: you can also use /setres 1366x768 for a custom resolution.");
	}

	bool handleCheckRes(DWORD self) {
		auto resolutions = ResolutionManager::listAvailableResolutions();
		if (resolutions.empty()) {
			showCommandMessage(self, "No resolutions found for the current monitor.");
			return true;
		}

		auto currentResolution = ResolutionManager::getCurrentResolution();
		bool currentResolutionShown = false;
		showCommandMessage(self, "===== Module Powered by rUNaW4y =====");
		for (size_t index = 0; index < resolutions.size(); ++index) {
			std::string line = "[" + std::to_string(index + 1) + "] " + ResolutionManager::formatResolution(resolutions[index]);
			if (currentResolution.has_value() && resolutions[index] == *currentResolution) {
				line += " (current)";
				showCommandMessageColored(self, line, ChatColorCurrent);
				currentResolutionShown = true;
				continue;
			}
			showCommandMessage(self, line);
		}
		if (currentResolution.has_value() && !currentResolutionShown) {
			showCommandMessageColored(
				self,
				"[custom] " + ResolutionManager::formatResolution(*currentResolution) + " (current)",
				ChatColorCurrent);
		}
		showCommandMessage(self, "type /setres 1, /setres 2, or /setres 1366x768 to change your resolution.");
		setPendingSelection(self, true);
		return true;
	}

	bool handleSetRes(DWORD self, const std::vector<std::string> &parts) {
		if (parts.size() < 2) {
			showCommandMessage(self, "wkSetResCustom: no resolution specified.");
			handleCheckRes(self);
			return true;
		}

		size_t zeroBasedIndex = 0;
		DisplayResolution customResolution{};
		bool hasIndex = tryParsePositiveIndex(parts[1], &zeroBasedIndex);
		bool hasCustomResolution = !hasIndex && tryParseResolutionText(parts[1], &customResolution);
		if (!hasIndex && !hasCustomResolution) {
			setPendingSelection(self, false);
			showUsage(self);
			return true;
		}

		DisplayResolution appliedResolution{};
		std::string errorMessage;
		bool applied = hasIndex
			? ResolutionManager::applyResolutionByIndex(zeroBasedIndex, &appliedResolution, &errorMessage, self)
			: ResolutionManager::applyResolutionValue(customResolution, &appliedResolution, &errorMessage, self);
		if (!applied) {
			setPendingSelection(self, hasIndex);
			showCommandMessage(self, "wkSetResCustom: " + errorMessage);
			showCommandMessage(self, "wkSetResCustom: try again with /setres 1 or /setres 1366x768.");
			return true;
		}

		setPendingSelection(self, false);
		showCommandMessage(self, "wkSetResCustom: resolution request sent for " + ResolutionManager::formatResolution(appliedResolution));
		return true;
	}
}

int Chat::onChatInput(DWORD self, const std::string &msg, int a3) {
	(void)self;
	(void)a3;

	if (msg.empty()) {
		return 0;
	}

	std::string message = trimAndCollapseWhitespace(msg);
	if (message.empty()) {
		return 0;
	}

	bool hasSlash = false;
	if (!message.empty() && message.front() == '/') {
		hasSlash = true;
		message.erase(0, 1);
	}

	std::transform(message.begin(), message.end(), message.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});

	auto parts = splitWords(message);
	if (parts.empty()) {
		return 0;
	}

	size_t zeroBasedIndex = 0;
	if (parts.size() == 1 && tryParsePositiveIndex(parts[0], &zeroBasedIndex) && isPendingSelection(self)) {
		Diagnostics::log("lobby pending selection: %s", parts[0].c_str());
		DisplayResolution appliedResolution{};
		std::string errorMessage;
		if (!ResolutionManager::applyResolutionByIndex(zeroBasedIndex, &appliedResolution, &errorMessage, self)) {
			showCommandMessage(self, "wkSetResCustom: " + errorMessage);
			clearLobbyInputText(self);
			return 1;
		}

		setPendingSelection(self, false);
		showCommandMessage(self, "wkSetResCustom: resolution request sent for " + ResolutionManager::formatResolution(appliedResolution));
		clearLobbyInputText(self);
		return 1;
	}

	if (parts[0] == "checkres") {
		Diagnostics::log("lobby command: /checkres");
		if (handleCheckRes(self)) {
			clearLobbyInputText(self);
			return 1;
		}
		return 0;
	}
	if (parts[0] == "setres") {
		Diagnostics::log("lobby command: %ssetres", hasSlash ? "/" : "");
		if (handleSetRes(self, parts)) {
			clearLobbyInputText(self);
			return 1;
		}
		return 0;
	}

	return 0;
}

int __fastcall Chat::hookLobbyClientCommands(void *self, void *edx, char **commandStrPtr, char **argStrPtr) {
	(void)edx;

	const char *commandText = (commandStrPtr != nullptr && commandStrPtr[0] != nullptr) ? commandStrPtr[0] : "";
	const char *argText = (argStrPtr != nullptr && argStrPtr[0] != nullptr) ? argStrPtr[0] : "";
	std::string message = buildCommandMessage(commandText, argText);
	Diagnostics::log(
		"lobby client command seen: self=0x%X raw=\"%s\"",
		static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)),
		escapeForLog(message).c_str());

	if (onChatInput(static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)), message, 0) != 0) {
		clearLobbyInputText(static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)));
		return 1;
	}

	return origLobbyClientCommands(self, edx, commandStrPtr, argStrPtr);
}

int __fastcall Chat::hookLobbyHostCommands(void *self, void *edx, char **commandStrPtr, char **argStrPtr) {
	(void)edx;

	const char *commandText = (commandStrPtr != nullptr && commandStrPtr[0] != nullptr) ? commandStrPtr[0] : "";
	const char *argText = (argStrPtr != nullptr && argStrPtr[0] != nullptr) ? argStrPtr[0] : "";
	std::string message = buildCommandMessage(commandText, argText);
	Diagnostics::log(
		"lobby host command seen: self=0x%X raw=\"%s\"",
		static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)),
		escapeForLog(message).c_str());

	if (onChatInput(static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)), message, 0) != 0) {
		clearLobbyInputText(static_cast<DWORD>(reinterpret_cast<uintptr_t>(self)));
		return 1;
	}

	return origLobbyHostCommands(self, edx, commandStrPtr, argStrPtr);
}

int __fastcall Chat::hookLobbyCommand(DWORD self, DWORD edx, DWORD a1, DWORD a2) {
	(void)edx;
	(void)a1;
	(void)a2;

	std::string rawMessage;
	if (tryGetLobbyInputText(self, &rawMessage)) {
		std::string normalizedMessage = trimAndCollapseWhitespace(rawMessage);
		if (!normalizedMessage.empty()) {
			Diagnostics::log(
				"lobby input seen: self=0x%X raw=\"%s\" normalized=\"%s\"",
				self,
				escapeForLog(rawMessage).c_str(),
				normalizedMessage.c_str());
		}

		if (onChatInput(self, rawMessage, 0) != 0) {
			return 1;
		}
	}

	return callOriginalLobbyCommandImpl(self, a1, a2);
}

void Chat::callShowChatMessage(const std::string &msg, int color, DWORD resourceContext) {
	DWORD resourceObject = W2App::getAddrDdGame();
	if (resourceObject == 0) {
		resourceObject = resourceContext;
	}
	if (!resourceObject || !addrShowChatMessage) {
		Diagnostics::log(
			"ShowChatMessage skipped: resourceObject=0x%X showChatMessage=0x%X text=%s",
			resourceObject,
			static_cast<DWORD>(reinterpret_cast<uintptr_t>(addrShowChatMessage)),
			msg.c_str());
		return;
	}

	__try {
		Diagnostics::log(
			"ShowChatMessage local display: resourceObject=0x%X color=%d text=%s",
			resourceObject,
			color,
			msg.c_str());
		addrShowChatMessage(resourceObject, color, const_cast<char *>(msg.c_str()), 1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		Diagnostics::log(
			"ShowChatMessage raised an exception: resourceObject=0x%X color=%d text=%s",
			resourceObject,
			color,
			msg.c_str());
	}
}

void Chat::install() {
	DWORD addrLobbyClientCommands = 0;
	DWORD addrLobbyHostCommands = 0;
	DWORD addrLobbyCommand = 0;
	try {
		addrLobbyClientCommands = _ScanPattern(
			"LobbyClientCommands",
			"\x55\x8B\xEC\x83\xE4\xF8\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x40\x53\x56\x8B\x75\x08\x8B\x06\x57\x8B\xD9\x68\x00\x00\x00\x00\x50\x89\x5C\x24\x1C\xE8\x00\x00\x00\x00\x83\xC4\x08\x85\xC0",
			"??????xxx????xx????xxxx????xxxxxxxxxxxxxx????xxxxxx????xxxxx");
		Hooks::hook(
			"LobbyClientCommands",
			addrLobbyClientCommands,
			(DWORD *)&hookLobbyClientCommands,
			(DWORD *)&origLobbyClientCommands,
			__CALLPOSITION__);
	} catch (const std::exception &e) {
		Diagnostics::log("LobbyClientCommands hook skipped: %s", e.what());
	}

	try {
		addrLobbyHostCommands = _ScanPattern(
			"LobbyHostCommands",
			"\x55\x8B\xEC\x83\xE4\xF8\x64\xA1\x00\x00\x00\x00\x6A\xFF\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x81\xEC\x00\x00\x00\x00\x53\x56\x8B\x75\x08\x8B\x06\x57\x68\x00\x00\x00\x00\x50\x8B\xF9\xE8\x00\x00\x00\x00\x83\xC4\x08\x85\xC0\x0F\x84\x00\x00\x00\x00\x8B\x06\x68\x00\x00\x00\x00\x50\xE8\x00\x00\x00\x00",
			"??????xx????xxx????xxxx????xx????xxxxxxxxx????xxxx????xxxxxxx????xxx????xx????");
		Hooks::hook(
			"LobbyHostCommands",
			addrLobbyHostCommands,
			(DWORD *)&hookLobbyHostCommands,
			(DWORD *)&origLobbyHostCommands,
			__CALLPOSITION__);
	} catch (const std::exception &e) {
		Diagnostics::log("LobbyHostCommands hook skipped: %s", e.what());
	}

	try {
		addrLobbyShowText = _ScanPattern(
			"LobbyShowText",
			"\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x51\x56\x57\x8B\xF1\x68\x7C\xA0\x65\x00\x8D\x4C\x24\x0C",
			"xxx????x????xxxx????xxxxxxx????xxxx");
	} catch (const std::exception &e) {
		addrLobbyShowText = 0;
		Diagnostics::log("LobbyShowText scan skipped: %s", e.what());
	}

	try {
		addrShowChatMessage = (void(__stdcall *)(DWORD, int, char *, int))
			_ScanPattern("ShowChatMessage", "\x81\xEC\x00\x00\x00\x00\x53\x55\x8B\xAC\x24\x00\x00\x00\x00\x80\xBD\x00\x00\x00\x00\x00\x8B\x85\x00\x00\x00\x00\x8B\x48\x24\x56\x8B\xB1\x00\x00\x00\x00\x57", "??????xxxxx????xx?????xx????xxxxxx????x");
	} catch (const std::exception &e) {
		addrShowChatMessage = nullptr;
		Diagnostics::log("ShowChatMessage scan skipped: %s", e.what());
	}

	try {
		addrLobbyDisplayMessage = (int(__stdcall *)(int, char *))
			_ScanPattern(
				"LobbyDisplayMessage",
				"\x55\x8B\xEC\x83\xE4\xF8\x64\xA1\x00\x00\x00\x00\x6A\xFF\x68\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x30\x53\x56\x57\xE8\x00\x00\x00\x00\x33\xC9\x85\xC0\x0F\x95\xC1\x85\xC9\x75\x0A\x68\x00\x00\x00\x00\xE8\x00\x00\x00\x00",
				"??????xx????xxx????xxxx????xxxxxxx????xxxxxxxxxxxx????x????");
	} catch (const std::exception &e) {
		addrLobbyDisplayMessage = nullptr;
		Diagnostics::log("LobbyDisplayMessage scan skipped: %s", e.what());
	}

	try {
		addrLobbyCommand = _ScanPattern(
			"LobbyCommand",
			"\x55\x8B\xEC\x83\xE4\xF8\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x64\x89\x25\x00\x00\x00\x00\x83\xEC\x34\x53\x55\x56\x57\x8B\xF9\x6A\x01",
			"xxxxxxxxx????x????xxxx????xxxxxxxxxxx");
		Hooks::hook("LobbyCommand", addrLobbyCommand, (DWORD *)&hookLobbyCommand, (DWORD *)&origLobbyCommand, __CALLPOSITION__);
	} catch (const std::exception &e) {
		addrLobbyCommand = 0;
		Diagnostics::log("LobbyCommand hook skipped: %s", e.what());
	}

	Diagnostics::log(
		"chat installed: lobbyClientCommands=0x%X lobbyHostCommands=0x%X lobbyCommand=0x%X lobbyShowText=0x%X showChatMessage=0x%X lobbyDisplayMessage=0x%X",
		addrLobbyClientCommands,
		addrLobbyHostCommands,
		addrLobbyCommand,
		addrLobbyShowText,
		static_cast<DWORD>(reinterpret_cast<uintptr_t>(addrShowChatMessage)),
		static_cast<DWORD>(reinterpret_cast<uintptr_t>(addrLobbyDisplayMessage)));
}
