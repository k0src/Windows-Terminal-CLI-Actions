# Windows Terminal CLI Actions (WTA)

A command-line utility for managing Windows Terminal settings, fonts, profiles, and actions.

## Features

- **Profile Management**: Create and configure Windows Terminal profiles
- **Font Management**: Install Nerd Fonts and set font configurations
- **Color Schemes**: Apply color schemes to profiles
- **Custom Actions**: Add and remove custom actions with keybindings
- **Launch Configuration**: Set Windows Terminal launch modes
- **Elevation Settings**: Configure admin privilege requirements

## Installation

### Compiling From Source:

1. Clone the repository
2. Compile with g++:
   ```bash
   g++ -std=c++17 -o wta main.cpp -lwininet -lgdi32
   ```
3. Add `wta.exe` to your PATH (optional)

## Commands

### Profile Management

#### Create Profile

```bash
wta createprofile <profileName>
```

Creates a new Windows Terminal profile with default settings.

**Example:**

```bash
wta createprofile "PowerShell Dev"
```

#### Set Elevation

```bash
wta elevate <true|false> [profileName]
```

Configure whether a profile should run with administrator privileges.

**Examples:**

```bash
wta elevate true                  # Set default profile to elevated
wta elevate false "PowerShell"    # Disable elevation for PowerShell profile
```

### Font Management

#### Set Font

```bash
wta font <fontName> [fontSize] [profileName]
```

Set the font family and size for a profile.

**Examples:**

```bash
wta font "Cascadia Code" 14                   # Set font for default profile
wta font "JetBrainsMono NF" 12 "PowerShell"   # Set font for specific profile
```

#### Install Font

```bash
wta install-font <fontName|help>
```

Download and install Nerd Fonts from the official repository.

**Examples:**

```bash
wta install-font help               # List available fonts
wta install-font "JetBrainsMono"    # Install JetBrains Mono Nerd Font
```

### Color Schemes

#### Apply Color Scheme

```bash
wta colorscheme <schemeName> [profileName]
```

Apply a color scheme to a profile.

**Examples:**

```bash
wta colorscheme "One Half Dark"                 # Apply to default profile
wta colorscheme "Solarized Dark" "PowerShell"   # Apply to specific profile
```

### Custom Actions

#### Add Action

```bash
wta add-action <command> <id> [keys] [name] [options...]
```

Add custom actions and keybindings to Windows Terminal.

**Options:**

- `--action <action>` - Action type for complex commands
- `--name <name>` - Display name in command palette
- `--icon <icon>` - Icon (file path or emoji)
- `--keys <keys>` - Keyboard shortcut
- `--commandline <cmd>` - Command line for `wt` actions
- `--index <number>` - Profile index for newTab actions
- `--profile <name>` - Profile name for actions
- `--directory <path>` - Starting directory
- `--size <size>` - Pane size for splits
- `--split <direction>` - Split direction

**Examples:**

```bash
# Simple command
wta add-action closeWindow User.MyCloseWindow alt+f4

# New tab with specific profile
wta add-action newTab User.PowerShellTab ctrl+shift+p "PowerShell" --action newTab --profile "PowerShell"

# Complex wt command
wta add-action wt User.DevSetup ctrl+shift+d "Dev Setup" --commandline "new-tab pwsh.exe ; split-pane -p \"Command Prompt\""
```

#### Remove Action

```bash
wta remove-action <actionId>
```

Remove an action and its associated keybindings.

**Example:**

```bash
wta remove-action User.MyCloseWindow
```

### Launch Configuration

#### Set Launch Mode

```bash
wta launch-mode <mode>
```

Configure how Windows Terminal launches.

**Available modes:**

- `default` - Default window mode
- `maximized` - Launch maximized
- `fullscreen` - Launch in fullscreen
- `focus` - Default mode with focus enabled
- `maximizedFocus` - Maximized with focus enabled

**Example:**

```bash
wta launch-mode maximized
```

### General

#### Help

```bash
wta help
```

Display available commands.

## All Commands

| Command         | Description             | Usage                                 |
| --------------- | ----------------------- | ------------------------------------- |
| `help`          | Show available commands | `wta help`                            |
| `createprofile` | Create new profile      | `wta createprofile <name>`            |
| `font`          | Set profile font        | `wta font <font> [size] [profile]`    |
| `install-font`  | Install Nerd Font       | `wta install-font <font>`             |
| `colorscheme`   | Apply color scheme      | `wta colorscheme <scheme> [profile]`  |
| `elevate`       | Set elevation mode      | `wta elevate <true/false> [profile]`  |
| `add-action`    | Add custom action       | `wta add-action <cmd> <id> [options]` |
| `remove-action` | Remove action           | `wta remove-action <id>`              |
| `launch-mode`   | Set launch mode         | `wta launch-mode <mode>`              |

## Requirements

- Windows 10/11
- Windows Terminal installed
- Internet connection (for font downloads and data updates)
- Administrator privileges (for font installation and registry modifications)

## Notes

- Profile names default to "defaults" if not specified
- Font installation requires administrator privileges
- Changes to launch mode take effect on next Windows Terminal launch
- Custom actions support the full Windows Terminal action specification

## Contributing

Feel free to submit issues and requests.

## License

This project is open source.
