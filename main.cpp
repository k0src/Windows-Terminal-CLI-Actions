#include "json.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <wingdi.h>
#include <wininet.h>
#include <winreg.h>

using json = nlohmann::json;

class FontManager;
class FileDownloader;
class FontInstaller;
class WTAFileManager;
class WTACommandManager;

int CALLBACK EnumFontFamExProc(ENUMLOGFONTEX *lpelfe, NEWTEXTMETRICEX *lpntme,
                               DWORD FontType, LPARAM lParam) {
  std::string *targetFont = reinterpret_cast<std::string *>(lParam);
  std::string fontName =
      reinterpret_cast<char *>(lpelfe->elfLogFont.lfFaceName);

  std::transform(fontName.begin(), fontName.end(), fontName.begin(), ::tolower);
  std::string targetLower = *targetFont;
  std::transform(targetLower.begin(), targetLower.end(), targetLower.begin(),
                 ::tolower);

  if (fontName == targetLower) {
    return 0;
  }
  return 1;
}

class FileDownloader {
public:
  bool downloadFile(const std::string &url, const std::string &filePath) {
    HINTERNET hInternet =
        InternetOpenA("FontDownloader", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet)
      return false;

    HINTERNET hFile = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0,
                                       INTERNET_FLAG_RELOAD, 0);
    if (!hFile) {
      InternetCloseHandle(hInternet);
      return false;
    }

    std::ofstream outFile(filePath, std::ios::binary);
    if (!outFile) {
      InternetCloseHandle(hFile);
      InternetCloseHandle(hInternet);
      return false;
    }

    char buffer[4096];
    DWORD bytesRead;
    while (InternetReadFile(hFile, buffer, sizeof(buffer), &bytesRead) &&
           bytesRead > 0) {
      outFile.write(buffer, bytesRead);
    }

    outFile.close();
    InternetCloseHandle(hFile);
    InternetCloseHandle(hInternet);
    return true;
  }
};

class FontInstaller {
public:
  bool extractAndInstallFonts(const std::string &zipPath, const std::string &fontName) {
    std::string tempDir = std::getenv("TEMP");
    tempDir += "\\" + fontName + "_fonts";

    if (!createTempDirectory(tempDir)) {
      return false;
    }

    if (!extractArchive(zipPath, tempDir)) {
      std::filesystem::remove_all(tempDir);
      return false;
    }

    std::cout << "Note: Font installation may require administrator privileges." << std::endl;

    bool fontsInstalled = installFontsFromDirectory(tempDir);

    std::filesystem::remove_all(tempDir);
    std::filesystem::remove(zipPath);

    return fontsInstalled;
  }

private:
  bool createTempDirectory(const std::string &tempDir) {
    try {
      std::filesystem::create_directories(tempDir);
      return true;
    } catch (...) {
      std::cerr << "Error: Failed to create temporary directory." << std::endl;
      return false;
    }
  }

  bool extractArchive(const std::string &zipPath, const std::string &tempDir) {
    std::string command = "powershell -Command \"Expand-Archive -Path '" +
                          zipPath + "' -DestinationPath '" + tempDir +
                          "' -Force\"";
    if (system(command.c_str()) != 0) {
      std::cerr << "Error: Failed to extract font archive." << std::endl;
      return false;
    }
    return true;
  }

  bool installFontsFromDirectory(const std::string &tempDir) {
    bool fontsInstalled = false;
    
    for (const auto &entry : std::filesystem::recursive_directory_iterator(tempDir)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (ext == ".ttf" || ext == ".otf") {
          if (installSingleFont(entry.path().string())) {
            fontsInstalled = true;
          }
        }
      }
    }
    
    return fontsInstalled;
  }

  bool installSingleFont(const std::string &fontFile) {
    std::string fontsDir = getWindowsFontsDirectory();
    if (fontsDir.empty()) {
      return false;
    }

    std::string destPath = fontsDir + "\\" + std::filesystem::path(fontFile).filename().string();
    
    if (CopyFileA(fontFile.c_str(), destPath.c_str(), FALSE)) {
      registerFontInRegistry(fontFile);
      return true;
    } else {
      std::cerr << "Error: Could not copy font file to " << destPath << std::endl;
      return false;
    }
  }

  std::string getWindowsFontsDirectory() {
    char fontsPath[MAX_PATH];
    if (GetWindowsDirectoryA(fontsPath, MAX_PATH) == 0) {
      std::cerr << "Error: Could not get Windows directory." << std::endl;
      return "";
    }
    return std::string(fontsPath) + "\\Fonts";
  }

  void registerFontInRegistry(const std::string &fontFile) {
    std::string fontName = std::filesystem::path(fontFile).stem().string();
    std::string fileName = std::filesystem::path(fontFile).filename().string();
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, 
                     "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
                     0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
      RegSetValueExA(hKey, fontName.c_str(), 0, REG_SZ,
                    (const BYTE*)fileName.c_str(), fileName.length() + 1);
      RegCloseKey(hKey);
      std::cout << "Installed font: " << fontName << std::endl;
    } else {
      std::cout << "Warning: Could not register font in registry (requires admin rights)" << std::endl;
    }
  }
};

