#ifndef WKSETRES_RESOLUTIONMANAGER_H
#define WKSETRES_RESOLUTIONMANAGER_H

#include <windows.h>
#include <optional>
#include <string>
#include <vector>

struct DisplayResolution {
	DWORD width = 0;
	DWORD height = 0;

	bool operator==(const DisplayResolution &other) const {
		return width == other.width && height == other.height;
	}
};

class ResolutionManager {
public:
	static void install();
	static void refreshBinding();
	static void onW2AppInitialized();
	static void shutdown();

	static std::vector<DisplayResolution> listAvailableResolutions();
	static std::optional<DisplayResolution> getCurrentResolution();
	static bool tryGetRequestedResolution(DisplayResolution *resolution);
	static bool applyResolutionByIndex(size_t zeroBasedIndex, DisplayResolution *appliedResolution, std::string *errorMessage, DWORD frontendContext = 0);
	static bool applyResolutionValue(const DisplayResolution &resolution, DisplayResolution *appliedResolution, std::string *errorMessage, DWORD frontendContext = 0);
	static std::string formatResolution(const DisplayResolution &resolution);
};

#endif // WKSETRES_RESOLUTIONMANAGER_H
