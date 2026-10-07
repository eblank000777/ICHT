# Crosshair

A pixel-precise crosshair overlay for Windows 10/11 with a built-in editor. Every crosshair
pixel shows the screen underneath it through a colour effect (invert, hue shift, adaptive
max contrast, ...), so it stays visible on any background.

## Install

Run **`CrosshairSetup.exe`**. It installs for your user only (no admin rights) to
`%LOCALAPPDATA%\Programs\Crosshair`, with optional Start menu / desktop shortcuts and
"start with Windows" (starts hidden in the tray). Uninstall from *Settings > Apps*.

The installer is not code-signed, so Windows SmartScreen may warn the first time:
*More info > Run anyway*.

No install needed either: `crosshair.exe` is portable and keeps its settings next to itself
(or in `%APPDATA%\Crosshair` if that folder is read-only).

## Use

The editor opens on start; closing it keeps the crosshair running in the tray. The header
shows the overlay's live state (fps, hidden, or stalled).

If the crosshair ever lags behind or freezes (for example after a game switches to
fullscreen), press **Refresh overlay** in the header, Ctrl+Alt+U, or use the tray menu: it
recreates the magnifier from scratch. The overlay runs on its own high-priority thread, so
working in the editor does not slow it down.

- **Components**: cross, dot, circle/ring, box, diagonal X; each can draw or erase.
- **Pixels**: left-drag draws, right-drag erases, Shift+click resets, wheel zooms, Ctrl+Z / Ctrl+Y.
- **Presets**: saved in `presets\*.ini`; switch from the editor, tray menu or hotkeys.
- **Effects**: checkboxes, applied top to bottom. *Max contrast* picks the colour furthest
  from what is under each pixel (100% = per colour channel, 0% = black / white).
- **Display**: monitor, update rate, theme (System / Light / Dark).
- **Hotkeys**: click a box and press the keys. Defaults: Ctrl+Alt+H show/hide,
  Ctrl+Alt+E editor, Ctrl+Alt+N / P next / previous preset, Ctrl+Alt+Arrows nudge,
  Ctrl+Alt+S save, Ctrl+Alt+R reload, Ctrl+Alt+U refresh overlay, Ctrl+Alt+Q quit.

`crosshair.exe /tray` starts with the editor hidden.

## Build

| File | Purpose |
|---|---|
| `crosshair.cpp` | the whole program (Win32, no dependencies) |
| `crosshair.rc`, `crosshair.ico` | icon and version info |
| `build.bat` | builds `crosshair.exe` with MSVC or MinGW-w64 |
| `installer\crosshair.nsi` | installer script (NSIS 3) |
| `installer\build_installer.bat` | builds `crosshair.exe`, then `CrosshairSetup.exe` |
| `CrosshairSetup.exe` | prebuilt installer |

Run `build.bat` from an "x64 Native Tools Command Prompt" (MSVC) or with MinGW-w64 `g++` on
PATH. For the installer, install [NSIS](https://nsis.sourceforge.io) and run
`installer\build_installer.bat`.