class FontManager {
private:
  const std::string FONT_DATA_URL = "https://raw.githubusercontent.com/k0src/Windows-Terminal-CLI-Actions/992ac7b89305645fe6c972cbffde7592c8f32b73/font_data.json";
  FileDownloader downloader;

public:
  bool fontExists(const std::string &fontName) {
    HDC hdc = GetDC(NULL);
    if (!hdc)
      return false;

    LOGFONT lf = {0};
    lf.lfCharSet = DEFAULT_CHARSET;

    std::string targetFont = fontName;
    int result = EnumFontFamiliesEx(hdc, &lf, (FONTENUMPROC)EnumFontFamExProc,
                                    reinterpret_cast<LPARAM>(&targetFont), 0);

    ReleaseDC(NULL, hdc);
    return (result == 0);
  }

  json readFontData() {
    std::string tempPath = std::getenv("TEMP");
    std::string fontDataPath = tempPath + "\\font_data.json";
    
    if (downloader.downloadFile(FONT_DATA_URL, fontDataPath)) {
      std::ifstream file(fontDataPath);
      if (file) {
        try {
          json data;
          file >> data;
          file.close();
          std::filesystem::remove(fontDataPath);
          return data;
        } catch (...) {
          std::cerr << "Error: Failed to parse font data." << std::endl;
          file.close();
          std::filesystem::remove(fontDataPath);
        }
      }
    }
  }

  bool findFontInData(const json &fontData, const std::string &fontName, std::string &fontUrl) {
    for (const auto &font : fontData) {
      if (font["Name"].get<std::string>() == fontName) {
        fontUrl = font["URL"].get<std::string>();
        return true;
      }
    }
    return false;
  }
};

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
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (!localAppData) {
      std::cerr << "Could not get LOCALAPPDATA environment variable." << std::endl;
      std::exit(1);
    }

    std::filesystem::path path =
        std::filesystem::path(localAppData) /
        "Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState/settings.json";

    if (!std::filesystem::exists(path)) {
      std::cerr << "settings.json does not exist. Make sure Windows Terminal is installed."
                << std::endl;
      std::exit(1);
    }

    return path.string();
  }

  json readSettings(const std::string &path) {
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

  json &getSettings() { return settings; }

  bool profileExists(const std::string &profileName) {
    return settings["profiles"].contains(profileName);
  }

  bool colorSchemeExists(const std::string &schemeName) {
    for (const auto &scheme : settings["schemes"]) {
      if (scheme["name"] == schemeName) {
        return true;
      }
    }
    return false;
  }

  bool profileNameExists(const std::string &profileName) {
    for (const auto &profile : settings["profiles"]["list"]) {
      if (profile["name"] == profileName) {
        return true;
      }
    }
    return false;
  }

  bool actionIdExists(const std::string &actionId) {
    if (!settings.contains("actions")) {
      return false;
    }
    
    for (const auto &action : settings["actions"]) {
      if (action.contains("id") && action["id"] == actionId) {
        return true;
      }
    }
    return false;
  }

  void addAction(const json &newAction) {
    if (!settings.contains("actions")) {
      settings["actions"] = json::array();
    }
    settings["actions"].push_back(newAction);
  }

  void addKeybinding(const json &newKeybinding) {
    if (!settings.contains("keybindings")) {
      settings["keybindings"] = json::array();
    }
    settings["keybindings"].push_back(newKeybinding);
  }

  bool removeAction(const std::string &actionId) {
    if (!settings.contains("actions")) {
      return false;
    }
    
    auto &actions = settings["actions"];
    bool removed = false;
    
    for (auto it = actions.begin(); it != actions.end(); ++it) {
      if (it->contains("id") && (*it)["id"] == actionId) {
        actions.erase(it);
        removed = true;
        break;
      }
    }
    
    return removed;
  }

  int removeKeybindingsForAction(const std::string &actionId) {
    if (!settings.contains("keybindings")) {
      return 0;
    }
    
    auto &keybindings = settings["keybindings"];
    int removedCount = 0;
    
    for (auto it = keybindings.begin(); it != keybindings.end();) {
      if (it->contains("id") && (*it)["id"] == actionId) {
        it = keybindings.erase(it);
        removedCount++;
      } else {
        ++it;
      }
    }
    
    return removedCount;
  }
};

class WTACommandManager {
private:
  WTAFileManager fileManager;
  FontManager fontManager;
  FileDownloader downloader;
  FontInstaller installer;
  std::unordered_map<std::string, std::function<void(const std::vector<std::string> &args)>> commands;

public:
  WTACommandManager() {
    registerCommands();
  }

