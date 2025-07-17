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

class FontManager {
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
    std::ifstream file("font_data.json");
    if (!file) {
      std::cerr << "Error: font_data.json not found." << std::endl;
      std::exit(1);
    }

    try {
      json data;
      file >> data;
      return data;
    } catch (...) {
      std::cerr << "Error: Failed to parse font_data.json." << std::endl;
      std::exit(1);
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
    commands["colorscheme"] = [this](const std::vector<std::string> &args) { colorSchemeCommand(args); };
    commands["font"] = [this](const std::vector<std::string> &args) { fontCommand(args); };
    commands["createprofile"] = [this](const std::vector<std::string> &args) { createProfileCommand(args); };
    commands["elevate"] = [this](const std::vector<std::string> &args) { elevateCommand(args); };
    commands["font-install"] = [this](const std::vector<std::string> &args) { fontInstallCommand(args); };
  }

  void helpCommand(const std::vector<std::string> &args) {
    std::cout << "Commands:" << std::endl;
    for (const auto &cmd : commands) {
      std::cout << " - " << cmd.first << std::endl;
    }
  }

  void colorSchemeCommand(const std::vector<std::string> &args) {
    if (args.size() < 1 || args.size() > 2) {
      std::cerr << "Usage: wta colorscheme <schemeName> [profileName]" << std::endl;
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

  void fontCommand(const std::vector<std::string> &args) {
    if (args.empty() || args.size() > 3) {
      std::cerr << "Usage: wta font <fontName> [fontSize] [profileName]" << std::endl;
      return;
    }

    std::string fontName = args[0];
    std::string fontSize = args.size() >= 2 ? args[1] : "";
    std::string profileName = args.size() == 3 ? args[2] : "defaults";

    if (!fileManager.profileExists(profileName)) {
      std::cerr << "Profile not found: " << profileName << std::endl;
      return;
    }

    if (!fontManager.fontExists(fontName)) {
      std::cerr << "Font not found on system: " << fontName << std::endl;
      std::cout << "Use: wta font-install <fontName> to install a font." << std::endl;
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

    fileManager.writeSettings();
    std::cout << "Font updated successfully." << std::endl;
  }

  void fontInstallCommand(const std::vector<std::string> &args) {
    if (args.empty()) {
      std::cerr << "Usage: wta font-install <fontName|help>" << std::endl;
      std::cout << "Use: wta font-install help to see a list of available Nerd Fonts." << std::endl;
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
      std::cerr << "Usage: wta createprofile <profileName>" << std::endl;
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
