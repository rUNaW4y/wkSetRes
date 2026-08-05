#include <windows.h>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include "Chat.h"
#include "Config.h"
#include "Diagnostics.h"
#include "Frontend.h"
#include "RegistryMonitor.h"
#include "ResolutionManager.h"
#include "W2App.h"

namespace {
	void install() {
		W2App::install();
		Frontend::install();
		RegistryMonitor::install();
		ResolutionManager::install();
		Chat::install();
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
	(void)hModule;
	(void)lpReserved;

	switch (ul_reason_for_call) {
		case DLL_PROCESS_ATTACH: {
			auto start = std::chrono::high_resolution_clock::now();
			decltype(start) finish;
			try {
				Config::initialize();
				Diagnostics::initialize(Config::getWaDir());
				Diagnostics::log("process attach");
				if (Config::waVersionCheck()) {
					Diagnostics::log("install start");
					install();
					Diagnostics::log("install done");
				}
				finish = std::chrono::high_resolution_clock::now();
			} catch (std::exception &e) {
				finish = std::chrono::high_resolution_clock::now();
				Diagnostics::log("exception: %s", e.what());
				MessageBoxA(nullptr, e.what(), Config::getFullStr().c_str(), MB_ICONERROR);
			}
			std::chrono::duration<double> elapsed = finish - start;
			printf("wkSetResCustom startup took %lf seconds\n", elapsed.count());
			break;
		}
		case DLL_PROCESS_DETACH:
			ResolutionManager::shutdown();
			break;
		case DLL_THREAD_ATTACH:
		case DLL_THREAD_DETACH:
		default:
			break;
	}
	return TRUE;
}