  void executeCommand(const std::string &command, const std::vector<std::string> &args) {
    auto it = commands.find(command);
    if (it != commands.end()) {
      it->second(args);
    } else {
      std::cerr << "Command not found: " << command << std::endl;
    }
  }

private:
  void registerCommands() {
    commands["help"] = [this](const std::vector<std::string> &args) { helpCommand(args); };
    commands["color-scheme"] = [this](const std::vector<std::string> &args) { colorSchemeCommand(args); };
    commands["create-scheme"] = [this](const std::vector<std::string> &args) { createSchemeCommand(args); };
    commands["font"] = [this](const std::vector<std::string> &args) { fontCommand(args); };
    commands["font-size"] = [this](const std::vector<std::string> &args) { fontSizeCommand(args); };
    commands["font-weight"] = [this](const std::vector<std::string> &args) { fontWeightCommand(args); };
    commands["cursor-shape"] = [this](const std::vector<std::string> &args) { cursorShapeCommand(args); };
    commands["cursor-height"] = [this](const std::vector<std::string> &args) { cursorHeightCommand(args); };
    commands["force-full-repaint"] = [this](const std::vector<std::string> &args) { forceFullRepaintCommand(args); };
    commands["software-rendering"] = [this](const std::vector<std::string> &args) { softwareRenderingCommand(args); };
    commands["create-profile"] = [this](const std::vector<std::string> &args) { createProfileCommand(args); };
    commands["elevate"] = [this](const std::vector<std::string> &args) { elevateCommand(args); };
    commands["install-font"] = [this](const std::vector<std::string> &args) { fontInstallCommand(args); };
    commands["add-action"] = [this](const std::vector<std::string> &args) { addActionCommand(args); };
    commands["remove-action"] = [this](const std::vector<std::string> &args) { removeActionCommand(args); };
    commands["launch-mode"] = [this](const std::vector<std::string> &args) { launchModeCommand(args); };
    commands["enable-unfocused-acrylic"] = [this](const std::vector<std::string> &args) { enableUnfocusedAcrylicCommand(args); };
    commands["copy-on-select"] = [this](const std::vector<std::string> &args) { copyOnSelectCommand(args); };
    commands["copy-formatting"] = [this](const std::vector<std::string> &args) { copyFormattingCommand(args); };
    commands["trim-block-selection"] = [this](const std::vector<std::string> &args) { trimBlockSelectionCommand(args); };
    commands["trim-paste"] = [this](const std::vector<std::string> &args) { trimPasteCommand(args); };
    commands["word-delimiters"] = [this](const std::vector<std::string> &args) { wordDelimitersCommand(args); };
    commands["snap-to-grid"] = [this](const std::vector<std::string> &args) { snapToGridCommand(args); };
    commands["minimize-to-notification"] = [this](const std::vector<std::string> &args) { minimizeToNotificationCommand(args); };
    commands["always-show-notification"] = [this](const std::vector<std::string> &args) { alwaysShowNotificationCommand(args); };
    commands["tab-switcher-mode"] = [this](const std::vector<std::string> &args) { tabSwitcherModeCommand(args); };
    commands["use-tab-switcher"] = [this](const std::vector<std::string> &args) { useTabSwitcherCommand(args); };
    commands["auto-hide-window"] = [this](const std::vector<std::string> &args) { autoHideWindowCommand(args); };
    commands["focus-follow-mouse"] = [this](const std::vector<std::string> &args) { focusFollowMouseCommand(args); };
    commands["detect-urls"] = [this](const std::vector<std::string> &args) { detectUrlsCommand(args); };
    commands["large-paste-warning"] = [this](const std::vector<std::string> &args) { largePasteWarningCommand(args); };
    commands["multiline-paste-warning"] = [this](const std::vector<std::string> &args) { multiLinePasteWarningCommand(args); };
    commands["force-vt"] = [this](const std::vector<std::string> &args) { forceVtCommand(args); };
    commands["right-click-context-menu"] = [this](const std::vector<std::string> &args) { rightClickContextMenuCommand(args); };
    commands["search-web-url"] = [this](const std::vector<std::string> &args) { searchWebUrlCommand(args); };
    commands["test"] = [this](const std::vector<std::string> &args) { testCommand(args); };
    commands["language"] = [this](const std::vector<std::string> &args) { languageCommand(args); };
    commands["theme"] = [this](const std::vector<std::string> &args) { themeCommand(args); };
    commands["always-show-tabs"] = [this](const std::vector<std::string> &args) { alwaysShowTabsCommand(args); };
    commands["new-tab-position"] = [this](const std::vector<std::string> &args) { newTabPositionCommand(args); };
    commands["show-tabs-in-titlebar"] = [this](const std::vector<std::string> &args) { showTabsInTitlebarCommand(args); };
    commands["use-acrylic-in-tab-row"] = [this](const std::vector<std::string> &args) { useAcrylicInTabRowCommand(args); };
    commands["show-terminal-title-in-titlebar"] = [this](const std::vector<std::string> &args) { showTerminalTitleInTitlebarCommand(args); };
    commands["always-on-top"] = [this](const std::vector<std::string> &args) { alwaysOnTopCommand(args); };
    commands["tab-width-mode"] = [this](const std::vector<std::string> &args) { tabWidthModeCommand(args); };
    commands["disable-animations"] = [this](const std::vector<std::string> &args) { disableAnimationsCommand(args); };
    commands["confirm-close-all-tabs"] = [this](const std::vector<std::string> &args) { confirmCloseAllTabsCommand(args); };
  }

