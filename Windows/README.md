# DropCode for Windows

DropCode is a drop-down terminal for OpenCode and other interactive CLI tools.
This fork uses native C++ Win32, ConPTY, Direct2D, DirectWrite, and `libvterm`.
WinUI 3 is used only for the settings window.

## Controls

- `Ctrl+Alt+T`: toggle the panel. Hold it for a momentary peek.
- `Ctrl+T`: create a new terminal tab.
- `Ctrl+W`: close the active tab. The final tab stays open.
- `Ctrl+Tab` / `Ctrl+Shift+Tab`: cycle tabs.
- `Ctrl+1` through `Ctrl+9`: select a tab directly. `Ctrl+0` selects the last tab.
- The `+` and `x` controls in the tab strip do the same actions with the mouse.

## Working directories

Open the tray menu and choose **Settings**. Choose a folder under **Working
directory**, then press **Apply & Restart**. That restarts only the active tab;
new tabs use the selected folder and existing inactive tabs keep their own
project.

The launcher leaves an interactive `cmd.exe` session after the configured
agent exits, so manual navigation is also available:

```bat
cd /d C:\path\to\project
dir
opencode
```

## Build

Configure and build the `Windows` CMake project with a Visual Studio x64
developer environment:

```powershell
.\Windows\scripts\build.ps1
.\Windows\scripts\package-release.ps1
```

The first configure restores Windows App SDK 2.3.1 and C++/WinRT into the
build directory, generates the C++/WinRT projection, and copies the bootstrap
DLL beside `DropCode.exe`. This is an unpackaged desktop build, so the Windows
App Runtime 2.3.1 x64 runtime must also be installed on the machine.

The package script creates `Windows/dist/DropCode-v<version>-windows-x64.zip`
and a SHA-256 checksum file.

Download the matching runtime from the [Windows App SDK downloads](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/downloads)
page before launching an unpackaged build.
