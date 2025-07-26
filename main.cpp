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

  bool themeExists(const std::string &themeName) {
    if (!settings.contains("themes")) {
      return false;
    }
    for (const auto &theme : settings["themes"]) {
      if (theme["name"] == themeName) {
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
    commands["theme"] = [this](const std::vector<std::string> &args) { themeCommand(args); };
    commands["create-theme"] = [this](const std::vector<std::string> &args) { createThemeCommand(args); };
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
    commands["icon"] = [this](const std::vector<std::string> &args) { iconCommand(args); };
    commands["tab-title"] = [this](const std::vector<std::string> &args) { tabTitleCommand(args); };
    commands["starting-directory"] = [this](const std::vector<std::string> &args) { startingDirectoryCommand(args); };
    commands["profile-name"] = [this](const std::vector<std::string> &args) { profileNameCommand(args); };
    commands["commandline"] = [this](const std::vector<std::string> &args) { commandlineCommand(args); };
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
    if (args.empty()) {
      std::cout << "Windows Terminal Actions (WTA) - Command Line Tool" << std::endl;
      std::cout << "=================================================" << std::endl;
      std::cout << std::endl;
      std::cout << "Available commands:" << std::endl;
      std::cout << std::endl;

      // Group commands by category for better organization
      std::cout << "Font Management:" << std::endl;
      std::cout << "  font, font-size, font-weight, install-font" << std::endl;
      std::cout << std::endl;

      std::cout << "Color & Themes:" << std::endl;
      std::cout << "  color-scheme, create-scheme, theme, create-theme" << std::endl;
      std::cout << std::endl;

      std::cout << "Cursor Settings:" << std::endl;
      std::cout << "  cursor-shape, cursor-height" << std::endl;
      std::cout << std::endl;

      std::cout << "Profile Management:" << std::endl;
      std::cout << "  create-profile, elevate, icon, tab-title, starting-directory" << std::endl;
      std::cout << "  profile-name, commandline" << std::endl;
      std::cout << std::endl;

      std::cout << "Terminal Behavior:" << std::endl;
      std::cout << "  copy-on-select, copy-formatting, trim-block-selection, trim-paste" << std::endl;
      std::cout << "  word-delimiters, snap-to-grid, focus-follow-mouse, detect-urls" << std::endl;
      std::cout << "  large-paste-warning, multiline-paste-warning" << std::endl;
      std::cout << std::endl;

      std::cout << "Window & Tab Settings:" << std::endl;
      std::cout << "  launch-mode, always-show-tabs, new-tab-position, show-tabs-in-titlebar" << std::endl;
      std::cout << "  use-acrylic-in-tab-row, show-terminal-title-in-titlebar, always-on-top" << std::endl;
      std::cout << "  tab-width-mode, disable-animations, confirm-close-all-tabs" << std::endl;
      std::cout << "  auto-hide-window, minimize-to-notification, always-show-notification" << std::endl;
      std::cout << std::endl;

      std::cout << "Advanced Features:" << std::endl;
      std::cout << "  tab-switcher-mode, use-tab-switcher, enable-unfocused-acrylic" << std::endl;
      std::cout << "  force-full-repaint, software-rendering, force-vt" << std::endl;
      std::cout << "  right-click-context-menu, search-web-url, language" << std::endl;
      std::cout << std::endl;

      std::cout << "Actions & Keybindings:" << std::endl;
      std::cout << "  add-action, remove-action" << std::endl;
      std::cout << std::endl;

      std::cout << "Use 'wta help <command>' for detailed information about a specific command." << std::endl;
      std::cout << "Example: wta help font" << std::endl;
    } else {
      std::string commandName = args[0];
      showDetailedHelp(commandName);
    }
  }

  void showDetailedHelp(const std::string &commandName) {
    if (commands.find(commandName) == commands.end()) {
      std::cerr << "Unknown command: " << commandName << std::endl;
      std::cout << "Use 'wta help' to see all available commands." << std::endl;
      return;
    }

    std::cout << "Command: " << commandName << std::endl;
    std::cout << std::string(commandName.length() + 9, '=') << std::endl;
    std::cout << std::endl;

    if (commandName == "font") {
      std::cout << "Usage: wta font <fontName> [fontSize] [weight] [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the font face, size, and weight for a terminal profile." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  fontName    - Name of the font (must be installed)" << std::endl;
      std::cout << "  fontSize    - Font size (optional, positive integer)" << std::endl;
      std::cout << "  weight      - Font weight (optional, see font-weight command)" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta font \"Cascadia Code\"" << std::endl;
      std::cout << "  wta font \"Fira Code\" 14 bold PowerShell" << std::endl;
    }
    else if (commandName == "font-size") {
      std::cout << "Usage: wta font-size <fontSize> [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the font size for a terminal profile." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  fontSize    - Font size (positive integer)" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta font-size 12" << std::endl;
      std::cout << "  wta font-size 16 PowerShell" << std::endl;
    }
    else if (commandName == "font-weight") {
      std::cout << "Usage: wta font-weight <weight> [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the font weight for a terminal profile." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  weight      - Font weight (see valid values below)" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Valid weights:" << std::endl;
      std::cout << "  normal, thin, extra-light, light, semi-light, medium," << std::endl;
      std::cout << "  semi-bold, bold, extra-bold, black, extra-black" << std::endl;
      std::cout << "  OR integer value from 1-1000" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta font-weight bold" << std::endl;
      std::cout << "  wta font-weight 600 PowerShell" << std::endl;
    }
    else if (commandName == "install-font") {
      std::cout << "Usage: wta install-font <fontName|help>" << std::endl;
      std::cout << std::endl;
      std::cout << "Downloads and installs Nerd Fonts from the internet." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  fontName - Name of the Nerd Font to install" << std::endl;
      std::cout << "  help     - Shows list of available fonts" << std::endl;
      std::cout << std::endl;
      std::cout << "Note: Font installation may require administrator privileges." << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta install-font help" << std::endl;
      std::cout << "  wta install-font \"Fira Code\"" << std::endl;
    }
    else if (commandName == "color-scheme") {
      std::cout << "Usage: wta color-scheme <schemeName> [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Applies a color scheme to a terminal profile." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  schemeName  - Name of the color scheme" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta color-scheme \"One Half Dark\"" << std::endl;
      std::cout << "  wta color-scheme Solarized PowerShell" << std::endl;
    }
    else if (commandName == "create-scheme") {
      std::cout << "Usage: wta create-scheme" << std::endl;
      std::cout << std::endl;
      std::cout << "Interactively creates a new color scheme with custom colors." << std::endl;
      std::cout << std::endl;
      std::cout << "This command will prompt you to enter:" << std::endl;
      std::cout << "  - Scheme name" << std::endl;
      std::cout << "  - Background, foreground, cursor, and selection colors" << std::endl;
      std::cout << "  - 16 terminal colors (black, red, green, yellow, blue, purple, cyan, white)" << std::endl;
      std::cout << "  - Bright variants of the 8 basic colors" << std::endl;
      std::cout << std::endl;
      std::cout << "Colors should be in hex format (#FFFFFF or FFFFFF)." << std::endl;
    }
    else if (commandName == "theme") {
      std::cout << "Usage: wta theme <themeName>" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the Windows Terminal theme." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  themeName - Name of the theme" << std::endl;
      std::cout << std::endl;
      std::cout << "Built-in themes: system, dark, light" << std::endl;
      std::cout << "Custom themes can be created with 'wta create-theme'" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta theme dark" << std::endl;
      std::cout << "  wta theme \"My Custom Theme\"" << std::endl;
    }
    else if (commandName == "create-theme") {
      std::cout << "Usage: wta create-theme" << std::endl;
      std::cout << std::endl;
      std::cout << "Interactively creates a new Windows Terminal theme." << std::endl;
      std::cout << std::endl;
      std::cout << "This command will prompt you to configure:" << std::endl;
      std::cout << "  - Theme name" << std::endl;
      std::cout << "  - Window settings (application theme, Mica effect, rainbow frame)" << std::endl;
      std::cout << "  - Tab row appearance (background colors)" << std::endl;
      std::cout << "  - Tab appearance (background colors, close button behavior)" << std::endl;
    }
    else if (commandName == "cursor-shape") {
      std::cout << "Usage: wta cursor-shape <cursorShape> [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the cursor shape for a terminal profile." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  cursorShape - Shape of the cursor" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Valid cursor shapes:" << std::endl;
      std::cout << "  bar, vintage, underscore, filledBox, emptyBox, doubleUnderscore" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta cursor-shape bar" << std::endl;
      std::cout << "  wta cursor-shape filledBox PowerShell" << std::endl;
    }
    else if (commandName == "cursor-height") {
      std::cout << "Usage: wta cursor-height <height> [profileName]" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets the cursor height for vintage cursor shapes." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  height      - Height percentage (1-100)" << std::endl;
      std::cout << "  profileName - Target profile (optional, defaults to 'defaults')" << std::endl;
      std::cout << std::endl;
      std::cout << "Note: This setting only affects the 'vintage' cursor shape." << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta cursor-height 25" << std::endl;
      std::cout << "  wta cursor-height 50 PowerShell" << std::endl;
    }
    else if (commandName == "launch-mode") {
      std::cout << "Usage: wta launch-mode <mode>" << std::endl;
      std::cout << std::endl;
      std::cout << "Sets how Windows Terminal launches." << std::endl;
      std::cout << std::endl;
      std::cout << "Available modes:" << std::endl;
      std::cout << "  default         - Launch in default window mode" << std::endl;
      std::cout << "  maximized       - Launch maximized" << std::endl;
      std::cout << "  fullscreen      - Launch in fullscreen mode" << std::endl;
      std::cout << "  focus           - Launch in default mode with focus enabled" << std::endl;
      std::cout << "  maximizedFocus  - Launch maximized with focus enabled" << std::endl;
      std::cout << std::endl;
      std::cout << "Note: Changes take effect when Windows Terminal is restarted." << std::endl;
    }
    else if (commandName == "create-profile") {
      std::cout << "Usage: wta create-profile <profileName>" << std::endl;
      std::cout << std::endl;
      std::cout << "Creates a new terminal profile with default settings." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  profileName - Name for the new profile" << std::endl;
      std::cout << std::endl;
      std::cout << "The new profile will use default settings that can be" << std::endl;
      std::cout << "customized with other WTA commands." << std::endl;
    }
    else if (commandName == "add-action") {
      std::cout << "Usage: wta add-action <command> <id> [keys] [name] [options...]" << std::endl;
      std::cout << std::endl;
      std::cout << "Adds a custom action with optional keybinding to Windows Terminal." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  command - The action command to execute" << std::endl;
      std::cout << "  id      - Unique identifier for the action" << std::endl;
      std::cout << "  keys    - Key combination (optional)" << std::endl;
      std::cout << "  name    - Display name (optional)" << std::endl;
      std::cout << std::endl;
      std::cout << "Additional options:" << std::endl;
      std::cout << "  --action <action>        - Specific action type" << std::endl;
      std::cout << "  --commandline <command>  - Command line to execute" << std::endl;
      std::cout << "  --profile <profile>      - Target profile" << std::endl;
      std::cout << "  --directory <path>       - Starting directory" << std::endl;
      std::cout << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta add-action closeWindow User.MyClose alt+f4" << std::endl;
      std::cout << "  wta add-action newTab User.PowerShell ctrl+shift+p --profile PowerShell" << std::endl;
    }
    else if (commandName == "remove-action") {
      std::cout << "Usage: wta remove-action <actionId>" << std::endl;
      std::cout << std::endl;
      std::cout << "Removes a custom action and its associated keybindings." << std::endl;
      std::cout << std::endl;
      std::cout << "Parameters:" << std::endl;
      std::cout << "  actionId - The unique identifier of the action to remove" << std::endl;
    }
    else {
      std::cout << "This command modifies Windows Terminal settings." << std::endl;
      std::cout << std::endl;
      std::cout << "For usage information, run the command without arguments" << std::endl;
      std::cout << "to see the specific syntax and available options." << std::endl;
      std::cout << std::endl;
      std::cout << "Example: wta " << commandName << std::endl;
    }
    
    std::cout << std::endl;
    std::cout << "Use 'wta help' to see all available commands." << std::endl;
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

  void languageCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta language <language-tag>" << std::endl;
      std::cout << "Sets the application's preferred language override." << std::endl;
      std::cout << "Example: wta language \"en-US\" or wta language \"fr-FR\"" << std::endl;
      return;
    }

    std::string language = args[0];
    json &settings = fileManager.getSettings();
    settings["language"] = language;
    fileManager.writeSettings();
    std::cout << "Language set to " << language << " successfully." << std::endl;
  }

  void themeCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta theme <themeName>" << std::endl;
      std::cout << "Built-in themes: system, dark, light" << std::endl;
      std::cout << "Use 'wta create-theme' to create a custom theme." << std::endl;
      return;
    }

    std::string themeName = args[0];

    if (themeName != "system" && themeName != "dark" && themeName != "light") {
      if (!fileManager.themeExists(themeName)) {
        std::cerr << "Theme not found: " << themeName << std::endl;
        std::cout << "Available built-in themes: system, dark, light" << std::endl;
        std::cout << "Use 'wta create-theme' to create a custom theme." << std::endl;
        return;
      }
    }

    json &settings = fileManager.getSettings();
    settings["theme"] = themeName;
    fileManager.writeSettings();
    std::cout << "Theme set to '" << themeName << "' successfully." << std::endl;
  }

  void createThemeCommand(const std::vector<std::string> &args) {
    if (args.size() > 0) {
      std::cerr << "Usage: wta create-theme" << std::endl;
      return;
    }

    std::cout << "Creating a new theme..." << std::endl;

    std::string themeName;
    std::cout << "Enter theme name: ";
    std::getline(std::cin, themeName);
    
    if (themeName.empty()) {
      std::cerr << "Error: Theme name cannot be empty." << std::endl;
      return;
    }

    if (themeName == "system" || themeName == "dark" || themeName == "light") {
      std::cerr << "Error: Theme names 'system', 'dark', and 'light' are reserved." << std::endl;
      return;
    }

    if (fileManager.themeExists(themeName)) {
      std::cerr << "Error: Theme '" << themeName << "' already exists." << std::endl;
      return;
    }

    json newTheme;
    newTheme["name"] = themeName;

    std::cout << "Configure theme properties (press Enter to skip optional settings):" << std::endl;
    std::cout << std::endl;

    // Window settings
    std::cout << "=== Window Settings ===" << std::endl;
    std::string applicationTheme;
    std::cout << "Application theme (system/dark/light) [default: dark]: ";
    std::getline(std::cin, applicationTheme);
    if (applicationTheme.empty()) applicationTheme = "dark";
    
    std::string useMica;
    std::cout << "Use Mica effect (true/false) [default: false]: ";
    std::getline(std::cin, useMica);
    if (useMica.empty()) useMica = "false";

    std::string rainbowFrame;
    std::cout << "Rainbow frame (experimental, true/false) [default: false]: ";
    std::getline(std::cin, rainbowFrame);
    if (rainbowFrame.empty()) rainbowFrame = "false";

    newTheme["window"]["applicationTheme"] = applicationTheme;
    newTheme["window"]["useMica"] = (useMica == "true");
    newTheme["window"]["experimental.rainbowFrame"] = (rainbowFrame == "true");

    // Tab row settings
    std::cout << std::endl;
    std::cout << "=== Tab Row Settings ===" << std::endl;
    std::string tabRowBackground;
    std::cout << "Tab row background color (hex color or 'terminalBackground') [default: #333333FF]: ";
    std::getline(std::cin, tabRowBackground);
    if (tabRowBackground.empty()) tabRowBackground = "#333333FF";

    std::string tabRowUnfocusedBackground;
    std::cout << "Tab row unfocused background color [default: #333333FF]: ";
    std::getline(std::cin, tabRowUnfocusedBackground);
    if (tabRowUnfocusedBackground.empty()) tabRowUnfocusedBackground = "#333333FF";

    newTheme["tabRow"]["background"] = tabRowBackground;
    newTheme["tabRow"]["unfocusedBackground"] = tabRowUnfocusedBackground;

    // Tab settings
    std::cout << std::endl;
    std::cout << "=== Tab Settings ===" << std::endl;
    std::string tabBackground;
    std::cout << "Active tab background ('terminalBackground' or hex color) [default: terminalBackground]: ";
    std::getline(std::cin, tabBackground);
    if (tabBackground.empty()) tabBackground = "terminalBackground";

    std::string tabUnfocusedBackground;
    std::cout << "Inactive tab background [default: #00000000]: ";
    std::getline(std::cin, tabUnfocusedBackground);
    if (tabUnfocusedBackground.empty()) tabUnfocusedBackground = "#00000000";

    std::string showCloseButton;
    std::cout << "Show close button (always/hover/never/activeOnly) [default: always]: ";
    std::getline(std::cin, showCloseButton);
    if (showCloseButton.empty()) showCloseButton = "always";

    newTheme["tab"]["background"] = tabBackground;
    newTheme["tab"]["unfocusedBackground"] = tabUnfocusedBackground;
    newTheme["tab"]["showCloseButton"] = showCloseButton;

    json &settings = fileManager.getSettings();
    
    if (!settings.contains("themes")) {
      settings["themes"] = json::array();
    }
    
    settings["themes"].push_back(newTheme);
    fileManager.writeSettings();
    
    std::cout << std::endl;
    std::cout << "Theme '" << themeName << "' created successfully." << std::endl;
    std::cout << "You can now use it with: wta theme " << themeName << std::endl;
  }

  void alwaysShowTabsCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta always-show-tabs <true|false>" << std::endl;
      std::cout << "When true, tabs are always displayed." << std::endl;
      std::cout << "Note: Changing this setting requires starting a new terminal instance." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["alwaysShowTabs"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Always show tabs set to " << option << " successfully." << std::endl;
    std::cout << "Note: You need to restart Windows Terminal for this change to take effect." << std::endl;
  }

  void newTabPositionCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta new-tab-position <afterLastTab|afterCurrentTab>" << std::endl;
      std::cout << "Specifies where new tabs appear in the tab row." << std::endl;
      return;
    }

    std::string position = args[0];
    if (position != "afterLastTab" && position != "afterCurrentTab") {
      std::cerr << "Invalid option: " << position << ". Use 'afterLastTab' or 'afterCurrentTab'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["newTabPosition"] = position;
    fileManager.writeSettings();
    std::cout << "New tab position set to " << position << " successfully." << std::endl;
  }

  void showTabsInTitlebarCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta show-tabs-in-titlebar <true|false>" << std::endl;
      std::cout << "When true, tabs are moved into the title bar and the title bar disappears." << std::endl;
      std::cout << "Note: Changing this setting requires starting a new terminal instance." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["showTabsInTitlebar"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Show tabs in titlebar set to " << option << " successfully." << std::endl;
    std::cout << "Note: You need to restart Windows Terminal for this change to take effect." << std::endl;
  }

  void useAcrylicInTabRowCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta use-acrylic-in-tab-row <true|false>" << std::endl;
      std::cout << "When true, the tab row is given an acrylic background at 50% opacity." << std::endl;
      std::cout << "Note: Changing this setting requires starting a new terminal instance." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["useAcrylicInTabRow"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Use acrylic in tab row set to " << option << " successfully." << std::endl;
    std::cout << "Note: You need to restart Windows Terminal for this change to take effect." << std::endl;
  }

  void showTerminalTitleInTitlebarCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta show-terminal-title-in-titlebar <true|false>" << std::endl;
      std::cout << "When true, the title bar displays the title of the selected tab." << std::endl;
      std::cout << "Note: Changing this setting requires starting a new terminal instance." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["showTerminalTitleInTitlebar"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Show terminal title in titlebar set to " << option << " successfully." << std::endl;
    std::cout << "Note: You need to restart Windows Terminal for this change to take effect." << std::endl;
  }

  void alwaysOnTopCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta always-on-top <true|false>" << std::endl;
      std::cout << "When true, Windows Terminal windows will launch on top of all other windows." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["alwaysOnTop"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Always on top set to " << option << " successfully." << std::endl;
  }

  void tabWidthModeCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta tab-width-mode <equal|titleLength|compact>" << std::endl;
      std::cout << "Sets the width of the tabs." << std::endl;
      std::cout << "  equal - each tab the same width" << std::endl;
      std::cout << "  titleLength - each tab sized to the length of its title" << std::endl;
      std::cout << "  compact - inactive tabs shrink to icon width, active tab gets more space" << std::endl;
      return;
    }

    std::string mode = args[0];
    if (mode != "equal" && mode != "titleLength" && mode != "compact") {
      std::cerr << "Invalid option: " << mode << ". Use 'equal', 'titleLength', or 'compact'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["tabWidthMode"] = mode;
    fileManager.writeSettings();
    std::cout << "Tab width mode set to " << mode << " successfully." << std::endl;
  }

  void disableAnimationsCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta disable-animations <true|false>" << std::endl;
      std::cout << "When true, disables visual animations across the application." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["disableAnimations"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Disable animations set to " << option << " successfully." << std::endl;
  }

  void confirmCloseAllTabsCommand(const std::vector<std::string> &args) {
    if (args.size() != 1) {
      std::cerr << "Usage: wta confirm-close-all-tabs <true|false>" << std::endl;
      std::cout << "When true, closing a window with multiple tabs open will require confirmation." << std::endl;
      return;
    }

    std::string option = args[0];
    if (option != "true" && option != "false") {
      std::cerr << "Invalid option: " << option << ". Use 'true' or 'false'." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["confirmCloseAllTabs"] = (option == "true");
    fileManager.writeSettings();
    std::cout << "Confirm close all tabs set to " << option << " successfully." << std::endl;
  }

  void iconCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta icon <icon-path-or-emoji> [profileName]" << std::endl;
      std::cout << "Sets the icon that displays within the tab, dropdown menu, jumplist, and tab switcher." << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta icon \"🐧\" defaults" << std::endl;
      std::cout << "  wta icon \"ms-appdata:///roaming/ubuntu.ico\" Ubuntu" << std::endl;
      std::cout << "  wta icon \"C:\\\\path\\\\to\\\\icon.ico\"" << std::endl;
      return;
    }

    std::string icon = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["icon"] = icon;
    fileManager.writeSettings();
    std::cout << "Icon set successfully for profile '" << profileName << "'." << std::endl;
  }

  void tabTitleCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta tab-title <title|null> [profileName]" << std::endl;
      std::cout << "Sets the title to pass to the shell on startup. Use 'null' to unset." << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta tab-title \"My PowerShell\" defaults" << std::endl;
      std::cout << "  wta tab-title null Ubuntu" << std::endl;
      return;
    }

    std::string title = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    if (title == "null") {
      settings["profiles"][profileName]["tabTitle"] = nullptr;
      std::cout << "Tab title unset for profile '" << profileName << "'." << std::endl;
    } else {
      settings["profiles"][profileName]["tabTitle"] = title;
      std::cout << "Tab title set to '" << title << "' for profile '" << profileName << "'." << std::endl;
    }
    fileManager.writeSettings();
  }

  void startingDirectoryCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta starting-directory <directory-path|null> [profileName]" << std::endl;
      std::cout << "Sets the directory the shell starts in when it is loaded. Use 'null' to unset." << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta starting-directory \"%USERPROFILE%\\\\Documents\" defaults" << std::endl;
      std::cout << "  wta starting-directory \"C:\\\\Projects\" PowerShell" << std::endl;
      std::cout << "  wta starting-directory null Ubuntu" << std::endl;
      return;
    }

    std::string directory = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    if (directory == "null") {
      settings["profiles"][profileName]["startingDirectory"] = nullptr;
      std::cout << "Starting directory unset for profile '" << profileName << "'." << std::endl;
    } else {
      settings["profiles"][profileName]["startingDirectory"] = directory;
      std::cout << "Starting directory set to '" << directory << "' for profile '" << profileName << "'." << std::endl;
    }
    fileManager.writeSettings();
  }

  void profileNameCommand(const std::vector<std::string> &args) {
    if (args.size() != 2) {
      std::cerr << "Usage: wta profile-name <new-name> <current-profile-name>" << std::endl;
      std::cout << "Sets the name of the profile that will be displayed in the dropdown menu." << std::endl;
      std::cout << "Note: This command requires specifying the current profile name to identify which profile to rename." << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta profile-name \"My PowerShell\" \"Windows PowerShell\"" << std::endl;
      std::cout << "  wta profile-name \"Dev Command Prompt\" \"Command Prompt\"" << std::endl;
      return;
    }

    std::string newName = args[0];
    std::string currentProfileName = args[1];

    if (currentProfileName == "defaults") {
      std::cerr << "Cannot rename the 'defaults' profile. The 'defaults' profile contains default settings for all profiles." << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    bool profileFound = false;
    
    for (auto &profile : settings["profiles"]["list"]) {
      if (profile.contains("name") && profile["name"] == currentProfileName) {
        profile["name"] = newName;
        profileFound = true;
        break;
      }
    }

    if (!profileFound) {
      std::cerr << "Profile '" << currentProfileName << "' not found in profile list." << std::endl;
      return;
    }

    fileManager.writeSettings();
    std::cout << "Profile renamed from '" << currentProfileName << "' to '" << newName << "' successfully." << std::endl;
  }

  void commandlineCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta commandline <executable-command> [profileName]" << std::endl;
      std::cout << "Sets the executable used in the profile." << std::endl;
      std::cout << "Examples:" << std::endl;
      std::cout << "  wta commandline \"powershell.exe\" defaults" << std::endl;
      std::cout << "  wta commandline \"cmd.exe /k path\\\\to\\\\script.bat\" \"Command Prompt\"" << std::endl;
      std::cout << "  wta commandline \"wsl.exe -d Ubuntu\" Ubuntu" << std::endl;
      return;
    }

    std::string commandline = args[0];
    std::string profileName = args.size() == 2 ? args[1] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    json &settings = fileManager.getSettings();
    settings["profiles"][profileName]["commandline"] = commandline;
    fileManager.writeSettings();
    std::cout << "Command line set to '" << commandline << "' for profile '" << profileName << "'." << std::endl;
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