  bool validateAndSetFontWeight(const std::string &weight, json &settings, const std::string &profileName) {
    std::vector<std::string> validWeights = {
      "normal", "thin", "extra-light", "light", "semi-light", 
      "medium", "semi-bold", "bold", "extra-bold", "black", "extra-black"
    };
    
    bool isValidWeight = false;
    for (const auto& validWeight : validWeights) {
      if (weight == validWeight) {
        isValidWeight = true;
        break;
      }
    }
    
    if (isValidWeight) {
      settings["profiles"][profileName]["font"]["weight"] = weight;
      return true;
    } else {
      try {
        int weightValue = std::stoi(weight);
        if (weightValue >= 1 && weightValue <= 1000) {
          settings["profiles"][profileName]["font"]["weight"] = weightValue;
          return true;
        } else {
          std::cerr << "Invalid font weight: " << weight << ". Must be 1-1000 or a valid weight name." << std::endl;
          return false;
        }
      } catch (...) {
        std::cerr << "Invalid font weight: " << weight << std::endl;
        std::cout << "Valid weights: normal, thin, extra-light, light, semi-light, medium, semi-bold, bold, extra-bold, black, extra-black, or integer 1-1000" << std::endl;
        return false;
      }
    }
  }

  bool isValidHexColor(const std::string &color) {
    std::string hexColor = color;
    
    if (hexColor.length() > 0 && hexColor[0] == '#') {
      hexColor = hexColor.substr(1);
    }
    
    if (hexColor.length() != 6) {
      return false;
    }
  
    for (char c : hexColor) {
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
        return false;
      }
    }
    
