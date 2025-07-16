#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <functional>
#include "json.hpp"

using json = nlohmann::json;

class WTAFileManager {
private:
    json settings;
    std::string settingsPath;

public:
    WTAFileManager() {
        settingsPath = getSettingsPath();
        settings = readSettings(settingsPath);
    }

    std::string getSettingsPath() {
        const char* localAppData = std::getenv("LOCALAPPDATA");
        if (!localAppData) {
            std::cerr << "Could not get LOCALAPPDATA environment variable." << std::endl;
            std::exit(1);
        }

        std::filesystem::path path = std::filesystem::path(localAppData) /
            "Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json";

        if (!std::filesystem::exists(path)) {
            std::cerr << "settings.json does not exist. Make sure Windows Terminal is installed." << std::endl;
            std::exit(1);
        }

        return path.string();
    }

    json readSettings(const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            std::cerr << "Failed to open settings.json." << std::endl;
            std::exit(1);
        }

        try {
            json data;
            file >> data;
            return data;
        } catch (...) {
            std::cerr << "Failed to parse settings.json." << std::endl;
            std::exit(1);
        }
    }

    void writeSettings() {
        std::ofstream file(settingsPath);
        if (!file) {
            std::cerr << "Failed to write to settings.json." << std::endl;
            std::exit(1);
        }

        try {
            file << std::setw(4) << settings << std::endl;
        } catch (...) {
            std::cerr << "Failed to write settings to settings.json." << std::endl;
            std::exit(1);
        }
    }

    json& getSettings() { return settings; }
};

class WTACommandManager {
private:
    WTAFileManager fileManager;
    std::unordered_map<std::string, std::function<void(const std::vector<std::string>& args)>> commands;

public:
    WTACommandManager() {
        commands["help"] = [this](const std::vector<std::string>& args) { helpCommand(args); };
        commands["colorscheme"] = [this](const std::vector<std::string>& args) { colorSchemeCommand(args); };
        commands["font"] = [this](const std::vector<std::string>& args) { fontCommand(args); };
        commands["createprofile"] = [this](const std::vector<std::string>& args) { createProfileCommand(args); };
    }

    void executeCommand(const std::string& command, const std::vector<std::string>& args) {
        auto it = commands.find(command);
        if (it != commands.end()) {
            it->second(args);
        } else {
            std::cerr << "Command not found: " << command << std::endl;
        }
    }

    void helpCommand(const std::vector<std::string>& args) {
        std::cout << "Commands:" << std::endl;
        for (const auto& cmd : commands) {
            std::cout << " - " << cmd.first << std::endl;
        }
    }

    void colorSchemeCommand(const std::vector<std::string>& args) {
        if (args.size() < 1 || args.size() > 2) {
            std::cerr << "Usage: wta colorscheme <schemeName> [profileName]" << std::endl;
            return;
        }

        std::string schemeName = args[0];
        std::string profileName = (args.size() == 2) ? args[1] : "defaults";

        json& settings = fileManager.getSettings();
        bool schemeExists = false;

        for (const auto& scheme : settings["schemes"]) {
            if (scheme["name"] == schemeName) {
                schemeExists = true;
                break;
            }
        }

        if (!schemeExists) {
            std::cerr << "Color scheme not found: " << schemeName << std::endl;
            return;
        }

        if (settings["profiles"].contains(profileName)) {
            settings["profiles"][profileName]["colorScheme"] = schemeName;
            fileManager.writeSettings();
            std::cout << "Color scheme updated successfully." << std::endl;
        } else {
            std::cerr << "Profile not found: " << profileName << std::endl;
        }
    }

    void fontCommand(const std::vector<std::string>& args) {
        if (args.empty() || args.size() > 3) {
            std::cerr << "Usage: wta font <fontName> [fontSize] [profileName]" << std::endl;
            return;
        }

        std::string fontName = args[0];
        std::string fontSize = args.size() >= 2 ? args[1] : "";
        std::string profileName = args.size() == 3 ? args[2] : "defaults";

        json& settings = fileManager.getSettings();

        if (!settings["profiles"].contains(profileName)) {
            std::cerr << "Profile not found: " << profileName << std::endl;
            return;
        }

        settings["profiles"][profileName]["font"]["face"] = fontName;
        if (!fontSize.empty()) {
            try {
                settings["profiles"][profileName]["font"]["size"] = std::stoi(fontSize);
            } catch (...) {
                std::cerr << "Invalid font size: " << fontSize << std::endl;
                return;
            }
        }

        fileManager.writeSettings();
        std::cout << "Font updated successfully." << std::endl;
    }

    void createProfileCommand(const std::vector<std::string>& args) {
        if (args.size() < 1) {
            std::cerr << "Usage: wta createprofile <profileName>" << std::endl;
            return;
        }

        std::string profileName = args[0];
        json& settings = fileManager.getSettings();

        for (const auto& profile : settings["profiles"]["list"]) {
            if (profile["name"] == profileName) {
                std::cerr << "Profile already exists: " << profileName << std::endl;
                return;
            }
        }

        json newProfile = {
            {"name", profileName},
            {"commandline", "cmd.exe"},
            {"colorScheme", "Campbell"},
            {"fontFace", "Consolas"},
            {"fontSize", 12}
        };

        settings["profiles"]["list"].push_back(newProfile);
        fileManager.writeSettings();

        std::cout << "Profile created successfully." << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: wta [command] [arguments]" << std::endl;
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;

    if (argc > 2) {
        args.assign(argv + 2, argv + argc);
    }

    WTACommandManager commandManager;
    commandManager.executeCommand(command, args);

    return 0;
}
