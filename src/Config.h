#ifndef WKSETRES_CONFIG_H
#define WKSETRES_CONFIG_H

#include <filesystem>
#include <string>

class Config {
public:
	static inline const std::string moduleName = "wkSetResCustom";

private:
	static inline std::filesystem::path waDir;

public:
	static void initialize();
	static int waVersionCheck();
	static const std::filesystem::path &getWaDir();

	static std::string getVersionStr();
	static std::string getBuildStr();
	static std::string getModuleStr();
	static std::string getFullStr();
};

#endif // WKSETRES_CONFIG_H