    return true;
  }

  std::string normalizeHexColor(const std::string &color) {
    std::string hexColor = color;
    
    if (hexColor.length() > 0 && hexColor[0] == '#') {
      hexColor = hexColor.substr(1);
    }
    
    std::transform(hexColor.begin(), hexColor.end(), hexColor.begin(), ::toupper);
    return "#" + hexColor;
  }

  std::string promptForHexColor(const std::string &fieldName) {
    std::string input;
    
    while (true) {
      std::cout << fieldName << ": ";
      std::getline(std::cin, input);
      
      if (isValidHexColor(input)) {
        return normalizeHexColor(input);
      } else {
        std::cerr << "Invalid hex color format. Please use 6-character hex format (e.g., #FFFFFF or FFFFFF)." << std::endl;
      }
    }
  }

  void helpCommand(const std::vector<std::string> &args) {
    std::cout << "Commands:" << std::endl;
    for (const auto &cmd : commands) {
      std::cout << " - " << cmd.first << std::endl;
    }
  }

  void colorSchemeCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta color-scheme <schemeName> [profileName]" << std::endl;
      return;
    }

    std::string schemeName = args[0];
    std::string profileName = (args.size() == 2) ? args[1] : "defaults";

    if (!fileManager.colorSchemeExists(schemeName)) {
      std::cerr << "Color scheme not found: " << schemeName << std::endl;
      return;
    }

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["colorScheme"] = schemeName;
    fileManager.writeSettings();
    std::cout << "Color scheme updated successfully." << std::endl;
  }

  void createSchemeCommand(const std::vector<std::string> &args) {
    if (args.size() > 0) {
      std::cerr << "Usage: wta create-scheme" << std::endl;
      return;
    }

    std::cout << "Creating a new color scheme..." << std::endl;

    std::string schemeName;
    std::cout << "Enter scheme name: ";
    std::getline(std::cin, schemeName);
    
    if (schemeName.empty()) {
      std::cerr << "Error: Scheme name cannot be empty." << std::endl;
      return;
    }

    if (fileManager.colorSchemeExists(schemeName)) {
      std::cerr << "Error: Color scheme '" << schemeName << "' already exists." << std::endl;
      return;
    }

    json newScheme;
    newScheme["name"] = schemeName;

    std::vector<std::string> colorFields = {
      "background", "foreground", "cursorColor", "selectionBackground",
      "black", "red", "green", "yellow", "blue", "purple", "cyan", "white",
      "brightBlack", "brightRed", "brightGreen", "brightYellow", 
      "brightBlue", "brightPurple", "brightCyan", "brightWhite"
    };

    std::cout << "Enter hex colors in format #FFFFFF or FFFFFF." << std::endl;
    std::cout << std::endl;

    for (const std::string &field : colorFields) {
      std::string color = promptForHexColor(field);
      newScheme[field] = color;
    }

    json &settings = fileManager.getSettings();
    
    if (!settings.contains("schemes")) {
      settings["schemes"] = json::array();
    }
    
    settings["schemes"].push_back(newScheme);
    fileManager.writeSettings();
    
    std::cout << std::endl;
    std::cout << "Color scheme '" << schemeName << "' created successfully." << std::endl;
    std::cout << "You can now use it with: wta color-scheme " << schemeName << " [profileName]" << std::endl;
  }

  void fontCommand(const std::vector<std::string> &args) {
    if (args.empty() || args.size() > 4) {
      std::cerr << "Usage: wta font <fontName> [fontSize] [weight] [profileName]" << std::endl;
      return;
    }

    std::string fontName = args[0];
    std::string fontSize = args.size() >= 2 ? args[1] : "";
    std::string weight = args.size() >= 3 ? args[2] : "";
    std::string profileName = args.size() == 4 ? args[3] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    if (!fontManager.fontExists(fontName)) {
      std::cerr << "Font not found on system: " << fontName << std::endl;
      std::cout << "Use: wta install-font <fontName> to install a font." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["font"]["face"] = fontName;
    
    if (!fontSize.empty()) {
      try {
        settings["profiles"][profileName]["font"]["size"] = std::stoi(fontSize);
      } catch (...) {
        std::cerr << "Invalid font size: " << fontSize << std::endl;
        return;
      }
    }

    if (!weight.empty()) {
      if (!validateAndSetFontWeight(weight, settings, profileName)) {
        return;
      }
    }

    fileManager.writeSettings();
    std::cout << "Font updated successfully." << std::endl;
  }

  void fontSizeCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta font-size <fontSize> [profileName]" << std::endl;
      return;
    }

    std::string fontSize = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    try {
      int fontSizeValue = std::stoi(fontSize);
      if (fontSizeValue < 1) {
        std::cerr << "Invalid font size: " << fontSize << ". Must be a positive integer." << std::endl;
        return;
      }

      json &settings = fileManager.getSettings();
      settings["profiles"][profileName]["font"]["size"] = fontSizeValue;
      fileManager.writeSettings();
      std::cout << "Font size updated successfully." << std::endl;
    } catch (...) {
      std::cerr << "Invalid font size: " << fontSize << ". Must be a positive integer." << std::endl;
    }
  }

  void fontWeightCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta font-weight <weight> [profileName]" << std::endl;
      std::cout << "Valid weights: normal, thin, extra-light, light, semi-light, medium, semi-bold, bold, extra-bold, black, extra-black, or integer 1-1000" << std::endl;
      return;
    }

    std::string weight = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    
    if (validateAndSetFontWeight(weight, settings, profileName)) {
      fileManager.writeSettings();
      std::cout << "Font weight updated successfully." << std::endl;
    }
  }

  void cursorShapeCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta cursor-shape <cursorShape> [profileName]" << std::endl;
      std::cout << "Valid cursor shapes: bar, vintage, underscore, filledBox, emptyBox, doubleUnderscore" << std::endl;
      return;
    }

    std::string cursorShape = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    std::vector<std::string> validCursorShapes = {
      "bar", "vintage", "underscore", "filledBox", "emptyBox", "doubleUnderscore"
    };
    
    bool isValidShape = false;
    for (const auto& validShape : validCursorShapes) {
      if (cursorShape == validShape) {
        isValidShape = true;
        break;
      }
    }
    
    if (!isValidShape) {
      std::cerr << "Invalid cursor shape: " << cursorShape << std::endl;
      std::cout << "Valid cursor shapes: bar, vintage, underscore, filledBox, emptyBox, doubleUnderscore" << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["cursorShape"] = cursorShape;
    fileManager.writeSettings();
    std::cout << "Cursor shape updated successfully." << std::endl;
  }

  void cursorHeightCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta cursor-height <height> [profileName]" << std::endl;
      std::cout << "Height must be an integer from 1-100 (only works with vintage cursor shape)" << std::endl;
      return;
    }

    std::string heightStr = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    try {
      int height = std::stoi(heightStr);
      if (height < 1 || height > 100) {
        std::cerr << "Invalid cursor height: " << heightStr << ". Must be between 1 and 100." << std::endl;
        return;
      }

      json &settings = fileManager.getSettings();
      settings["profiles"][profileName]["cursorHeight"] = height;
      fileManager.writeSettings();
      std::cout << "Cursor height updated successfully." << std::endl;
      
      if (settings["profiles"][profileName].contains("cursorShape") && 
          settings["profiles"][profileName]["cursorShape"] != "vintage") {
        std::cout << "Note: Cursor height only affects the 'vintage' cursor shape." << std::endl;
      }
    } catch (...) {
      std::cerr << "Invalid cursor height: " << heightStr << ". Must be an integer between 1 and 100." << std::endl;
    }
  }

  void forceFullRepaintCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta force-full-repaint <true|false> [profileName]" << std::endl;
      std::cout << "Controls whether the terminal redraws the entire screen each frame." << std::endl;
      return;
    }

    std::string option = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["experimental.rendering.forceFullRepaint"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Force full repaint set to " << option << " successfully." << std::endl;
  }

  void softwareRenderingCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta software-rendering <true|false> [profileName]" << std::endl;
      std::cout << "Controls whether the terminal uses software rendering (WARP) instead of hardware rendering." << std::endl;
      return;
    }

    std::string option = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["experimental.rendering.software"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Software rendering set to " << option << " successfully." << std::endl;
  }

  void fontInstallCommand(const std::vector<std::string> &args) {
    if (args.empty()) {
      std::cerr << "Usage: wta install-font <fontName|help>" << std::endl;
      std::cout << "Use: wta install-font help to see a list of available Nerd Fonts." << std::endl;
      return;
    }

    std::string fontArg = args[0];

    if (fontArg == "help") {
      displayAvailableFonts();
      return;
    }

    if (fontManager.fontExists(fontArg)) {
      std::cerr << "Font '" << fontArg << "' is already installed on the system." << std::endl;
      std::cout << "Use: wta font <fontName> to set it for a profile." << std::endl;
      return;
    }

    installFont(fontArg);
  }

  void displayAvailableFonts() {
    json fontData = fontManager.readFontData();
    std::cout << "Available Nerd Fonts:" << std::endl;
    for (const auto &font : fontData) {
      std::cout << font["Name"].get<std::string>() << std::endl;
    }
  }

  void installFont(const std::string &fontName) {
    json fontData = fontManager.readFontData();
    std::string fontUrl;
    
    if (!fontManager.findFontInData(fontData, fontName, fontUrl)) {
      std::cerr << "Error: Font '" << fontName << "' not found in available fonts." << std::endl;
      std::cout << "Use: wta font-install help to see available fonts." << std::endl;
      return;
    }

    std::cout << "Downloading " << fontName << " font..." << std::endl;

    std::string tempPath = std::getenv("TEMP");
    std::string zipPath = tempPath + "\\" + fontName + ".zip";

    if (!downloader.downloadFile(fontUrl, zipPath)) {
      std::cerr << "Error: Failed to download font from " << fontUrl << std::endl;
      return;
    }

    std::cout << "Installing " << fontName << " font..." << std::endl;

    if (installer.extractAndInstallFonts(zipPath, fontName)) {
      std::cout << "Font '" << fontName << "' installed successfully!" << std::endl;
      std::cout << "You may need to restart applications to see the new font." << std::endl;
    } else {
      std::cerr << "Error: Failed to install font." << std::endl;
    }
  }

  void createProfileCommand(const std::vector<std::string> &args) {
    if (args.size() < 1) {
      std::cerr << "Usage: wta create-profile <profileName>" << std::endl;
      return;
    }

    std::string profileName = args[0];

    if (fileManager.profileNameExists(profileName)) {
      std::cerr << "Profile already exists: " << profileName << std::endl;
      return;
    }

    json newProfile = {
      {"name", profileName},
      {"commandline", "cmd.exe"},
      {"colorScheme", "Campbell"},
      {"fontFace", "Consolas"},
      {"fontSize", 12}
    };

    json &settings = fileManager.getSettings();
    settings["profiles"]["list"].push_back(newProfile);
    fileManager.writeSettings();

    std::cout << "Profile created successfully." << std::endl;
  }

  void elevateCommand(const std::vector<std::string> &args) {
    if (args.size() < 1) {
      std::cerr << "Usage: wta elevate <true | false> [profileName]" << std::endl;
      return;
    }

    std::string elevateOption = args[0];
    std::string profileName = (args.size() == 2) ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    if (elevateOption != "true" && elevateOption != "false") {
      std::cerr << "Invalid option for elevate: " << elevateOption << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["elevate"] = (elevateOption == "true");
    fileManager.writeSettings();
  }

  void addActionCommand(const std::vector<std::string> &args) {
    if (args.size() < 2) {
      std::cerr << "Usage: wta add-action <command> <id> [keys] [name] [icon] [action] [arguments...]" << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  Simple command: wta add-action closeWindow User.MyCloseWindow alt+f4" << std::endl;
      std::cout << "  With action: wta add-action newTab User.MyNewTab ctrl+shift+1 \"New Tab\" --action newTab --index 0" << std::endl;
      std::cout << "  With commandline: wta add-action wt User.MyCommand ctrl+shift+o \"My Setup\" --commandline \"new-tab pwsh.exe\"" << std::endl;
      return;
    }

    std::string command = args[0];
    std::string id = args[1];

    if (fileManager.actionIdExists(id)) {
      std::cerr << "Error: Action ID '" << id << "' already exists." << std::endl;
      return;
    }

    json newAction;
    newAction["id"] = id;

    std::string keys = "";
    std::string name = "";
    std::string icon = "";
    std::string action = "";
    json commandObj;
    bool hasComplexCommand = false;

    for (size_t i = 2; i < args.size(); i++) {
      if (args[i] == "--action" && i + 1 < args.size()) {
        action = args[++i];
        hasComplexCommand = true;
      } else if (args[i] == "--name" && i + 1 < args.size()) {
        name = args[++i];
      } else if (args[i] == "--icon" && i + 1 < args.size()) {
        icon = args[++i];
      } else if (args[i] == "--keys" && i + 1 < args.size()) {
        keys = args[++i];
      } else if (args[i] == "--commandline" && i + 1 < args.size()) {
        commandObj["action"] = command;
        commandObj["commandline"] = args[++i];
        hasComplexCommand = true;
      } else if (args[i] == "--index" && i + 1 < args.size()) {
        try {
          commandObj["index"] = std::stoi(args[++i]);
          hasComplexCommand = true;
        } catch (...) {
          std::cerr << "Error: Invalid index value." << std::endl;
          return;
        }
      } else if (args[i] == "--profile" && i + 1 < args.size()) {
        commandObj["profile"] = args[++i];
        hasComplexCommand = true;
      } else if (args[i] == "--directory" && i + 1 < args.size()) {
        commandObj["startingDirectory"] = args[++i];
        hasComplexCommand = true;
      } else if (args[i] == "--size" && i + 1 < args.size()) {
        commandObj["size"] = args[++i];
        hasComplexCommand = true;
      } else if (args[i] == "--split" && i + 1 < args.size()) {
        commandObj["split"] = args[++i];
        hasComplexCommand = true;
      } else if (i == 2 && args[i].find("+") != std::string::npos) {
        keys = args[i];
      } else if (i == 3 && keys != "" && name == "") {
        name = args[i];
      } else if (i == 4 && name != "" && icon == "") {
        icon = args[i];
      }
    }

    if (hasComplexCommand) {
      if (action != "") {
        commandObj["action"] = action;
      } else if (commandObj.find("action") == commandObj.end()) {
        commandObj["action"] = command;
      }
      newAction["command"] = commandObj;
    } else {
      newAction["command"] = command;
    }

    if (!name.empty()) {
      newAction["name"] = name;
    }
    if (!icon.empty()) {
      newAction["icon"] = icon;
    }

    fileManager.addAction(newAction);

    if (!keys.empty()) {
      json keybinding;
      keybinding["keys"] = keys;
      keybinding["id"] = id;
      fileManager.addKeybinding(keybinding);
    }

    fileManager.writeSettings();

    std::cout << "Action '" << id << "' added successfully." << std::endl;
    if (!keys.empty()) {
      std::cout << "Keybinding '" << keys << "' assigned to action." << std::endl;
    }
  }

  void removeActionCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta remove-action <actionId>" << std::endl;
      std::cout << "Example: wta remove-action User.MyCloseWindow" << std::endl;
      return;
    }

    std::string actionId = args[0];

    if (!fileManager.actionIdExists(actionId)) {
      std::cerr << "Error: Action ID '" << actionId << "' not found." << std::endl;
      return;
    }

    bool actionRemoved = fileManager.removeAction(actionId);
    int keybindingsRemoved = fileManager.removeKeybindingsForAction(actionId);

    if (actionRemoved) {
      fileManager.writeSettings();
      std::cout << "Action '" << actionId << "' removed successfully." << std::endl;
      
      if (keybindingsRemoved > 0) {
        std::cout << "Removed " << keybindingsRemoved << " associated keybinding(s)." << std::endl;
      }
    } else {
      std::cerr << "Error: Failed to remove action '" << actionId << "'." << std::endl;
    }
  }

  void launchModeCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta launch-mode <mode>" << std::endl;
      std::cout << "Available modes:" << std::endl;
      std::cout << "  default         - Launch in default window mode" << std::endl;
      std::cout << "  maximized       - Launch maximized" << std::endl;
      std::cout << "  fullscreen      - Launch in fullscreen mode" << std::endl;
      std::cout << "  focus           - Launch in default mode with focus enabled" << std::endl;
      std::cout << "  maximizedFocus  - Launch maximized with focus enabled" << std::endl;
      std::cout << "Example: wta launch-mode maximized" << std::endl;
      return;
    }

    std::string mode = args[0];
    
    if (mode != "default" && mode != "maximized" && mode != "fullscreen" && 
        mode != "focus" && mode != "maximizedFocus") {
      std::cerr << "Error: Invalid launch mode '" << mode << "'." << std::endl;
      std::cout << "Valid modes: default, maximized, fullscreen, focus, maximizedFocus" << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    
    settings["launchMode"] = mode;
    
    fileManager.writeSettings();
    
    std::cout << "Launch mode set to '" << mode << "' successfully." << std::endl;
    std::cout << "Note: Changes will take effect the next time Windows Terminal is launched." << std::endl;
  }

  void enableUnfocusedAcrylicCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta enable-unfocused-acrylic <true|false>" << std::endl;
      std::cout << "Controls if unfocused acrylic is possible. When true, unfocused windows can have acrylic instead of opaque." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["compatibility.enableUnfocusedAcrylic"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Enable unfocused acrylic set to " << option << " successfully." << std::endl;
  }

  void copyOnSelectCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta copy-on-select <true|false>" << std::endl;
      std::cout << "When true, a selection is immediately copied to clipboard upon creation." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["copyOnSelect"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Copy on select set to " << option << " successfully." << std::endl;
  }

  void copyFormattingCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta copy-formatting <true|false|all|none|html|rtf>" << std::endl;
      std::cout << "Controls whether color and font formatting is copied along with text." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false" && option != "all" && option != "none" && option != "html" && option != "rtf") {
      std::cerr << "Invalid option: " << option << ". Use 'true', 'false', 'all', 'none', 'html', or 'rtf'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    if (option == "true" || option == "false") {
      settings["copyFormatting"] = (option == "true");
    } else {
      settings["copyFormatting"] = option;
    }
    fileManager.writeSettings();
    std::cout << "Copy formatting set to " << option << " successfully." << std::endl;
  }

  void trimBlockSelectionCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta trim-block-selection <true|false>" << std::endl;
      std::cout << "When true, trailing white-space is removed from each line in rectangular selection." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["trimBlockSelection"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Trim block selection set to " << option << " successfully." << std::endl;
  }

  void trimPasteCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta trim-paste <true|false>" << std::endl;
      std::cout << "When enabled, automatically trims trailing whitespace when pasting text." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["trimPaste"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Trim paste set to " << option << " successfully." << std::endl;
  }

  void wordDelimitersCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta word-delimiters <delimiters>" << std::endl;
      std::cout << "Sets the word delimiters used in double-click selection." << std::endl;
      std::cout << "Default: \" /\\\\()\\\"'-:,.;<>~!@#$%^&*|+=[]{}?│\"" << std::endl;
      std::cout << "Note: Escape \\ and \" characters with backslash." << std::endl;
      return;
    }

    std::string delimiters = args[0];
    json &settings = fileManager.getSettings();
    settings["wordDelimiters"] = delimiters;
    fileManager.writeSettings();
    std::cout << "Word delimiters set successfully." << std::endl;
  }

  void snapToGridCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta snap-to-grid <true|false>" << std::endl;
      std::cout << "When true, window will snap to nearest character boundary on resize." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["snapToGridOnResize"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Snap to grid on resize set to " << option << " successfully." << std::endl;
  }

  void minimizeToNotificationCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta minimize-to-notification <true|false>" << std::endl;
      std::cout << "When true, minimizing will hide window from taskbar and show in notification area." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["minimizeToNotificationArea"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Minimize to notification area set to " << option << " successfully." << std::endl;
  }

  void alwaysShowNotificationCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta always-show-notification <true|false>" << std::endl;
      std::cout << "When true, terminal will always place its icon in the notification area." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["alwaysShowNotificationIcon"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Always show notification icon set to " << option << " successfully." << std::endl;
  }

  void tabSwitcherModeCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta tab-switcher-mode <true|false|mru|inOrder|disabled>" << std::endl;
      std::cout << "Controls tab switcher interface style and behavior." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false" && option != "mru" && option != "inOrder" && option != "disabled") {
      std::cerr << "Invalid option: " << option << ". Use 'true', 'false', 'mru', 'inOrder', or 'disabled'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    if (option == "true" || option == "false") {
      settings["tabSwitcherMode"] = (option == "true");
    } else {
      settings["tabSwitcherMode"] = option;
    }
    fileManager.writeSettings();
    std::cout << "Tab switcher mode set to " << option << " successfully." << std::endl;
  }

  void useTabSwitcherCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta use-tab-switcher <true|false>" << std::endl;
      std::cout << "When true, nextTab and prevTab commands will use the tab switcher UI." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["useTabSwitcher"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Use tab switcher set to " << option << " successfully." << std::endl;
  }

  void autoHideWindowCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta auto-hide-window <true|false>" << std::endl;
      std::cout << "When enabled, terminal window will automatically hide when it loses focus." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["autoHideWindow"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Auto hide window set to " << option << " successfully." << std::endl;
  }

  void focusFollowMouseCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta focus-follow-mouse <true|false>" << std::endl;
      std::cout << "When true, terminal will move focus to pane on mouse hover." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["focusFollowMouse"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Focus follow mouse set to " << option << " successfully." << std::endl;
  }

  void detectUrlsCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta detect-urls <true|false>" << std::endl;
      std::cout << "When true, URLs will be detected and made clickable (experimental feature)." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["experimental.detectURLs"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Detect URLs set to " << option << " successfully." << std::endl;
  }

  void largePasteWarningCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta large-paste-warning <true|false>" << std::endl;
      std::cout << "When true, warns when pasting text larger than 5 KiB." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["largePasteWarning"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Large paste warning set to " << option << " successfully." << std::endl;
  }

  void multiLinePasteWarningCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta multiline-paste-warning <true|false>" << std::endl;
      std::cout << "When true, warns when pasting text with multiple lines." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["multiLinePasteWarning"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Multi-line paste warning set to " << option << " successfully." << std::endl;
  }

  void forceVtCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta force-vt <true|false>" << std::endl;
      std::cout << "Forces terminal to use legacy input encoding (experimental feature)." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["experimental.input.forceVT"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Force VT input set to " << option << " successfully." << std::endl;
  }

  void rightClickContextMenuCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta right-click-context-menu <true|false>" << std::endl;
      std::cout << "When true, right-click activates context menu instead of paste." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["experimental.rightClickContextMenu"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Right-click context menu set to " << option << " successfully." << std::endl;
  }

  void searchWebUrlCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta search-web-url <url>" << std::endl;
      std::cout << "Sets the default URL for web searching. Use %s as placeholder for selected text." << std::endl;
      std::cout << "Example: wta search-web-url \"https://www.google.com/search?q=%s\"" << std::endl;
      return;
    }

    std::string url = args[0];
    json &settings = fileManager.getSettings();
    settings["searchWebDefaultQueryUrl"] = url;
    fileManager.writeSettings();
    std::cout << "Search web URL set successfully." << std::endl;
  }
};

int main(int argc, char *argv[]) {
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
