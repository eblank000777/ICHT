// crosshair.cpp - pixel-precise colour-effect crosshair overlay + editor for Windows 10/11
//
// How it works: a click-through, topmost window hosts a Magnifier control at 1:1 scale
// that shows the screen content directly underneath it, run through a colour matrix
// (invert, hue shift, adaptive max contrast, ...). The window is clipped (SetWindowRgn) to the exact
// crosshair pixel mask, so every crosshair pixel is a transformed copy of whatever is behind
// it. Refreshes once per compositor frame (DwmFlush), i.e. at your monitor's refresh rate, on
// its own high-priority thread so the editor can never hold it up. "Refresh overlay" (button,
// hotkey or tray menu) rebuilds the magnifier from scratch if it ever freezes or lags.
//
// The crosshair is built from components (cross, dot, circle, box, diagonal X) plus a layer
// of hand-drawn pixel edits on top. Everything is edited live in the editor window, saved as
// presets (presets\*.ini next to the exe) and switched with hotkeys or the tray icon.
//
// Settings and presets live next to the exe, or in %APPDATA%\Crosshair if that folder is not
// writable. The editor has a light and a dark theme (or follows Windows). Start with /tray to
// start with the editor hidden.
//
// Build: run build.bat (MSVC or MinGW-w64; it also compiles the icon from crosshair.rc).
// Manual build without the icon:
//   cl /O2 /EHsc /std:c++17 crosshair.cpp /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib dwmapi.lib winmm.lib
//   g++ -O2 -std=c++17 -municode -mwindows -static crosshair.cpp -o crosshair.exe -ldwmapi -lwinmm -lshell32 -lgdi32 -ladvapi32
// Installer: installer\build_installer.bat (needs NSIS) builds CrosshairSetup.exe.
//
// Default hotkeys (all changeable in the editor):
//   Ctrl+Alt+H       show / hide crosshair       Ctrl+Alt+E   show / hide editor
//   Ctrl+Alt+N / P   next / previous preset      Ctrl+Alt+S   save preset
//   Ctrl+Alt+Arrows  nudge crosshair by 1 px     Ctrl+Alt+R   reload preset from disk
//   Ctrl+Alt+Q       quit
// In the editor: Ctrl+Z undo, Ctrl+Y redo, Ctrl+S save.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10+ (needed for DPI awareness contexts)
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <cmath>
#include <cwchar>
#include <atomic>
#include <mutex>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#endif

namespace fs = std::filesystem;

#define CROSSHAIR_VERSION L"2.2"

// ---------------------------------------------------------------- Magnification API
// Loaded from Magnification.dll at runtime, so no SDK header or import library is needed
// (some MinGW versions ship an incomplete magnification.h).

struct MagTransform   { float v[3][3]; };
struct MagColorEffect { float transform[5][5]; };
static const wchar_t* kMagClass = L"Magnifier";   // = WC_MAGNIFIER
static const DWORD kFilterExclude = 0;            // = MW_FILTERMODE_EXCLUDE

typedef BOOL (WINAPI *PFN_MagInitialize)();
typedef BOOL (WINAPI *PFN_MagUninitialize)();
typedef BOOL (WINAPI *PFN_MagSetWindowSource)(HWND, RECT);
typedef BOOL (WINAPI *PFN_MagSetWindowTransform)(HWND, MagTransform*);
typedef BOOL (WINAPI *PFN_MagSetColorEffect)(HWND, MagColorEffect*);
typedef BOOL (WINAPI *PFN_MagSetWindowFilterList)(HWND, DWORD, int, HWND*);

static PFN_MagInitialize          MagInitialize;
static PFN_MagUninitialize        MagUninitialize;
static PFN_MagSetWindowSource     MagSetWindowSource;
static PFN_MagSetWindowTransform  MagSetWindowTransform;
static PFN_MagSetColorEffect      MagSetColorEffect;
static PFN_MagSetWindowFilterList MagSetWindowFilterList;

static bool loadMagnification() {
    HMODULE m = LoadLibraryW(L"Magnification.dll");
    if (!m) return false;
    MagInitialize          = (PFN_MagInitialize)         (void*)GetProcAddress(m, "MagInitialize");
    MagUninitialize        = (PFN_MagUninitialize)       (void*)GetProcAddress(m, "MagUninitialize");
    MagSetWindowSource     = (PFN_MagSetWindowSource)    (void*)GetProcAddress(m, "MagSetWindowSource");
    MagSetWindowTransform  = (PFN_MagSetWindowTransform) (void*)GetProcAddress(m, "MagSetWindowTransform");
    MagSetColorEffect      = (PFN_MagSetColorEffect)     (void*)GetProcAddress(m, "MagSetColorEffect");
    MagSetWindowFilterList = (PFN_MagSetWindowFilterList)(void*)GetProcAddress(m, "MagSetWindowFilterList");
    return MagInitialize && MagUninitialize && MagSetWindowSource &&
           MagSetWindowTransform && MagSetColorEffect && MagSetWindowFilterList;
}

// ---------------------------------------------------------------- DPI
// The overlay works in physical pixels (per-monitor aware). The editor window is created
// system-DPI aware so its fixed layout can simply be scaled once. Window procedures run in
// their window's DPI context, so anything touching the overlay switches back to physical
// pixels first (PHYSICAL_PIXELS).

typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
static PFN_SetThreadDpiAwarenessContext pSetThreadDpiCtx;

struct DpiScope {
    DPI_AWARENESS_CONTEXT old = nullptr;
    explicit DpiScope(DPI_AWARENESS_CONTEXT ctx) { if (pSetThreadDpiCtx) old = pSetThreadDpiCtx(ctx); }
    ~DpiScope() { if (old) pSetThreadDpiCtx(old); }
};
#define PHYSICAL_PIXELS DpiScope dpiScope_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)

// ---------------------------------------------------------------- string helpers

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
static std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring u2w(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static bool sameName(const std::wstring& a, const std::wstring& b) { return lstrcmpiW(a.c_str(), b.c_str()) == 0; }

// Splits "key = value  # comment" into key (lower case) and value. False for blank/comment lines.
static bool splitKeyValue(const std::string& line, std::string& key, std::string& val) {
    std::string t = trim(line);
    size_t hash = t.find_first_of("#;");
    if (hash != std::string::npos) t = trim(t.substr(0, hash));
    size_t eq = t.find('=');
    if (t.empty() || eq == std::string::npos) return false;
    key = lower(trim(t.substr(0, eq)));
    val = trim(t.substr(eq + 1));
    return true;
}

static std::vector<std::string> readLines(const fs::path& p) {
    std::vector<std::string> out;
    std::ifstream f(p);
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

// ---------------------------------------------------------------- model

typedef std::pair<int, int> Px;   // (x, y) relative to the crosshair centre, +y down
typedef std::set<Px> PxSet;

struct TypeInfo {
    const char* id; const wchar_t* name;
    const char* key[3]; const wchar_t* label[3]; int def[3];   // nullptr key = unused parameter
    const char* fkey[4]; const wchar_t* flabel[4];             // nullptr key = unused flag
};
static const TypeInfo kTypes[] = {
    { "cross", L"Cross",
      { "length", "thickness", "gap" }, { L"Arm length", L"Thickness", L"Gap" }, { 6, 2, 3 },
      { "top", "bottom", "left", "right" }, { L"Top arm", L"Bottom arm", L"Left arm", L"Right arm" } },
    { "dot", L"Dot",
      { "size", nullptr, nullptr }, { L"Size", nullptr, nullptr }, { 2, 0, 0 }, {}, {} },
    { "circle", L"Circle / ring",
      { "radius", "thickness", nullptr }, { L"Radius", L"Thickness (0 = filled)", nullptr }, { 5, 1, 0 }, {}, {} },
    { "box", L"Box",
      { "width", "height", "thickness" }, { L"Width", L"Height", L"Thickness (0 = filled)" }, { 9, 9, 1 }, {}, {} },
    { "diagonal", L"Diagonal X",
      { "length", "thickness", "gap" }, { L"Arm length", L"Thickness", L"Gap" }, { 5, 1, 2 },
      { "up_right", "down_right", "down_left", "up_left" }, { L"Up-right", L"Down-right", L"Down-left", L"Up-left" } },
};
static const int kTypeCount = (int)(sizeof(kTypes) / sizeof(kTypes[0]));
static const int kMaxParam = 256, kMaxOffset = 4000;

struct Component {
    int type = 0;
    bool enabled = true, subtract = false;
    int x = 0, y = 0;
    int p[3] = { 0, 0, 0 };
    bool f[4] = { true, true, true, true };
};
static Component makeComponent(int type) {
    Component c;
    c.type = type;
    for (int i = 0; i < 3; ++i) c.p[i] = kTypes[type].def[i];
    return c;
}

// Effects, applied in this order. Each is a checkbox; some have a value.
// Max contrast is adaptive and replaces all the others when it is on.
enum { FX_MAXCON, FX_GRAY, FX_INVERT, FX_INVVAL, FX_HUE, FX_SAT, FX_CONTRAST, FX_VALUE, FX_OVERLAY, FX_COUNT };
struct FxDef { const char* key; const char* valKey; const wchar_t* label; int lo, hi, defVal; bool defOn; };
static const FxDef kFx[FX_COUNT] = {
    { "max_contrast",  "max_contrast_color", L"Max contrast (color %)", 0, 100, 100, false },
    { "grayscale",     nullptr,           L"Grayscale",         0,    0,   0,   false },
    { "invert",        nullptr,           L"Invert colors",     0,    0,   0,   true  },
    { "invert_value",  nullptr,           L"Invert brightness", 0,    0,   0,   false },
    { "hue_shift",     "hue_degrees",     L"Hue shift (\u00B0)",   -360, 360, 180, false },
    { "saturation",    "saturation_pct",  L"Saturation (%)",    0,    1000, 200, false },
    { "contrast",      "contrast_pct",    L"Contrast (%)",      0,    1000, 150, false },
    { "value_shift",   "value_amount",    L"Value shift",       -255, 255, 64,  false },
    { "color_overlay", "overlay_opacity", L"Color overlay (%)", 0,    100, 100, false },
};
struct Effects {
    bool on[FX_COUNT];
    int val[FX_COUNT];
    int rgb[3] = { 255, 0, 0 };
    Effects() { for (int i = 0; i < FX_COUNT; ++i) { on[i] = kFx[i].defOn; val[i] = kFx[i].defVal; } }
    bool any() const { for (bool b : on) if (b) return true; return false; }
};

struct Preset {
    std::wstring name;
    std::vector<Component> comps;
    std::map<Px, bool> pixels;   // hand-drawn edits: true = force on, false = force off
    int offX = 0, offY = 0;
    Effects fx;
    bool dirty = false;
};

static Preset defaultPreset(const std::wstring& name) {
    Preset p;
    p.name = name;
    p.comps.push_back(makeComponent(0));
    return p;
}

// ---------------------------------------------------------------- hotkeys (definitions)

enum { HK_TOGGLE, HK_EDITOR, HK_NEXT, HK_PREV, HK_LEFT, HK_RIGHT, HK_UP, HK_DOWN, HK_SAVE, HK_RELOAD, HK_REFRESH, HK_QUIT, HK_COUNT };
struct HkDef { const char* key; const wchar_t* label; UINT vk; bool repeat; };
static const HkDef kHk[HK_COUNT] = {
    { "key_toggle", L"Toggle crosshair",      'H',      false },
    { "key_editor", L"Toggle editor",         'E',      false },
    { "key_next",   L"Next preset",           'N',      false },
    { "key_prev",   L"Previous preset",       'P',      false },
    { "key_left",   L"Nudge left",            VK_LEFT,  true  },
    { "key_right",  L"Nudge right",           VK_RIGHT, true  },
    { "key_up",     L"Nudge up",              VK_UP,    true  },
    { "key_down",   L"Nudge down",            VK_DOWN,  true  },
    { "key_save",   L"Save preset",           'S',      false },
    { "key_reload", L"Reload preset",         'R',      false },
    { "key_refresh", L"Refresh overlay",      'U',      false },
    { "key_quit",   L"Quit",                  'Q',      false },
};
// A hotkey is stored as (MOD_* flags << 16) | virtual-key code; 0 = unassigned.
static UINT defaultHotkey(int i) { return ((UINT)(MOD_CONTROL | MOD_ALT) << 16) | kHk[i].vk; }

static const struct { UINT vk; const char* name; } kKeyNames[] = {
    { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_UP, "Up" }, { VK_DOWN, "Down" },
    { VK_HOME, "Home" }, { VK_END, "End" }, { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" },
    { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_SPACE, "Space" }, { VK_RETURN, "Enter" },
    { VK_TAB, "Tab" }, { VK_ESCAPE, "Escape" }, { VK_BACK, "Backspace" }, { VK_PAUSE, "Pause" },
    { VK_MULTIPLY, "NumMul" }, { VK_ADD, "NumPlus" }, { VK_SUBTRACT, "NumMinus" },
    { VK_DECIMAL, "NumDot" }, { VK_DIVIDE, "NumDiv" },
};

static std::string vkName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    for (auto& k : kKeyNames) if (k.vk == vk) return k.name;
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", vk & 0xFF);
    return buf;
}
static std::string modsPrefix(UINT mods) {
    std::string s;
    if (mods & MOD_CONTROL) s += "Ctrl+";
    if (mods & MOD_ALT)     s += "Alt+";
    if (mods & MOD_SHIFT)   s += "Shift+";
    if (mods & MOD_WIN)     s += "Win+";
    return s;
}
static std::string hotkeyToString(UINT hk) {
    UINT vk = hk & 0xFFFF;
    if (!vk) return "None";
    return modsPrefix(hk >> 16) + vkName(vk);
}
static UINT parseHotkey(const std::string& text) {
    UINT mods = 0, vk = 0;
    std::string rest = text;
    while (!rest.empty()) {
        size_t plus = rest.find('+');
        std::string tok = lower(trim(rest.substr(0, plus)));
        rest = plus == std::string::npos ? "" : rest.substr(plus + 1);
        if (tok.empty() || tok == "none") continue;
        if (tok == "ctrl" || tok == "control") mods |= MOD_CONTROL;
        else if (tok == "alt")   mods |= MOD_ALT;
        else if (tok == "shift") mods |= MOD_SHIFT;
        else if (tok == "win")   mods |= MOD_WIN;
        else if (tok.size() == 1 && std::isalnum((unsigned char)tok[0])) vk = (UINT)std::toupper((unsigned char)tok[0]);
        else if (tok.size() > 2 && tok[0] == '0' && tok[1] == 'x') vk = (UINT)strtoul(tok.c_str(), nullptr, 16) & 0xFF;
        else if (tok[0] == 'f' && tok.size() <= 3 && std::isdigit((unsigned char)tok[1])) {
            int n = atoi(tok.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
        } else if (tok.size() == 4 && tok.compare(0, 3, "num") == 0 && std::isdigit((unsigned char)tok[3])) {
            vk = VK_NUMPAD0 + (tok[3] - '0');
        } else {
            for (auto& k : kKeyNames) if (lower(k.name) == tok) vk = k.vk;
        }
    }
    return vk ? ((mods << 16) | vk) : 0;
}

// ---------------------------------------------------------------- settings

struct Settings {
    std::wstring preset = L"default";
    int monitor = 0;              // 0 = primary, 1..N = enumeration order
    int fps = 0;                  // 0 = sync to display refresh
    bool startHidden = false;     // start with the editor hidden in the tray
    int theme = 0;                // editor colours: 0 = follow Windows, 1 = light, 2 = dark
    UINT hk[HK_COUNT];
    Settings() { for (int i = 0; i < HK_COUNT; ++i) hk[i] = defaultHotkey(i); }
};

static const char* kThemeNames[3] = { "system", "light", "dark" };
static fs::path g_iniPath, g_presetDir;
static Settings g_set;

static void loadSettings() {
    for (auto& line : readLines(g_iniPath)) {
        std::string key, val;
        if (!splitKeyValue(line, key, val)) continue;
        int v = atoi(val.c_str());
        if      (key == "preset")       g_set.preset = u2w(val);
        else if (key == "monitor")      g_set.monitor = std::max(0, v);
        else if (key == "fps")          g_set.fps = std::max(0, v);
        else if (key == "start_hidden") g_set.startHidden = v != 0;
        else if (key == "theme")        g_set.theme = lower(val) == "dark" ? 2 : lower(val) == "light" ? 1 : 0;
        else for (int i = 0; i < HK_COUNT; ++i) if (key == kHk[i].key) g_set.hk[i] = parseHotkey(val);
    }
}

static void saveSettings() {
    std::ofstream f(g_iniPath, std::ios::trunc);
    if (!f) return;
    f << "# Crosshair settings. The crosshair shapes themselves are saved as presets in the presets folder.\n"
         "# Everything here can be changed in the editor (Ctrl+Alt+E or the tray icon).\n\n";
    f << "preset       = " << w2u(g_set.preset) << "\n";
    f << "monitor      = " << g_set.monitor << "   # 0 = primary, 1, 2, ... = other monitors\n";
    f << "fps          = " << g_set.fps << "   # 0 = every display refresh; otherwise a fixed update rate\n";
    f << "start_hidden = " << (g_set.startHidden ? 1 : 0) << "   # 1 = start with the editor hidden in the tray\n";
    f << "theme        = " << kThemeNames[std::clamp(g_set.theme, 0, 2)] << "   # editor colours: system, light or dark\n\n";
    f << "# Hotkeys: Ctrl/Alt/Shift/Win + key (A-Z, 0-9, F1-F24, Left, Right, Up, Down, Home, End,\n"
         "# PageUp, PageDown, Insert, Delete, Space, Num0-9, ...), or None\n";
    for (int i = 0; i < HK_COUNT; ++i) f << kHk[i].key << " = " << hotkeyToString(g_set.hk[i]) << "\n";
}

// ---------------------------------------------------------------- preset files

static fs::path presetPath(const std::wstring& name) { return g_presetDir / (name + L".ini"); }

// Pixel rows: X = on, - = off, . = untouched. Centre = pixels_origin, or the C/c cell
// (C = on, c = untouched), or the middle of the block.
static void parsePixelRows(const std::vector<std::string>& rows, bool haveOrigin, int ox, int oy,
                           std::map<Px, bool>& out) {
    int cx = -1, cy = -1, w = 0;
    for (int y = 0; y < (int)rows.size(); ++y) {
        w = std::max(w, (int)rows[y].size());
        for (int x = 0; x < (int)rows[y].size(); ++x)
            if (rows[y][x] == 'C' || rows[y][x] == 'c') { cx = x; cy = y; }
    }
    if (!haveOrigin) {
        if (cx >= 0) { ox = cx; oy = cy; }
        else { ox = w / 2; oy = (int)rows.size() / 2; }
    }
    for (int y = 0; y < (int)rows.size(); ++y)
        for (int x = 0; x < (int)rows[y].size(); ++x) {
            char ch = rows[y][x];
            Px k{ x - ox, y - oy };
            if (ch == 'X' || ch == 'x' || ch == 'C') out[k] = true;
            else if (ch == '-' || ch == 'o' || ch == 'O') out[k] = false;
        }
}

// Also reads the old single-file crosshair.ini format (mode = lines / pattern), so an
// existing config is imported as a preset. Old monitor / fps keys go into *legacy.
static bool loadPreset(const fs::path& path, Preset& out, Settings* legacy) {
    std::ifstream f(path);
    if (!f) return false;
    Preset p;
    p.name = path.stem().wstring();
    p.fx = Effects();

    bool isLegacy = false, legacyBlock = false;
    std::string mode = "lines";
    int L = 6, T = 2, G = 3, D = 0;
    bool arms[4] = { true, true, true, true };
    std::vector<std::string> rows;
    bool inBlock = false, haveOrigin = false;
    int ox = 0, oy = 0;
    bool inComp = false;

    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        std::string lt = lower(t);
        if (inBlock) {
            if (lt == "pixels_end" || lt == "pattern_end") inBlock = false;
            else if (!t.empty()) rows.push_back(t);
            continue;
        }
        if (lt == "pixels_begin" || lt == "pattern_begin") {
            inBlock = true; rows.clear();
            legacyBlock = lt == "pattern_begin";
            continue;
        }
        if (lt == "[component]") { p.comps.push_back(Component()); inComp = true; continue; }
        if (!lt.empty() && lt[0] == '[') { inComp = false; continue; }

        std::string key, val;
        if (!splitKeyValue(line, key, val)) continue;
        int v = atoi(val.c_str());
        bool b = v != 0;

        if (inComp) {
            Component& c = p.comps.back();
            if (key == "type") {
                for (int i = 0; i < kTypeCount; ++i)
                    if (lower(val) == kTypes[i].id) c = makeComponent(i);
                continue;
            }
            if (key == "enabled")  { c.enabled = b; continue; }
            if (key == "subtract") { c.subtract = b; continue; }
            if (key == "x")        { c.x = v; continue; }
            if (key == "y")        { c.y = v; continue; }
            const TypeInfo& ti = kTypes[c.type];
            for (int i = 0; i < 3; ++i) if (ti.key[i] && key == ti.key[i]) c.p[i] = std::clamp(v, 0, kMaxParam);
            for (int i = 0; i < 4; ++i) if (ti.fkey[i] && key == ti.fkey[i]) c.f[i] = b;
            continue;
        }

        if      (key == "offset_x") p.offX = v;
        else if (key == "offset_y") p.offY = v;
        else if (key == "overlay_rgb") {
            int r = 255, g = 0, bl = 0;
            if (sscanf(val.c_str(), "%d %d %d", &r, &g, &bl) == 3 || sscanf(val.c_str(), "%d,%d,%d", &r, &g, &bl) == 3) {
                p.fx.rgb[0] = std::clamp(r, 0, 255); p.fx.rgb[1] = std::clamp(g, 0, 255); p.fx.rgb[2] = std::clamp(bl, 0, 255);
            }
        }
        else if (key == "pixels_origin") haveOrigin = sscanf(val.c_str(), "%d %d", &ox, &oy) == 2;
        // legacy single-file format
        else if (key == "mode")      { mode = lower(val); isLegacy = true; }
        else if (key == "length")    { L = v; isLegacy = true; }
        else if (key == "thickness") { T = v; isLegacy = true; }
        else if (key == "gap")       { G = v; isLegacy = true; }
        else if (key == "dot")       { D = v; isLegacy = true; }
        else if (key == "top")       { arms[0] = b; isLegacy = true; }
        else if (key == "bottom")    { arms[1] = b; isLegacy = true; }
        else if (key == "left")      { arms[2] = b; isLegacy = true; }
        else if (key == "right")     { arms[3] = b; isLegacy = true; }
        else if (key == "monitor")   { if (legacy) legacy->monitor = std::max(0, v); }
        else if (key == "fps")       { if (legacy) legacy->fps = std::max(0, v); }
        else {
            for (int i = 0; i < FX_COUNT; ++i) {
                if (key == kFx[i].key) p.fx.on[i] = b;
                if (kFx[i].valKey && key == kFx[i].valKey) p.fx.val[i] = std::clamp(v, kFx[i].lo, kFx[i].hi);
            }
        }
    }

    // An old file in lines mode still carries a pattern block; only use it in pattern mode.
    if (!rows.empty() && (!legacyBlock || mode == "pattern"))
        parsePixelRows(rows, haveOrigin, ox, oy, p.pixels);
    if (isLegacy && p.comps.empty() && mode != "pattern") {
        Component c = makeComponent(0);
        c.p[0] = std::clamp(L, 0, kMaxParam); c.p[1] = std::clamp(T, 1, kMaxParam); c.p[2] = std::clamp(G, 0, kMaxParam);
        for (int i = 0; i < 4; ++i) c.f[i] = arms[i];
        if (L > 0) p.comps.push_back(c);
        if (D > 0) { Component d = makeComponent(1); d.p[0] = std::min(D, kMaxParam); p.comps.push_back(d); }
    }
    out = p;
    return true;
}

static bool writePreset(const fs::path& path, const Preset& p) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# Crosshair preset - written by the crosshair editor, safe to edit by hand.\n"
         "# Coordinates are screen pixels relative to the centre (+x right, +y down).\n\n";
    f << "offset_x = " << p.offX << "\n";
    f << "offset_y = " << p.offY << "\n\n";
    f << "# effects, applied top to bottom (1 = on)\n";
    for (int i = 0; i < FX_COUNT; ++i) {
        f << kFx[i].key << " = " << (p.fx.on[i] ? 1 : 0) << "\n";
        if (kFx[i].valKey) f << kFx[i].valKey << " = " << p.fx.val[i] << "\n";
    }
    f << "overlay_rgb = " << p.fx.rgb[0] << " " << p.fx.rgb[1] << " " << p.fx.rgb[2] << "\n";

    f << "\n# components, drawn in order (subtract = 1 erases instead of drawing)\n";
    for (auto& c : p.comps) {
        const TypeInfo& t = kTypes[c.type];
        f << "\n[component]\n";
        f << "type = " << t.id << "\n";
        f << "enabled = " << (c.enabled ? 1 : 0) << "\n";
        f << "subtract = " << (c.subtract ? 1 : 0) << "\n";
        f << "x = " << c.x << "\ny = " << c.y << "\n";
        for (int i = 0; i < 3; ++i) if (t.key[i]) f << t.key[i] << " = " << c.p[i] << "\n";
        for (int i = 0; i < 4; ++i) if (t.fkey[i]) f << t.fkey[i] << " = " << (c.f[i] ? 1 : 0) << "\n";
    }

    if (!p.pixels.empty()) {
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (auto& kv : p.pixels) {
            minX = std::min(minX, kv.first.first);  maxX = std::max(maxX, kv.first.first);
            minY = std::min(minY, kv.first.second); maxY = std::max(maxY, kv.first.second);
        }
        f << "\n[pixels]\n# hand-drawn edits on top of the components: X = force on, - = force off, . = untouched\n";
        f << "pixels_origin = " << -minX << " " << -minY << "   # column / row of the centre pixel\n";
        f << "pixels_begin\n";
        for (int y = minY; y <= maxY; ++y) {
            std::string row;
            for (int x = minX; x <= maxX; ++x) {
                auto it = p.pixels.find({ x, y });
                row += it == p.pixels.end() ? '.' : (it->second ? 'X' : '-');
            }
            f << row << "\n";
        }
        f << "pixels_end\n";
    }
    return f.good();
}

static bool fileHasLegacyKeys(const fs::path& p) {
    for (auto& line : readLines(p)) {
        std::string lt = lower(trim(line)), key, val;
        if (lt == "pattern_begin") return true;
        if (splitKeyValue(line, key, val) && (key == "mode" || key == "length" || key == "thickness")) return true;
    }
    return false;
}

// ---------------------------------------------------------------- mask

static int bandStart(int n) { return -(n / 2); }   // a band of n pixels "centred" on pixel 0

template <class F>
static void componentPoints(const Component& c, F add) {
    auto P = [&](int i) { return std::clamp(c.p[i], 0, kMaxParam); };
    auto put = [&](int x, int y) { add(x + c.x, y + c.y); };
    switch (c.type) {
    case 0: {   // cross
        int L = P(0), t = std::max(1, P(1)), g = P(2), bs = bandStart(t);
        for (int a = bs; a < bs + t; ++a)
            for (int i = 0; i < L; ++i) {
                int outer = bs + t + g + i, inner = bs - g - 1 - i;
                if (c.f[0]) put(a, inner);
                if (c.f[1]) put(a, outer);
                if (c.f[2]) put(inner, a);
                if (c.f[3]) put(outer, a);
            }
        break;
    }
    case 1: {   // dot
        int s = std::max(1, P(0)), bs = bandStart(s);
        for (int y = bs; y < bs + s; ++y)
            for (int x = bs; x < bs + s; ++x) put(x, y);
        break;
    }
    case 2: {   // circle / ring
        int r = P(0), t = P(1), in = r - t;
        long long outer = (long long)r * r + r, inner = (long long)in * in + in;
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                long long d2 = (long long)x * x + (long long)y * y;
                if (d2 > outer) continue;
                if (t > 0 && in >= 0 && d2 <= inner) continue;
                put(x, y);
            }
        break;
    }
    case 3: {   // box
        int w = std::max(1, P(0)), h = std::max(1, P(1)), t = P(2);
        int xs = bandStart(w), ys = bandStart(h);
        for (int y = ys; y < ys + h; ++y)
            for (int x = xs; x < xs + w; ++x) {
                bool edge = t <= 0 || x - xs < t || xs + w - 1 - x < t || y - ys < t || ys + h - 1 - y < t;
                if (edge) put(x, y);
            }
        break;
    }
    case 4: {   // diagonal X
        static const int dx[4] = { 1, 1, -1, -1 }, dy[4] = { -1, 1, 1, -1 };
        int L = P(0), t = std::max(1, P(1)), g = P(2), bs = bandStart(t);
        for (int d = 0; d < 4; ++d) {
            if (!c.f[d]) continue;
            for (int i = g + 1; i <= g + L; ++i)
                for (int a = bs; a < bs + t; ++a) put(dx[d] * i + a, dy[d] * i);
        }
        break;
    }
    }
}

static PxSet componentLayer(const Preset& p) {
    PxSet s;
    for (auto& c : p.comps) {
        if (!c.enabled) continue;
        componentPoints(c, [&](int x, int y) {
            if (c.subtract) s.erase({ x, y }); else s.insert({ x, y });
        });
    }
    return s;
}
static PxSet finalLayer(const Preset& p, const PxSet& base) {
    PxSet s = base;
    for (auto& kv : p.pixels) {
        if (kv.second) s.insert(kv.first); else s.erase(kv.first);
    }
    return s;
}

// ---------------------------------------------------------------- colour matrix

// Column-vector convention: out = M * [r g b a 1]. Magnification wants the transpose.
typedef std::array<std::array<float, 5>, 5> M5;
static M5 identity5() {
    M5 m{};
    for (int i = 0; i < 5; ++i) m[i][i] = 1.0f;
    return m;
}
static M5 mul5(const M5& a, const M5& b) {
    M5 c{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            float s = 0;
            for (int k = 0; k < 5; ++k) s += a[i][k] * b[k][j];
            c[i][j] = s;
        }
    return c;
}

static MagColorEffect buildEffect(const Effects& fx) {
    const float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;   // Rec.709 luma
    M5 m = identity5();
    auto apply = [&](const M5& s) { m = mul5(s, m); };
    if (fx.on[FX_MAXCON]) {
        // Per pixel, the colour furthest from the background c is the opposite corner of the
        // RGB cube: each channel becomes 1 if c < 0.5, else 0. The matrix itself is affine, but
        // the output is clamped to [0, 1], so a very steep "invert" produces exactly that step:
        //   out = clamp(K * (0.5 - s) + 0.5),  s = w * c + (1 - w) * luma
        // w = 1 decides every channel separately (furthest colour), w = 0 decides on brightness
        // only (pure black or white), anything between mixes the two. A large K keeps the step
        // between the 8-bit levels 127 and 128 and leaves almost no grey band around it.
        const float K = 4096.0f, w = fx.val[FX_MAXCON] / 100.0f, l[3] = { lr, lg, lb };
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] = -K * ((i == j ? w : 0.0f) + (1.0f - w) * l[j]);
            m[i][4] = 0.5f * K + 0.5f;
        }
        MagColorEffect e;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) e.transform[i][j] = m[j][i];
        return e;
    }
    if (fx.on[FX_GRAY]) {
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) { s[i][0] = lr; s[i][1] = lg; s[i][2] = lb; }
        apply(s);
    }
    if (fx.on[FX_INVERT]) {   // c' = 1 - c
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) { s[i][i] = -1.0f; s[i][4] = 1.0f; }
        apply(s);
    }
    if (fx.on[FX_INVVAL]) {   // flip brightness, keep hue: c' = c + 1 - 2*luma
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) { s[i][0] -= 2 * lr; s[i][1] -= 2 * lg; s[i][2] -= 2 * lb; s[i][4] = 1.0f; }
        apply(s);
    }
    if (fx.on[FX_HUE]) {      // rotation around the grey axis (same as SVG hueRotate)
        float a = fx.val[FX_HUE] * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        M5 h = identity5();
        h[0][0] = 0.213f + c * 0.787f - s * 0.213f; h[0][1] = 0.715f - c * 0.715f - s * 0.715f; h[0][2] = 0.072f - c * 0.072f + s * 0.928f;
        h[1][0] = 0.213f - c * 0.213f + s * 0.143f; h[1][1] = 0.715f + c * 0.285f + s * 0.140f; h[1][2] = 0.072f - c * 0.072f - s * 0.283f;
        h[2][0] = 0.213f - c * 0.213f - s * 0.787f; h[2][1] = 0.715f - c * 0.715f + s * 0.715f; h[2][2] = 0.072f + c * 0.928f + s * 0.072f;
        apply(h);
    }
    if (fx.on[FX_SAT]) {
        float k = fx.val[FX_SAT] / 100.0f;
        M5 s = identity5();
        s[0][0] = 0.213f + 0.787f * k; s[0][1] = 0.715f - 0.715f * k; s[0][2] = 0.072f - 0.072f * k;
        s[1][0] = 0.213f - 0.213f * k; s[1][1] = 0.715f + 0.285f * k; s[1][2] = 0.072f - 0.072f * k;
        s[2][0] = 0.213f - 0.213f * k; s[2][1] = 0.715f - 0.715f * k; s[2][2] = 0.072f + 0.928f * k;
        apply(s);
    }
    if (fx.on[FX_CONTRAST]) {  // around mid grey
        float k = fx.val[FX_CONTRAST] / 100.0f;
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) { s[i][i] = k; s[i][4] = 0.5f * (1.0f - k); }
        apply(s);
    }
    if (fx.on[FX_VALUE]) {
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) s[i][4] = fx.val[FX_VALUE] / 255.0f;
        apply(s);
    }
    if (fx.on[FX_OVERLAY]) {   // blend towards a fixed colour; 100% = solid colour
        float a = fx.val[FX_OVERLAY] / 100.0f;
        M5 s = identity5();
        for (int i = 0; i < 3; ++i) { s[i][i] = 1.0f - a; s[i][4] = a * fx.rgb[i] / 255.0f; }
        apply(s);
    }
    MagColorEffect e;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) e.transform[i][j] = m[j][i];
    return e;
}

// ---------------------------------------------------------------- monitors

struct MonInfo { RECT rc; std::wstring name; bool primary; };
static BOOL CALLBACK monEnumProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) {
        std::wstring n = mi.szDevice;
        if (n.rfind(L"\\\\.\\", 0) == 0) n = n.substr(4);
        reinterpret_cast<std::vector<MonInfo>*>(lp)->push_back({ mi.rcMonitor, n, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
    }
    return TRUE;
}
static std::vector<MonInfo> enumMonitors() {
    PHYSICAL_PIXELS;
    std::vector<MonInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, monEnumProc, (LPARAM)&v);
    return v;
}
static RECT monitorRect(int which) {
    PHYSICAL_PIXELS;
    if (which > 0) {
        auto mons = enumMonitors();
        if (which <= (int)mons.size()) return mons[which - 1].rc;
    }
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfo(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcMonitor;
}

// ---------------------------------------------------------------- app state

static HINSTANCE g_inst;
static std::vector<Preset> g_presets;
static int g_cur = 0;
static PxSet g_base, g_final;   // component layer / final mask of the current preset
static bool  g_visible = true, g_hkRegistered = false;
static HICON g_icon = nullptr, g_iconBig = nullptr;

static Preset& cur() { return g_presets[g_cur]; }

// editor controls
struct NumEdit { HWND edit = nullptr, ud = nullptr; int lo = 0, hi = 0; };
static HWND g_editor = nullptr, g_grid = nullptr, g_status = nullptr, g_hover = nullptr;
static HWND g_presetCombo, g_nameEdit, g_compList, g_compType, g_themeCombo, g_legend;
static int g_compGroupIdx = -1;
static std::vector<NumEdit> g_numEdits;   // for arrow-key stepping
static HWND g_cEnabled, g_cSubtract, g_pLabel[3], g_cFlag[4];
static NumEdit g_pEdit[3], g_cX, g_cY;
static std::vector<HWND> g_compCtrls;   // enabled only while a component is selected
static HWND g_monitorCombo, g_overlayCheck, g_startHidden;
static NumEdit g_fps, g_viewR, g_offX, g_offY;
static HWND g_mirrorH, g_mirrorV;
static HWND g_fxCheck[FX_COUNT];
static NumEdit g_fxVal[FX_COUNT], g_rgb[3];
static HWND g_hkCtrl[HK_COUNT];
static HFONT g_font = nullptr;
static int g_dpi = 96, g_populating = 0, g_selComp = -1, g_viewRadius = 0;
static bool g_ready = false;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbarCreated = 0;

enum {
    WM_APP_TRAY = WM_APP + 1, WM_APP_SHOW,
    WM_APP_QUIT = WM_APP + 3,   // sent by the installer / uninstaller to close a running copy
    ID_PRESET_COMBO = 100, ID_PRESET_NAME, ID_PRESET_SAVE, ID_PRESET_NEW, ID_PRESET_DELETE,
    ID_COMP_LIST, ID_COMP_TYPE, ID_COMP_ADD, ID_COMP_DUP, ID_COMP_REMOVE, ID_COMP_UP, ID_COMP_DOWN,
    ID_COMP_ENABLED, ID_COMP_SUBTRACT, ID_COMP_P0, ID_COMP_P1, ID_COMP_P2, ID_COMP_X, ID_COMP_Y,
    ID_COMP_F0, ID_COMP_F1, ID_COMP_F2, ID_COMP_F3,
    ID_MONITOR, ID_FPS, ID_OVERLAY_VISIBLE, ID_START_HIDDEN, ID_THEME,
    ID_GRID, ID_MIRROR_H, ID_MIRROR_V, ID_VIEW_RADIUS, ID_CLEAR_PIXELS, ID_BAKE, ID_UNDO, ID_REDO,
    ID_OFF_X, ID_OFF_Y, ID_CENTRE,
    ID_HK_DEFAULTS, ID_HIDE, ID_QUIT, ID_REFRESH, ID_TOGGLE_OVERLAY,
    ID_FX_CHECK = 300, ID_FX_VAL = 320, ID_FX_RGB = 340,
    ID_HK = 400, ID_HK_CLEAR = 450,
    ID_TRAY_OPEN = 900, ID_TRAY_TOGGLE, ID_TRAY_QUIT, ID_TRAY_REFRESH, ID_TRAY_PRESET = 1000,
};

static void setStatus(const std::wstring& s) { if (g_status) SetWindowTextW(g_status, s.c_str()); }

// ---------------------------------------------------------------- overlay thread
// The overlay (host window + magnifier) lives on its own high-priority thread with its own
// frame loop. The editor only publishes a new mask / effect / position into g_ov; the thread
// picks it up on its next frame. "Refresh overlay" destroys and recreates the magnifier.

struct OverlayShared {
    std::mutex lock;
    std::vector<Px> pixels;    // final mask, relative to the centre
    MagColorEffect effect{};
    int monitor = 0, offX = 0, offY = 0, fps = 0;
    bool visible = true;
    unsigned version = 0;      // bumped on every change
};
static OverlayShared g_ov;
static HANDLE g_ovThread = nullptr;
static std::atomic<bool> g_ovRun{ true }, g_ovRecreate{ false }, g_ovForceApply{ false }, g_ovHealthy{ false };
static std::atomic<int> g_ovFps{ 0 };
static std::atomic<DWORD> g_ovLastFrame{ 0 };

// Called on the editor thread after every change: recompute the mask and hand it over.
static void rebuildOverlay() {
    g_base = componentLayer(cur());
    g_final = finalLayer(cur(), g_base);
    if (g_grid) InvalidateRect(g_grid, nullptr, FALSE);
    MagColorEffect e = buildEffect(cur().fx);
    std::vector<Px> v(g_final.begin(), g_final.end());
    std::lock_guard<std::mutex> lk(g_ov.lock);
    g_ov.pixels.swap(v);
    g_ov.effect = e;
    g_ov.monitor = g_set.monitor;
    g_ov.offX = cur().offX;
    g_ov.offY = cur().offY;
    g_ov.fps = g_set.fps;
    g_ov.visible = g_visible;
    ++g_ov.version;
}

static LRESULT CALLBACK hostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_ovForceApply = true;   // monitor layout changed: recompute the position
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    }
    return DefWindowProc(h, msg, wp, lp);
}

struct OverlayWindows { HWND host = nullptr, mag = nullptr; };

static bool createOverlayWindows(OverlayWindows& w) {
    w.host = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"InvertCrosshairHost", L"Crosshair", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    if (!w.host) return false;
    SetLayeredWindowAttributes(w.host, 0, 255, LWA_ALPHA);
    w.mag = CreateWindowExW(0, kMagClass, L"CrosshairMag", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, w.host, nullptr, g_inst, nullptr);
    if (!w.mag) return false;
    MagTransform identity = {};
    identity.v[0][0] = identity.v[1][1] = identity.v[2][2] = 1.0f;
    MagSetWindowTransform(w.mag, &identity);
    HWND exclude[] = { w.host };   // don't capture ourselves, or the effect would apply to our own output
    MagSetWindowFilterList(w.mag, kFilterExclude, 1, exclude);
    return true;
}
static void destroyOverlayWindows(OverlayWindows& w) {
    if (w.host) DestroyWindow(w.host);   // also destroys the magnifier child
    w = OverlayWindows();
}

// Positions and clips the host window. Returns true if the crosshair is on screen.
static bool applyOverlay(const OverlayWindows& w, std::vector<Px>& v, MagColorEffect eff, int monitor,
                         int offX, int offY, bool visible, RECT& src) {
    MagSetColorEffect(w.mag, &eff);
    if (v.empty() || !visible) { ShowWindow(w.host, SW_HIDE); return false; }

    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (auto& q : v) {
        minX = std::min(minX, q.first);  maxX = std::max(maxX, q.first);
        minY = std::min(minY, q.second); maxY = std::max(maxY, q.second);
    }
    int wd = maxX - minX + 1, ht = maxY - minY + 1;
    RECT mon = monitorRect(monitor);
    // Centre pixel of the monitor: for a 1920x1080 screen this is (960, 540).
    int x = mon.left + (mon.right - mon.left) / 2 + offX + minX;
    int y = mon.top + (mon.bottom - mon.top) / 2 + offY + minY;
    src = { x, y, x + wd, y + ht };
    SetWindowPos(w.host, HWND_TOPMOST, x, y, wd, ht, SWP_NOACTIVATE);
    SetWindowPos(w.mag, nullptr, 0, 0, wd, ht, SWP_NOZORDER | SWP_NOACTIVATE);

    // Clip the window to exactly the mask pixels (one rect per horizontal run).
    std::sort(v.begin(), v.end(), [](const Px& a, const Px& b) {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    });
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    for (size_t i = 0; i < v.size();) {
        int ry = v[i].second, x0 = v[i].first, x1 = x0 + 1;
        size_t j = i + 1;
        while (j < v.size() && v[j].second == ry && v[j].first == x1) { ++x1; ++j; }
        HRGN run = CreateRectRgn(x0 - minX, ry - minY, x1 - minX, ry - minY + 1);
        CombineRgn(rgn, rgn, run, RGN_OR);
        DeleteObject(run);
        i = j;
    }
    SetWindowRgn(w.host, rgn, TRUE);   // the system now owns rgn
    ShowWindow(w.host, SW_SHOWNOACTIVATE);
    return true;
}

static DWORD WINAPI overlayThread(LPVOID startedEvent) {
    PHYSICAL_PIXELS;
    // Stay smooth while a game keeps the CPU busy: high thread priority plus the MMCSS "Games" class.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsW)(LPCWSTR, LPDWORD);
    if (HMODULE avrt = LoadLibraryW(L"avrt.dll")) {
        auto avSet = (PFN_AvSetMmThreadCharacteristicsW)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
        DWORD taskIndex = 0;
        if (avSet) avSet(L"Games", &taskIndex);
    }

    OverlayWindows w;
    g_ovHealthy = MagInitialize() && createOverlayWindows(w);
    SetEvent((HANDLE)startedEvent);
    timeBeginPeriod(1);

    unsigned seen = ~0u;
    std::vector<Px> pixels;
    MagColorEffect effect{};
    int monitor = 0, offX = 0, offY = 0, fps = 0;
    bool visible = true, shown = false;
    RECT src = {};
    DWORD lastTopmost = GetTickCount(), fpsTick = lastTopmost, frames = 0;

    while (g_ovRun) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_ovRecreate.exchange(false)) {
            destroyOverlayWindows(w);
            MagUninitialize();
            g_ovHealthy = MagInitialize() && createOverlayWindows(w);
            seen = ~0u;
        }
        if (g_ovForceApply.exchange(false)) seen = ~0u;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_ov.lock);
            if (g_ov.version != seen) {
                seen = g_ov.version;
                pixels = g_ov.pixels;
                effect = g_ov.effect;
                monitor = g_ov.monitor; offX = g_ov.offX; offY = g_ov.offY;
                fps = g_ov.fps; visible = g_ov.visible;
                changed = true;
            }
        }
        if (changed && g_ovHealthy) shown = applyOverlay(w, pixels, effect, monitor, offX, offY, visible, src);
        if (shown && g_ovHealthy) {
            MagSetWindowSource(w.mag, src);
            InvalidateRect(w.mag, nullptr, FALSE);
            UpdateWindow(w.mag);   // render now, right after the vblank, instead of on a later frame
        }

        DWORD now = GetTickCount();
        // Games sometimes push themselves above other topmost windows; reassert periodically.
        if (shown && now - lastTopmost > 500) {
            SetWindowPos(w.host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            lastTopmost = now;
        }
        ++frames;
        if (now - fpsTick >= 500) {
            g_ovFps = (int)(frames * 1000 / (now - fpsTick));
            frames = 0;
            fpsTick = now;
        }
        g_ovLastFrame = now;

        if (fps > 0) Sleep(std::max(1, 1000 / fps));
        else if (FAILED(DwmFlush())) Sleep(1);   // wait for the next composed frame
    }
    timeEndPeriod(1);
    destroyOverlayWindows(w);
    MagUninitialize();
    return 0;
}

static bool startOverlay() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = hostProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"InvertCrosshairHost";
    RegisterClassExW(&wc);
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ovRun = true;
    g_ovThread = CreateThread(nullptr, 0, overlayThread, started, 0, nullptr);
    if (g_ovThread) WaitForSingleObject(started, 10000);
    CloseHandle(started);
    return g_ovThread && g_ovHealthy;
}
static void stopOverlay() {
    if (!g_ovThread) return;
    g_ovRun = false;
    WaitForSingleObject(g_ovThread, 3000);
    CloseHandle(g_ovThread);
    g_ovThread = nullptr;
}

// Short state for the editor header. kind: 0 = fine, 1 = neutral, 2 = needs attention.
static std::wstring overlayStatus(int& kind) {
    DWORD age = GetTickCount() - g_ovLastFrame;
    if (!g_ovThread)            { kind = 1; return L"Overlay off"; }
    if (!g_ovHealthy)           { kind = 2; return L"Overlay error \u00B7 press Refresh"; }
    if (age > 1500)             { kind = 2; return L"Stalled \u00B7 press Refresh"; }
    if (!g_visible)             { kind = 1; return L"Hidden"; }
    if (g_final.empty())        { kind = 1; return L"Nothing to draw"; }
    kind = 0;
    return L"Live \u00B7 " + std::to_wstring(g_ovFps.load()) + L" fps";
}

// ---------------------------------------------------------------- editor helpers

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static const int kHeader = 48;   // height of the header bar; everything else sits below it
static int g_yOff = 0;
static std::set<HWND> g_outside, g_primary;   // controls on the window background / accent buttons

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y + g_yOff), S(w), S(h),
                             g_editor, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, FALSE);
    return c;
}
static HWND label(const wchar_t* text, int x, int y, int w) { return mk(L"STATIC", text, SS_LEFT | SS_NOPREFIX, x, y + 3, w, 18, -1); }
static HWND button(const wchar_t* text, int x, int y, int w, int id, int h = 24) { return mk(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, h, id); }
static HWND check(const wchar_t* text, int x, int y, int w, int id) { return mk(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, 22, id); }
static HWND combo(int x, int y, int w, int id) { return mk(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, w, 300, id); }

// ---------------------------------------------------------------- theme
// Standard controls mostly ignore colours, so buttons and checkboxes are custom drawn
// (NM_CUSTOMDRAW), sections are cards painted by the editor itself, and the spin buttons and
// hotkey boxes are small custom controls. Same flat look in both themes.

struct Palette {
    COLORREF bg, card, text, textDim, ctrl, ctrlHot, ctrlPressed, border, accent, accentHot, accentPressed;
    COLORREF groupLine, spin, arrow, okBg, okText, badBg, badText, mutedBg;
    COLORREF gridGap, gridEmpty, gridAxis, gridOn, gridDrawn, gridErasedOn, gridErasedOff, gridCentre;
};
static Palette g_pal;
static bool g_dark = false;
static HBRUSH g_bgBrush = nullptr, g_ctrlBrush = nullptr, g_cardBrush = nullptr;

typedef HRESULT (WINAPI *PFN_SetWindowTheme)(HWND, LPCWSTR, LPCWSTR);
static PFN_SetWindowTheme pSetWindowTheme;

static bool systemUsesDarkApps() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

static void loadPalette() {
    g_dark = g_set.theme == 2 || (g_set.theme == 0 && systemUsesDarkApps());
    Palette& p = g_pal;
    if (g_dark) {
        p.bg = RGB(28, 28, 28);        p.card = RGB(40, 40, 40);
        p.text = RGB(235, 235, 235);   p.textDim = RGB(140, 140, 140);
        p.ctrl = RGB(52, 52, 52);      p.ctrlHot = RGB(64, 64, 64);    p.ctrlPressed = RGB(78, 78, 78);
        p.border = RGB(80, 80, 80);    p.groupLine = RGB(54, 54, 54);
        p.accent = RGB(0, 120, 215);   p.accentHot = RGB(26, 138, 230); p.accentPressed = RGB(0, 98, 180);
        p.spin = RGB(58, 58, 58);      p.arrow = RGB(205, 205, 205);
        p.okBg = RGB(30, 60, 33);      p.okText = RGB(108, 203, 95);
        p.badBg = RGB(72, 38, 38);     p.badText = RGB(255, 153, 164); p.mutedBg = RGB(52, 52, 52);
        p.gridGap = RGB(24, 24, 24);   p.gridEmpty = RGB(46, 46, 46);  p.gridAxis = RGB(54, 59, 74);
        p.gridOn = RGB(225, 225, 225); p.gridDrawn = RGB(40, 140, 255);
        p.gridErasedOn = RGB(170, 70, 70); p.gridErasedOff = RGB(88, 52, 52); p.gridCentre = RGB(255, 70, 70);
    } else {
        p.bg = RGB(243, 243, 243);     p.card = RGB(255, 255, 255);
        p.text = RGB(27, 27, 27);      p.textDim = RGB(128, 128, 128);
        p.ctrl = RGB(251, 251, 251);   p.ctrlHot = RGB(240, 245, 252); p.ctrlPressed = RGB(222, 233, 247);
        p.border = RGB(205, 205, 205); p.groupLine = RGB(228, 228, 228);
        p.accent = RGB(0, 103, 192);   p.accentHot = RGB(25, 117, 197); p.accentPressed = RGB(0, 84, 160);
        p.spin = RGB(242, 242, 242);   p.arrow = RGB(90, 90, 90);
        p.okBg = RGB(223, 246, 221);   p.okText = RGB(16, 124, 16);
        p.badBg = RGB(253, 231, 233);  p.badText = RGB(196, 43, 28);   p.mutedBg = RGB(232, 232, 232);
        p.gridGap = RGB(205, 205, 205); p.gridEmpty = RGB(255, 255, 255); p.gridAxis = RGB(232, 238, 250);
        p.gridOn = RGB(40, 40, 40);    p.gridDrawn = RGB(0, 105, 230);
        p.gridErasedOn = RGB(255, 160, 160); p.gridErasedOff = RGB(255, 228, 228); p.gridCentre = RGB(230, 30, 30);
    }
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_ctrlBrush) DeleteObject(g_ctrlBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    g_bgBrush = CreateSolidBrush(p.bg);
    g_ctrlBrush = CreateSolidBrush(p.ctrl);
    g_cardBrush = CreateSolidBrush(p.card);
}

static DWORD windowsBuild() {
    typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOW*);
    auto f = (PFN_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    return (f && f(&v) == 0) ? v.dwBuildNumber : 0;
}

// Dark tray menu. These uxtheme exports are undocumented (Explorer, Notepad++ etc. use them),
// so they are only looked up on Windows builds where they exist with this signature.
static void setMenuDarkMode(bool dark) {
    typedef int  (WINAPI *PFN_SetPreferredAppMode)(int);
    typedef void (WINAPI *PFN_FlushMenuThemes)();
    static bool looked = false;
    static PFN_SetPreferredAppMode setMode;
    static PFN_FlushMenuThemes flush;
    if (!looked) {
        looked = true;
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux && windowsBuild() >= 18362) {
            setMode = (PFN_SetPreferredAppMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
            flush = (PFN_FlushMenuThemes)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
    }
    if (setMode) setMode(dark ? 2 : 3);   // ForceDark / ForceLight
    if (flush) flush();
}

static void fillSolid(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
static void fillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// Group frames, painted by the editor window (works in both themes).
struct GroupBox { int x, y, w, h; std::wstring title; };
static std::vector<GroupBox> g_groups;

static int group(const wchar_t* text, int x, int y, int w, int h) {
    g_groups.push_back({ x, y + g_yOff, w, h, text });
    return (int)g_groups.size() - 1;
}
static void setGroupTitle(int i, const std::wstring& title) {
    if (i < 0 || g_groups[i].title == title) return;
    GroupBox& g = g_groups[i];
    g.title = title;
    RECT r = { S(g.x), S(g.y), S(g.x + g.w), S(g.y + 20) };
    InvalidateRect(g_editor, &r, TRUE);
}
static HFONT g_fontBold = nullptr, g_fontTitle = nullptr;

// Sections are cards: a filled rounded panel with a semibold title inside the top edge.
static void paintGroups(HDC dc) {
    HGDIOBJ oldFont = SelectObject(dc, g_fontBold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_pal.text);
    for (auto& g : g_groups) {
        RECT r = { S(g.x), S(g.y), S(g.x + g.w), S(g.y + g.h) };
        fillRound(dc, r, g_pal.card, g_pal.groupLine, S(10));
        TextOutW(dc, r.left + S(10), r.top + S(3), g.title.c_str(), (int)g.title.size());
    }
    SelectObject(dc, oldFont);
}

// Header bar: app name, overlay status pill, current preset. Buttons on the right are controls.
static std::wstring g_headerStatus;
static int g_headerKind = -1;

static void paintHeader(HDC dc) {
    RECT rc;
    GetClientRect(g_editor, &rc);
    DrawIconEx(dc, S(14), S(12), g_iconBig, S(24), S(24), 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_fontTitle);
    SetTextColor(dc, g_pal.text);
    TextOutW(dc, S(46), S(10), L"Crosshair", 9);
    SIZE sz;
    GetTextExtentPoint32W(dc, L"Crosshair", 9, &sz);
    SelectObject(dc, g_font);
    SetTextColor(dc, g_pal.textDim);
    std::wstring ver = L"v" CROSSHAIR_VERSION;
    TextOutW(dc, S(46) + sz.cx + S(6), S(17), ver.c_str(), (int)ver.size());

    int kind = 1;
    std::wstring st = overlayStatus(kind);
    g_headerStatus = st;
    g_headerKind = kind;
    COLORREF bgc = kind == 0 ? g_pal.okBg : kind == 2 ? g_pal.badBg : g_pal.mutedBg;
    COLORREF fgc = kind == 0 ? g_pal.okText : kind == 2 ? g_pal.badText : g_pal.textDim;
    SelectObject(dc, g_fontBold);
    GetTextExtentPoint32W(dc, st.c_str(), (int)st.size(), &sz);
    RECT pill = { S(180), S(12), S(180) + sz.cx + S(32), S(36) };
    fillRound(dc, pill, bgc, bgc, pill.bottom - pill.top);
    int cy = (pill.top + pill.bottom) / 2, dot = S(4);
    RECT dr = { pill.left + S(10), cy - dot, pill.left + S(10) + 2 * dot, cy + dot };
    fillRound(dc, dr, fgc, fgc, 2 * dot);
    SetTextColor(dc, fgc);
    RECT tr = { pill.left + S(22), pill.top, pill.right, pill.bottom };
    DrawTextW(dc, st.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, g_font);
    SetTextColor(dc, g_pal.textDim);
    std::wstring pre = g_presets.empty() ? L"" : L"Preset: " + cur().name + (cur().dirty ? L" *" : L"");
    RECT pr = { pill.right + S(14), pill.top, S(760), pill.bottom };
    DrawTextW(dc, pre.c_str(), -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}
static void invalidateHeader() {
    if (!g_editor) return;
    RECT r = { 0, 0, S(765), S(kHeader) };
    InvalidateRect(g_editor, &r, TRUE);
}

// Flat push buttons and checkboxes in both themes (comctl32 v6 custom draw).
static LRESULT customDrawButton(const NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND h = cd->hdr.hwndFrom;
    LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    bool isCheck = type == BS_AUTOCHECKBOX || type == BS_CHECKBOX;
    if (!isCheck && type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return CDRF_DODEFAULT;

    HDC dc = cd->hdc;
    RECT rc = cd->rc;
    bool disabled = !IsWindowEnabled(h);
    bool hot = (cd->uItemState & CDIS_HOT) != 0, pressed = (cd->uItemState & CDIS_SELECTED) != 0;
    bool focus = (cd->uItemState & CDIS_FOCUS) != 0;
    wchar_t text[128];
    GetWindowTextW(h, text, 128);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? g_pal.textDim : g_pal.text);
    COLORREF back = g_outside.count(h) ? g_pal.bg : g_pal.card;
    fillSolid(dc, rc, back);
    if (isCheck) {
        bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        int bs = S(14), top = (rc.top + rc.bottom - bs) / 2;
        RECT box = { rc.left + S(1), top, rc.left + S(1) + bs, top + bs };
        COLORREF fill = checked ? (disabled ? g_pal.border : g_pal.accent)
                                : (pressed ? g_pal.ctrlPressed : hot ? g_pal.ctrlHot : g_pal.ctrl);
        fillRound(dc, box, fill, checked ? fill : ((hot || focus) && !disabled ? g_pal.accent : g_pal.border), S(4));
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, pen);
            POINT pts[3] = { { box.left + bs * 22 / 100, box.top + bs * 52 / 100 },
                             { box.left + bs * 42 / 100, box.top + bs * 72 / 100 },
                             { box.left + bs * 78 / 100, box.top + bs * 30 / 100 } };
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        RECT tr = rc;
        tr.left = box.right + S(6);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else if (g_primary.count(h) && !disabled) {
        COLORREF fill = pressed ? g_pal.accentPressed : hot ? g_pal.accentHot : g_pal.accent;
        fillRound(dc, rc, fill, focus ? g_pal.text : fill, S(8));
        SelectObject(dc, g_fontBold);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        COLORREF fill = disabled ? back : pressed ? g_pal.ctrlPressed : hot ? g_pal.ctrlHot : g_pal.ctrl;
        fillRound(dc, rc, fill, focus ? g_pal.accent : g_pal.border, S(8));
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, oldFont);
    return CDRF_SKIPDEFAULT;
}

// ---- spin buttons next to number boxes (click, hold to repeat, Shift = 10, wheel)

struct SpinData { HWND buddy; int lo, hi, pressed, hot, step; };   // pressed / hot: +1 up, -1 down

static void stepEdit(HWND edit, int lo, int hi, int delta) {
    wchar_t b[32];
    GetWindowTextW(edit, b, 32);
    long v = wcstol(b, nullptr, 10);
    SetWindowTextW(edit, std::to_wstring(std::clamp<long>(v + delta, lo, hi)).c_str());   // fires EN_CHANGE
}

static LRESULT CALLBACK spinProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SpinData* d = (SpinData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    RECT rc;
    GetClientRect(h, &rc);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int mid = rc.bottom / 2, s = std::max(2, S(3));
        bool en = IsWindowEnabled(h) != FALSE;
        for (int half = 0; half < 2; ++half) {
            int dir = half == 0 ? 1 : -1;
            RECT r = { 0, half ? mid : 0, rc.right, half ? rc.bottom : mid };
            COLORREF col = !d || !en ? g_pal.spin : d->pressed == dir ? g_pal.ctrlPressed : d->hot == dir ? g_pal.ctrlHot : g_pal.spin;
            fillSolid(dc, r, col);
            int cx = rc.right / 2, cy = (r.top + r.bottom) / 2, k = dir * ((s + 1) / 2);
            POINT tri[3] = { { cx - s, cy + k }, { cx + s + 1, cy + k }, { cx, cy - k - dir } };
            COLORREF ac = en ? g_pal.arrow : g_pal.textDim;
            HBRUSH b = CreateSolidBrush(ac);
            HPEN p = CreatePen(PS_SOLID, 1, ac);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            Polygon(dc, tri, 3);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(b);
            DeleteObject(p);
        }
        SetDCBrushColor(dc, g_pal.border);
        FrameRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (!d) break;
        SetCapture(h);
        d->pressed = GET_Y_LPARAM(lp) < rc.bottom / 2 ? 1 : -1;
        d->step = (wp & MK_SHIFT) ? 10 : 1;
        stepEdit(d->buddy, d->lo, d->hi, d->pressed * d->step);
        SetTimer(h, 1, 400, nullptr);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (d && d->pressed) {
            stepEdit(d->buddy, d->lo, d->hi, d->pressed * d->step);
            SetTimer(h, 1, 50, nullptr);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        KillTimer(h, 1);
        if (d) d->pressed = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (d) {
            int hot = GET_Y_LPARAM(lp) < rc.bottom / 2 ? 1 : -1;
            if (hot != d->hot) {
                d->hot = hot;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d) { d->hot = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSEWHEEL:
        if (d) stepEdit(d->buddy, d->lo, d->hi, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static NumEdit numEdit(int x, int y, int w, int id, int lo, int hi) {
    NumEdit n;
    n.edit = mk(L"EDIT", L"0", WS_TABSTOP | ES_AUTOHSCROLL, x, y, w - 15, 22, id, WS_EX_CLIENTEDGE);
    n.ud = mk(L"CrosshairSpin", L"", 0, x + w - 16, y, 16, 22, -1);
    SetWindowLongPtrW(n.ud, GWLP_USERDATA, (LONG_PTR)new SpinData{ n.edit, lo, hi, 0, 0, 1 });
    n.lo = lo;
    n.hi = hi;
    g_numEdits.push_back(n);
    return n;
}

// ---- hotkey box: click it, press a key combination

static const UINT KB_SETHOTKEY = WM_USER + 1, KB_GETHOTKEY = WM_USER + 2;

static bool isModifierVk(UINT vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
           (vk >= VK_LSHIFT && vk <= VK_RMENU);
}
static UINT heldMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000)    m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000)   m |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= MOD_WIN;
    return m;
}

static LRESULT CALLBACK keyBoxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case KB_SETHOTKEY:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)wp);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case KB_GETHOTKEY:
        return GetWindowLongPtrW(h, GWLP_USERDATA);
    case WM_GETDLGCODE: {
        const MSG* m = (const MSG*)lp;   // plain Tab still moves to the next control
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_TAB && !(heldMods() & (MOD_CONTROL | MOD_ALT)))
            return DLGC_WANTCHARS;
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_KEYUP: case WM_SYSKEYUP: case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        InvalidateRect(h, nullptr, FALSE);
        if (isModifierVk(vk)) return 0;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((heldMods() << 16) | (vk & 0xFF)));
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), EN_CHANGE), (LPARAM)h);
        return 0;
    }
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR: case WM_SYSDEADCHAR:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        bool focus = GetFocus() == h;
        fillSolid(dc, rc, g_pal.ctrl);
        SetDCBrushColor(dc, focus ? g_pal.accent : g_pal.border);
        FrameRect(dc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
        if (focus) { RECT in = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 }; FrameRect(dc, &in, (HBRUSH)GetStockObject(DC_BRUSH)); }
        UINT hk = (UINT)GetWindowLongPtrW(h, GWLP_USERDATA), mods = focus ? heldMods() : 0;
        std::wstring text;
        COLORREF col = g_pal.text;
        if (mods) text = u2w(modsPrefix(mods)) + L"...";
        else if (hk & 0xFFFF) text = u2w(hotkeyToString(hk));
        else { text = focus ? L"Press keys..." : L"None"; col = g_pal.textDim; }
        HGDIOBJ oldFont = SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, col);
        RECT tr = rc;
        tr.left += S(6);
        DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK themeChildProc(HWND h, LPARAM) {
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    if (!lstrcmpiW(cls, L"ComboBox") || !lstrcmpiW(cls, L"Edit"))
        pSetWindowTheme(h, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    else if (!lstrcmpiW(cls, L"ListBox"))
        pSetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);   // dark scrollbar
    return TRUE;
}

static void applyTheme() {
    loadPalette();
    if (!g_editor) return;
    if (!pSetWindowTheme) {
        if (HMODULE ux = LoadLibraryW(L"uxtheme.dll"))
            pSetWindowTheme = (PFN_SetWindowTheme)(void*)GetProcAddress(ux, "SetWindowTheme");
    }
    BOOL dark = g_dark;   // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 on Windows 10 2004+, 19 before
    if (FAILED(DwmSetWindowAttribute(g_editor, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(g_editor, 19, &dark, sizeof(dark));
    setMenuDarkMode(g_dark);
    if (pSetWindowTheme) EnumChildWindows(g_editor, themeChildProc, 0);
    if (g_legend)
        SetWindowTextW(g_legend, g_dark ? L"Blue = drawn, white = component, red = erased."
                                        : L"Blue = drawn, black = component, pink = erased.");
    SetWindowPos(g_editor, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(g_editor, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void setCheck(HWND h, bool v) { SendMessageW(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
static bool getCheck(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void setInt(const NumEdit& e, int v) {
    ++g_populating;
    SetWindowTextW(e.edit, std::to_wstring(v).c_str());
    --g_populating;
}
static int getInt(const NumEdit& e, int lo, int hi, int fallback) {
    wchar_t b[32];
    GetWindowTextW(e.edit, b, 32);
    wchar_t* end = nullptr;
    long v = wcstol(b, &end, 10);
    if (end == b) return fallback;
    return (int)std::clamp<long>(v, lo, hi);
}
static void showNum(const NumEdit& e, bool show) {
    ShowWindow(e.edit, show ? SW_SHOW : SW_HIDE);
    ShowWindow(e.ud, show ? SW_SHOW : SW_HIDE);
}
static std::wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

// ---------------------------------------------------------------- hotkey registration

static void unregisterHotkeys() {
    for (int i = 0; i < HK_COUNT; ++i) UnregisterHotKey(nullptr, i + 1);
    g_hkRegistered = false;
}
static void registerHotkeys() {
    unregisterHotkeys();
    std::wstring fails;
    for (int i = 0; i < HK_COUNT; ++i) {
        UINT hk = g_set.hk[i];
        if (!(hk & 0xFFFF)) continue;
        UINT mods = (hk >> 16) | (kHk[i].repeat ? 0 : MOD_NOREPEAT);
        if (!RegisterHotKey(nullptr, i + 1, mods, hk & 0xFFFF)) {
            if (!fails.empty()) fails += L", ";
            fails += std::wstring(kHk[i].label) + L" (" + u2w(hotkeyToString(hk)) + L")";
        }
    }
    g_hkRegistered = true;
    if (!fails.empty()) setStatus(L"Hotkey already in use by another program: " + fails);
}

// While a hotkey box has focus, release our global hotkeys so they can be typed into it.
static void syncHotkeyRegistration() {
    HWND f = GetFocus();
    bool capturing = false;
    for (int i = 0; i < HK_COUNT; ++i) if (f && f == g_hkCtrl[i]) capturing = true;
    if (capturing && g_hkRegistered) unregisterHotkeys();
    else if (!capturing && !g_hkRegistered) registerHotkeys();
}

// ---------------------------------------------------------------- populate editor

static std::wstring compText(const Component& c) {
    const TypeInfo& t = kTypes[c.type];
    std::wstring s = c.subtract ? L"[erase] " : L"";
    s += t.name;
    s += L"  ";
    bool first = true;
    for (int i = 0; i < 3; ++i) {
        if (!t.key[i]) continue;
        if (!first) s += L"/";
        s += std::to_wstring(c.p[i]);
        first = false;
    }
    if (c.x || c.y) s += L"  @" + std::to_wstring(c.x) + L"," + std::to_wstring(c.y);
    if (!c.enabled) s += L"  (off)";
    return s;
}

static void updateTitle() {
    if (!g_editor) return;
    std::wstring t = L"Crosshair Editor - " + cur().name + (cur().dirty ? L" *" : L"");
    SetWindowTextW(g_editor, t.c_str());
    invalidateHeader();
}

static void populatePresetCombo() {
    ++g_populating;
    SendMessageW(g_presetCombo, CB_RESETCONTENT, 0, 0);
    for (auto& p : g_presets) {
        std::wstring s = p.name + (p.dirty ? L" *" : L"");
        SendMessageW(g_presetCombo, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    SendMessageW(g_presetCombo, CB_SETCURSEL, g_cur, 0);
    --g_populating;
}

static void populateCompList() {
    ++g_populating;
    SendMessageW(g_compList, LB_RESETCONTENT, 0, 0);
    for (auto& c : cur().comps) SendMessageW(g_compList, LB_ADDSTRING, 0, (LPARAM)compText(c).c_str());
    if (g_selComp >= (int)cur().comps.size()) g_selComp = (int)cur().comps.size() - 1;
    SendMessageW(g_compList, LB_SETCURSEL, g_selComp, 0);
    --g_populating;
}

static void populateCompPanel() {
    bool has = g_selComp >= 0 && g_selComp < (int)cur().comps.size();
    for (HWND h : g_compCtrls) EnableWindow(h, has);
    Component c = has ? cur().comps[g_selComp] : makeComponent(0);
    const TypeInfo& t = kTypes[c.type];
    ++g_populating;
    setGroupTitle(g_compGroupIdx, has ? std::wstring(L"Selected component: ") + t.name : L"Selected component: none");
    setCheck(g_cEnabled, c.enabled);
    setCheck(g_cSubtract, c.subtract);
    for (int i = 0; i < 3; ++i) {
        bool vis = t.key[i] != nullptr;
        ShowWindow(g_pLabel[i], vis ? SW_SHOW : SW_HIDE);
        showNum(g_pEdit[i], vis);
        if (vis) { SetWindowTextW(g_pLabel[i], t.label[i]); setInt(g_pEdit[i], c.p[i]); }
    }
    setInt(g_cX, c.x);
    setInt(g_cY, c.y);
    for (int i = 0; i < 4; ++i) {
        bool vis = t.fkey[i] != nullptr;
        ShowWindow(g_cFlag[i], vis ? SW_SHOW : SW_HIDE);
        if (vis) { SetWindowTextW(g_cFlag[i], t.flabel[i]); setCheck(g_cFlag[i], c.f[i]); }
    }
    --g_populating;
}

// Max contrast replaces the other effects, so grey them out while it is on.
static void enableFxControls() {
    bool others = !cur().fx.on[FX_MAXCON];
    for (int i = 0; i < FX_COUNT; ++i) {
        if (i == FX_MAXCON) continue;
        EnableWindow(g_fxCheck[i], others);
        if (g_fxVal[i].edit) { EnableWindow(g_fxVal[i].edit, others); EnableWindow(g_fxVal[i].ud, others); }
    }
    for (int i = 0; i < 3; ++i) { EnableWindow(g_rgb[i].edit, others); EnableWindow(g_rgb[i].ud, others); }
}

static void populateFx() {
    const Effects& fx = cur().fx;
    for (int i = 0; i < FX_COUNT; ++i) {
        setCheck(g_fxCheck[i], fx.on[i]);
        if (g_fxVal[i].edit) setInt(g_fxVal[i], fx.val[i]);
    }
    for (int i = 0; i < 3; ++i) setInt(g_rgb[i], fx.rgb[i]);
    enableFxControls();
}

static void populateOffsets() {
    setInt(g_offX, cur().offX);
    setInt(g_offY, cur().offY);
}

static void populateHotkeys() {
    ++g_populating;
    for (int i = 0; i < HK_COUNT; ++i) SendMessageW(g_hkCtrl[i], KB_SETHOTKEY, g_set.hk[i], 0);
    --g_populating;
}

static void fillMonitors() {
    auto mons = enumMonitors();
    ++g_populating;
    SendMessageW(g_monitorCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_monitorCombo, CB_ADDSTRING, 0, (LPARAM)L"Primary monitor");
    for (size_t i = 0; i < mons.size(); ++i) {
        const RECT& r = mons[i].rc;
        wchar_t buf[200];
        swprintf(buf, 200, L"%d: %ls  %dx%d at (%d, %d)%ls", (int)i + 1, mons[i].name.c_str(),
                 (int)(r.right - r.left), (int)(r.bottom - r.top), (int)r.left, (int)r.top,
                 mons[i].primary ? L"  [primary]" : L"");
        SendMessageW(g_monitorCombo, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    int sel = g_set.monitor <= (int)mons.size() ? g_set.monitor : 0;
    SendMessageW(g_monitorCombo, CB_SETCURSEL, sel, 0);
    --g_populating;
}

static void populateAll() {
    if (!g_editor) return;
    populatePresetCombo();
    ++g_populating;
    SetWindowTextW(g_nameEdit, cur().name.c_str());
    --g_populating;
    populateCompList();
    populateCompPanel();
    populateFx();
    populateOffsets();
    updateTitle();
    if (g_grid) InvalidateRect(g_grid, nullptr, FALSE);
}

// Called after any edit to the current preset.
static void changed() {
    Preset& p = cur();
    if (!p.dirty) {
        p.dirty = true;
        if (g_editor) populatePresetCombo();
    }
    rebuildOverlay();
    updateTitle();
}

// ---------------------------------------------------------------- undo

struct Snapshot { std::vector<Component> comps; std::map<Px, bool> pixels; int offX, offY; Effects fx; };
static std::vector<Snapshot> g_undo, g_redo;
static int g_undoTag = 0;
static DWORD g_undoTime = 0;

static Snapshot snapshot() { const Preset& p = cur(); return { p.comps, p.pixels, p.offX, p.offY, p.fx }; }
static void restore(const Snapshot& s) {
    Preset& p = cur();
    p.comps = s.comps; p.pixels = s.pixels; p.offX = s.offX; p.offY = s.offY; p.fx = s.fx;
}
// tag != 0: consecutive edits of the same control within a moment become one undo step.
static void pushUndo(int tag) {
    DWORD now = GetTickCount();
    if (tag && tag == g_undoTag && now - g_undoTime < 1500) { g_undoTime = now; return; }
    g_undo.push_back(snapshot());
    if (g_undo.size() > 300) g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_undoTag = tag;
    g_undoTime = now;
}
static void undoRedo(bool isUndo) {
    auto& from = isUndo ? g_undo : g_redo;
    auto& to = isUndo ? g_redo : g_undo;
    if (from.empty()) { setStatus(isUndo ? L"Nothing to undo." : L"Nothing to redo."); return; }
    to.push_back(snapshot());
    restore(from.back());
    from.pop_back();
    g_undoTag = 0;
    populateAll();
    changed();
}

// ---------------------------------------------------------------- presets (operations)

static void sortPresets() {
    std::sort(g_presets.begin(), g_presets.end(), [](const Preset& a, const Preset& b) {
        return lstrcmpiW(a.name.c_str(), b.name.c_str()) < 0;
    });
}
static int findPreset(const std::wstring& name) {
    for (int i = 0; i < (int)g_presets.size(); ++i) if (sameName(g_presets[i].name, name)) return i;
    return -1;
}
static std::wstring sanitizeName(std::wstring s) {
    std::wstring out;
    for (wchar_t ch : s) if (ch >= 32 && !wcschr(L"\\/:*?\"<>|", ch)) out += ch;
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    return out;
}

static void switchPreset(int i) {
    int n = (int)g_presets.size();
    if (!n) return;
    g_cur = ((i % n) + n) % n;
    g_set.preset = cur().name;
    saveSettings();
    g_undo.clear(); g_redo.clear(); g_undoTag = 0;
    g_selComp = cur().comps.empty() ? -1 : 0;
    populateAll();
    rebuildOverlay();
    setStatus(L"Preset: " + cur().name);
}

static bool savePresetIdx(int i) {
    std::error_code ec;
    fs::create_directories(g_presetDir, ec);
    Preset& p = g_presets[i];
    if (!writePreset(presetPath(p.name), p)) {
        MessageBoxW(g_editor, (L"Could not write " + presetPath(p.name).wstring()).c_str(), L"Crosshair", MB_ICONERROR);
        return false;
    }
    p.dirty = false;
    if (g_editor) { populatePresetCombo(); updateTitle(); }
    setStatus(L"Saved preset \"" + p.name + L"\".");
    return true;
}

// Saves the current preset under the name in the name box (Save As when the name differs).
static void savePresetAs(const std::wstring& rawName) {
    std::wstring name = sanitizeName(rawName);
    if (name.empty()) { setStatus(L"Type a preset name first."); return; }
    if (sameName(name, cur().name)) {
        cur().name = name;
        savePresetIdx(g_cur);
        return;
    }
    int existing = findPreset(name);
    if (existing >= 0 &&
        MessageBoxW(g_editor, (L"Overwrite preset \"" + g_presets[existing].name + L"\"?").c_str(), L"Crosshair",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    Preset copy = cur();
    copy.name = name;
    std::wstring oldName = cur().name;
    fs::path oldFile = presetPath(oldName);
    if (fs::exists(oldFile)) {
        // The original keeps its last saved state; the edits go to the new preset.
        Preset r;
        if (loadPreset(oldFile, r, nullptr)) { r.name = oldName; cur() = r; }
        else cur().dirty = false;
    } else {
        g_presets.erase(g_presets.begin() + g_cur);   // never saved: this is just a rename
    }
    existing = findPreset(name);
    if (existing >= 0) g_presets[existing] = copy; else g_presets.push_back(copy);
    sortPresets();
    g_cur = findPreset(name);
    if (savePresetIdx(g_cur)) switchPreset(g_cur);
}

static void newPreset() {
    std::wstring name = L"new";
    for (int n = 2; findPreset(name) >= 0; ++n) name = L"new " + std::to_wstring(n);
    Preset p = defaultPreset(name);
    p.dirty = true;
    g_presets.push_back(p);
    sortPresets();
    switchPreset(findPreset(name));
    setStatus(L"New preset created. Type a name and press Save.");
    SetFocus(g_nameEdit);
    SendMessageW(g_nameEdit, EM_SETSEL, 0, -1);
}

static void deletePreset() {
    std::wstring name = cur().name;
    if (MessageBoxW(g_editor, (L"Delete preset \"" + name + L"\"? This removes its file.").c_str(), L"Crosshair",
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    std::error_code ec;
    fs::remove(presetPath(name), ec);
    g_presets.erase(g_presets.begin() + g_cur);
    if (g_presets.empty()) {
        g_presets.push_back(defaultPreset(L"default"));
        savePresetIdx(0);
    }
    switchPreset(std::min(g_cur, (int)g_presets.size() - 1));
    setStatus(L"Deleted preset \"" + name + L"\".");
}

static void reloadPreset() {
    fs::path p = presetPath(cur().name);
    Preset r;
    if (!fs::exists(p) || !loadPreset(p, r, nullptr)) { setStatus(L"This preset has not been saved yet."); return; }
    r.name = cur().name;
    cur() = r;
    switchPreset(g_cur);
    setStatus(L"Reloaded preset \"" + r.name + L"\" from disk.");
}

// ---------------------------------------------------------------- reading edits back

static void readCompPanel() {
    if (g_selComp < 0 || g_selComp >= (int)cur().comps.size()) return;
    Component& c = cur().comps[g_selComp];
    const TypeInfo& t = kTypes[c.type];
    c.enabled = getCheck(g_cEnabled);
    c.subtract = getCheck(g_cSubtract);
    for (int i = 0; i < 3; ++i) if (t.key[i]) c.p[i] = getInt(g_pEdit[i], 0, kMaxParam, c.p[i]);
    c.x = getInt(g_cX, -kMaxOffset, kMaxOffset, c.x);
    c.y = getInt(g_cY, -kMaxOffset, kMaxOffset, c.y);
    for (int i = 0; i < 4; ++i) if (t.fkey[i]) c.f[i] = getCheck(g_cFlag[i]);
    ++g_populating;
    SendMessageW(g_compList, LB_DELETESTRING, g_selComp, 0);
    SendMessageW(g_compList, LB_INSERTSTRING, g_selComp, (LPARAM)compText(c).c_str());
    SendMessageW(g_compList, LB_SETCURSEL, g_selComp, 0);
    --g_populating;
}

static void readFx() {
    Effects& fx = cur().fx;
    for (int i = 0; i < FX_COUNT; ++i) {
        fx.on[i] = getCheck(g_fxCheck[i]);
        if (g_fxVal[i].edit) fx.val[i] = getInt(g_fxVal[i], kFx[i].lo, kFx[i].hi, fx.val[i]);
    }
    for (int i = 0; i < 3; ++i) fx.rgb[i] = getInt(g_rgb[i], 0, 255, fx.rgb[i]);
    enableFxControls();
    if (fx.on[FX_MAXCON])
        setStatus(L"Max contrast: every pixel takes the colour furthest from what is under it "
                  L"(100% = per colour channel, 0% = black / white by brightness).");
    else if (!fx.any()) setStatus(L"No effect enabled: the crosshair pixels look exactly like the screen (invisible).");
}

// ---------------------------------------------------------------- component operations

static void compOp(int id) {
    auto& comps = cur().comps;
    int n = (int)comps.size(), s = g_selComp;
    bool valid = s >= 0 && s < n;
    switch (id) {
    case ID_COMP_ADD: {
        int type = (int)SendMessageW(g_compType, CB_GETCURSEL, 0, 0);
        pushUndo(0);
        comps.push_back(makeComponent(std::clamp(type, 0, kTypeCount - 1)));
        g_selComp = (int)comps.size() - 1;
        break;
    }
    case ID_COMP_DUP:
        if (!valid) return;
        pushUndo(0);
        comps.insert(comps.begin() + s + 1, comps[s]);
        g_selComp = s + 1;
        break;
    case ID_COMP_REMOVE:
        if (!valid) return;
        pushUndo(0);
        comps.erase(comps.begin() + s);
        g_selComp = std::min(s, (int)comps.size() - 1);
        break;
    case ID_COMP_UP:
        if (!valid || s == 0) return;
        pushUndo(0);
        std::swap(comps[s], comps[s - 1]);
        g_selComp = s - 1;
        break;
    case ID_COMP_DOWN:
        if (!valid || s >= n - 1) return;
        pushUndo(0);
        std::swap(comps[s], comps[s + 1]);
        g_selComp = s + 1;
        break;
    }
    populateCompList();
    populateCompPanel();
    changed();
}

// ---------------------------------------------------------------- pixel grid

enum { PAINT_NONE, PAINT_ON, PAINT_OFF, PAINT_CLEAR };
static int g_paintMode = PAINT_NONE, g_dragR = 0, g_lastX = 0, g_lastY = 0;

static int autoRadius() {
    int r = 5;
    for (auto& q : g_final) r = std::max(r, std::max(std::abs(q.first), std::abs(q.second)));
    for (auto& kv : cur().pixels) r = std::max(r, std::max(std::abs(kv.first.first), std::abs(kv.first.second)));
    return std::min(r + 3, 150);
}
static int gridRadius() {
    if (g_paintMode != PAINT_NONE) return g_dragR;
    return g_viewRadius > 0 ? g_viewRadius : autoRadius();
}

struct GridGeo { int R, n, cell, ox, oy; };
static GridGeo gridGeo(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    GridGeo g;
    g.R = gridRadius();
    g.n = 2 * g.R + 1;
    g.cell = std::max(1, (int)std::min(rc.right, rc.bottom) / g.n);
    g.ox = (rc.right - g.cell * g.n) / 2;
    g.oy = (rc.bottom - g.cell * g.n) / 2;
    return g;
}
static bool cellAt(HWND h, int mx, int my, int& x, int& y) {
    GridGeo g = gridGeo(h);
    if (mx < g.ox || my < g.oy) return false;
    int cx = (mx - g.ox) / g.cell, cy = (my - g.oy) / g.cell;
    if (cx >= g.n || cy >= g.n) return false;
    x = cx - g.R;
    y = cy - g.R;
    return true;
}

static void paintLine(int x0, int y0, int x1, int y1) {
    auto& px = cur().pixels;
    bool mh = getCheck(g_mirrorH), mv = getCheck(g_mirrorV);
    auto set1 = [&](int x, int y) {
        Px k{ x, y };
        if (g_paintMode == PAINT_CLEAR) px.erase(k);
        else px[k] = g_paintMode == PAINT_ON;
    };
    auto set = [&](int x, int y) {
        set1(x, y);
        if (mh) set1(-x, y);
        if (mv) set1(x, -y);
        if (mh && mv) set1(-x, -y);
    };
    // Bresenham, so fast drags don't leave gaps
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        set(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    changed();
}

static void paintGrid(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH dcBrush = (HBRUSH)GetStockObject(DC_BRUSH);
    auto fill = [&](int l, int t, int r, int b, COLORREF col) {
        RECT q = { l, t, r, b };
        SetDCBrushColor(mem, col);
        FillRect(mem, &q, dcBrush);
    };
    fill(0, 0, rc.right, rc.bottom, g_pal.card);

    GridGeo g = gridGeo(h);
    int gap = g.cell >= 5 ? 1 : 0;
    fill(g.ox, g.oy, g.ox + g.cell * g.n, g.oy + g.cell * g.n, g_pal.gridGap);
    const auto& px = cur().pixels;
    for (int y = -g.R; y <= g.R; ++y) {
        for (int x = -g.R; x <= g.R; ++x) {
            Px k{ x, y };
            bool base = g_base.count(k) != 0;
            auto it = px.find(k);
            COLORREF col;
            if (it != px.end()) col = it->second ? g_pal.gridDrawn : (base ? g_pal.gridErasedOn : g_pal.gridErasedOff);
            else if (base) col = g_pal.gridOn;
            else col = (x == 0 || y == 0) ? g_pal.gridAxis : g_pal.gridEmpty;
            int l = g.ox + (x + g.R) * g.cell, t = g.oy + (y + g.R) * g.cell;
            fill(l, t, l + g.cell - gap, t + g.cell - gap, col);
        }
    }
    if (g.cell >= 3) {   // centre pixel marker
        int l = g.ox + g.R * g.cell, t = g.oy + g.R * g.cell;
        RECT q = { l - 1, t - 1, l + g.cell, t + g.cell };
        SetDCBrushColor(mem, g_pal.gridCentre);
        FrameRect(mem, &q, dcBrush);
    }
    SetDCBrushColor(mem, g_pal.border);
    FrameRect(mem, &rc, dcBrush);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void updateHover(int x, int y) {
    Px k{ x, y };
    auto it = cur().pixels.find(k);
    std::wstring state;
    if (it != cur().pixels.end()) state = it->second ? L"drawn on" : L"erased";
    else state = g_base.count(k) ? L"on (component)" : L"off";
    wchar_t buf[120];
    swprintf(buf, 120, L"Pixel x = %d, y = %d   %ls", x, y, state.c_str());
    SetWindowTextW(g_hover, buf);
}

static LRESULT CALLBACK gridProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintGrid(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        SetFocus(h);
        int x, y;
        if (!cellAt(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y)) return 0;
        g_dragR = gridRadius();
        pushUndo(0);
        g_paintMode = (wp & MK_SHIFT) ? PAINT_CLEAR : (msg == WM_RBUTTONDOWN ? PAINT_OFF : PAINT_ON);
        SetCapture(h);
        g_lastX = x; g_lastY = y;
        paintLine(x, y, x, y);
        updateHover(x, y);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x, y;
        if (!cellAt(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), x, y)) return 0;
        if (g_paintMode != PAINT_NONE && (x != g_lastX || y != g_lastY)) {
            paintLine(g_lastX, g_lastY, x, y);
            g_lastX = x; g_lastY = y;
        }
        updateHover(x, y);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_paintMode != PAINT_NONE) {
            g_paintMode = PAINT_NONE;
            ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_paintMode = PAINT_NONE;
        return 0;
    case WM_MOUSEWHEEL: {
        int r = gridRadius() + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -2 : 2);
        g_viewRadius = std::clamp(r, 2, 150);
        setInt(g_viewR, g_viewRadius);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- tray & window visibility

static void addTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_editor;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"Crosshair (click to edit)", 64);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void showEditor() {
    if (!g_editor) return;
    ShowWindow(g_editor, IsIconic(g_editor) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_editor);
}
static void hideEditor() { if (g_editor) ShowWindow(g_editor, SW_HIDE); }
static void toggleEditor() {
    if (g_editor && IsWindowVisible(g_editor) && !IsIconic(g_editor) && GetForegroundWindow() == g_editor) hideEditor();
    else showEditor();
}

static HWND g_toggleBtn = nullptr;

static void setOverlayVisible(bool v) {
    g_visible = v;
    if (g_overlayCheck) setCheck(g_overlayCheck, v);
    if (g_toggleBtn) SetWindowTextW(g_toggleBtn, v ? L"Hide crosshair" : L"Show crosshair");
    rebuildOverlay();
    invalidateHeader();
    setStatus(v ? L"Crosshair shown." : L"Crosshair hidden.");
}

static void refreshOverlay() {
    g_ovRecreate = true;   // the overlay thread rebuilds the magnifier on its next frame
    rebuildOverlay();
    setStatus(L"Overlay refreshed: the magnifier was recreated from scratch.");
}

static void requestQuit() {
    int dirty = 0;
    for (auto& p : g_presets) if (p.dirty) ++dirty;
    if (dirty) {
        bool vis = g_editor && IsWindowVisible(g_editor);
        int r = MessageBoxW(vis ? g_editor : nullptr, L"Some presets have unsaved changes. Save them before quitting?",
                            L"Crosshair", MB_YESNOCANCEL | MB_ICONQUESTION | (vis ? 0 : MB_TOPMOST | MB_SETFOREGROUND));
        if (r == IDCANCEL) return;
        if (r == IDYES)
            for (int i = 0; i < (int)g_presets.size(); ++i)
                if (g_presets[i].dirty && !savePresetIdx(i)) return;
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_editor) { HWND e = g_editor; g_editor = nullptr; g_grid = nullptr; g_status = nullptr; DestroyWindow(e); }
    stopOverlay();
    PostQuitMessage(0);
}

static void trayMenu() {
    HMENU m = CreatePopupMenu(), sub = CreatePopupMenu();
    bool edVis = g_editor && IsWindowVisible(g_editor);
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, edVis ? L"Hide editor" : L"Open editor");
    AppendMenuW(m, MF_STRING | (g_visible ? MF_CHECKED : 0), ID_TRAY_TOGGLE, L"Show crosshair");
    AppendMenuW(m, MF_STRING, ID_TRAY_REFRESH, L"Refresh overlay");
    for (int i = 0; i < (int)g_presets.size() && i < 500; ++i)
        AppendMenuW(sub, MF_STRING | (i == g_cur ? MF_CHECKED : 0), ID_TRAY_PRESET + i, g_presets[i].name.c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Preset");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_QUIT, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_editor);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_editor, nullptr);
    PostMessageW(g_editor, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- commands

static void nudge(int dx, int dy) {
    pushUndo(ID_OFF_X);
    cur().offX = std::clamp(cur().offX + dx, -kMaxOffset, kMaxOffset);
    cur().offY = std::clamp(cur().offY + dy, -kMaxOffset, kMaxOffset);
    if (g_editor) populateOffsets();
    changed();
    setStatus(L"Offset " + std::to_wstring(cur().offX) + L", " + std::to_wstring(cur().offY));
}

static void onHotkey(int i) {
    switch (i) {
    case HK_TOGGLE: setOverlayVisible(!g_visible); break;
    case HK_EDITOR: toggleEditor(); break;
    case HK_NEXT:   switchPreset(g_cur + 1); break;
    case HK_PREV:   switchPreset(g_cur - 1); break;
    case HK_LEFT:   nudge(-1, 0); break;
    case HK_RIGHT:  nudge(1, 0); break;
    case HK_UP:     nudge(0, -1); break;
    case HK_DOWN:   nudge(0, 1); break;
    case HK_SAVE:   savePresetIdx(g_cur); break;
    case HK_RELOAD: reloadPreset(); break;
    case HK_REFRESH: refreshOverlay(); break;
    case HK_QUIT:   requestQuit(); break;
    }
}

static void onCommand(int id, int code) {
    if (!g_ready || g_populating) return;

    if (id >= ID_FX_CHECK && id < ID_FX_CHECK + FX_COUNT && code == BN_CLICKED) { pushUndo(id); readFx(); changed(); return; }
    if (((id >= ID_FX_VAL && id < ID_FX_VAL + FX_COUNT) || (id >= ID_FX_RGB && id < ID_FX_RGB + 3)) && code == EN_CHANGE) {
        pushUndo(id); readFx(); changed(); return;
    }
    if (id >= ID_HK && id < ID_HK + HK_COUNT && code == EN_CHANGE) {
        int i = id - ID_HK;
        g_set.hk[i] = (UINT)SendMessageW(g_hkCtrl[i], KB_GETHOTKEY, 0, 0);
        saveSettings();
        setStatus(std::wstring(kHk[i].label) + L": " + u2w(hotkeyToString(g_set.hk[i])) + L" (active once you leave the box)");
        return;
    }
    if (id >= ID_HK_CLEAR && id < ID_HK_CLEAR + HK_COUNT) {
        int i = id - ID_HK_CLEAR;
        g_set.hk[i] = 0;
        populateHotkeys();
        saveSettings();
        registerHotkeys();
        setStatus(std::wstring(kHk[i].label) + L": no hotkey");
        return;
    }
    if (id >= ID_TRAY_PRESET && id < ID_TRAY_PRESET + 500) { switchPreset(id - ID_TRAY_PRESET); return; }

    switch (id) {
    case ID_PRESET_COMBO:
        if (code == CBN_SELCHANGE) switchPreset((int)SendMessageW(g_presetCombo, CB_GETCURSEL, 0, 0));
        break;
    case ID_PRESET_SAVE:   savePresetAs(getText(g_nameEdit)); break;
    case ID_PRESET_NEW:    newPreset(); break;
    case ID_PRESET_DELETE: deletePreset(); break;

    case ID_COMP_LIST:
        if (code == LBN_SELCHANGE) {
            g_selComp = (int)SendMessageW(g_compList, LB_GETCURSEL, 0, 0);
            populateCompPanel();
        }
        break;
    case ID_COMP_ADD: case ID_COMP_DUP: case ID_COMP_REMOVE: case ID_COMP_UP: case ID_COMP_DOWN:
        compOp(id);
        break;
    case ID_COMP_ENABLED: case ID_COMP_SUBTRACT:
    case ID_COMP_F0: case ID_COMP_F1: case ID_COMP_F2: case ID_COMP_F3:
        if (code == BN_CLICKED) { pushUndo(id); readCompPanel(); changed(); }
        break;
    case ID_COMP_P0: case ID_COMP_P1: case ID_COMP_P2: case ID_COMP_X: case ID_COMP_Y:
        if (code == EN_CHANGE) { pushUndo(id); readCompPanel(); changed(); }
        break;

    case ID_OFF_X: case ID_OFF_Y:
        if (code == EN_CHANGE) {
            pushUndo(ID_OFF_X);
            cur().offX = getInt(g_offX, -kMaxOffset, kMaxOffset, cur().offX);
            cur().offY = getInt(g_offY, -kMaxOffset, kMaxOffset, cur().offY);
            changed();
        }
        break;
    case ID_CENTRE:
        pushUndo(0);
        cur().offX = cur().offY = 0;
        populateOffsets();
        changed();
        break;

    case ID_MONITOR:
        if (code == CBN_SELCHANGE) {
            g_set.monitor = std::max(0, (int)SendMessageW(g_monitorCombo, CB_GETCURSEL, 0, 0));
            saveSettings();
            rebuildOverlay();
        }
        break;
    case ID_FPS:
        if (code == EN_CHANGE) { g_set.fps = getInt(g_fps, 0, 1000, g_set.fps); saveSettings(); rebuildOverlay(); }
        break;
    case ID_OVERLAY_VISIBLE: setOverlayVisible(getCheck(g_overlayCheck)); break;
    case ID_START_HIDDEN:    g_set.startHidden = getCheck(g_startHidden); saveSettings(); break;
    case ID_THEME:
        if (code == CBN_SELCHANGE) {
            g_set.theme = std::clamp((int)SendMessageW(g_themeCombo, CB_GETCURSEL, 0, 0), 0, 2);
            saveSettings();
            applyTheme();
        }
        break;

    case ID_VIEW_RADIUS:
        if (code == EN_CHANGE) { g_viewRadius = getInt(g_viewR, 0, 150, 0); InvalidateRect(g_grid, nullptr, FALSE); }
        break;
    case ID_CLEAR_PIXELS:
        if (cur().pixels.empty()) break;
        pushUndo(0);
        cur().pixels.clear();
        changed();
        setStatus(L"Pixel edits cleared (Ctrl+Z to undo).");
        break;
    case ID_BAKE: {
        pushUndo(0);
        PxSet fin = g_final;
        cur().comps.clear();
        cur().pixels.clear();
        for (auto& q : fin) cur().pixels[q] = true;
        g_selComp = -1;
        populateCompList();
        populateCompPanel();
        changed();
        setStatus(L"Components baked into pixels (Ctrl+Z to undo).");
        break;
    }
    case ID_UNDO: undoRedo(true); break;
    case ID_REDO: undoRedo(false); break;

    case ID_HK_DEFAULTS:
        for (int i = 0; i < HK_COUNT; ++i) g_set.hk[i] = defaultHotkey(i);
        populateHotkeys();
        saveSettings();
        registerHotkeys();
        setStatus(L"Hotkeys reset to defaults.");
        break;
    case ID_HIDE:        hideEditor(); break;
    case ID_QUIT:        requestQuit(); break;
    case ID_TRAY_OPEN:   if (g_editor && IsWindowVisible(g_editor)) hideEditor(); else showEditor(); break;
    case ID_TRAY_TOGGLE: setOverlayVisible(!g_visible); break;
    case ID_TRAY_QUIT:   requestQuit(); break;
    case ID_TRAY_REFRESH:
    case ID_REFRESH:     refreshOverlay(); break;
    case ID_TOGGLE_OVERLAY: setOverlayVisible(!g_visible); break;
    }
}

static LRESULT CALLBACK editorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        onCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_CLOSE:
        hideEditor();
        setStatus(L"");
        return 0;
    case WM_TIMER: {   // keep the header's live status current
        int kind = 1;
        std::wstring st = overlayStatus(kind);
        if (st != g_headerStatus || kind != g_headerKind) invalidateHeader();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (g_ready) fillMonitors();
        break;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) { if (IsWindowVisible(h)) hideEditor(); else showEditor(); }
        else if (LOWORD(lp) == WM_RBUTTONUP) trayMenu();
        return 0;
    case WM_APP_SHOW:
        showEditor();
        return 0;
    case WM_APP_QUIT:
        requestQuit();
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, g_bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintHeader(dc);
        paintGroups(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        bool outside = g_outside.count((HWND)lp) != 0;
        SetTextColor((HDC)wp, (HWND)lp == g_status ? g_pal.textDim : g_pal.text);
        SetBkColor((HDC)wp, outside ? g_pal.bg : g_pal.card);
        return (LRESULT)(outside ? g_bgBrush : g_cardBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, g_pal.text);
        SetBkColor((HDC)wp, g_pal.ctrl);
        return (LRESULT)g_ctrlBrush;
    case WM_NOTIFY: {
        const NMHDR* nh = (const NMHDR*)lp;
        if (nh->code == NM_CUSTOMDRAW) {
            wchar_t cls[16];
            GetClassNameW(nh->hwndFrom, cls, 16);
            if (!lstrcmpiW(cls, L"Button")) return customDrawButton((const NMCUSTOMDRAW*)lp);
        }
        break;
    }
    case WM_SETTINGCHANGE:   // Windows switched between light and dark apps
        if (g_set.theme == 0 && lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet")) applyTheme();
        break;
    case WM_SYSCOLORCHANGE:
        applyTheme();
        break;
    default:
        if (msg == g_taskbarCreated && g_taskbarCreated) { addTray(); return 0; }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Editor-wide shortcuts. Returns true if the message was consumed.
static bool handleEditorKeys(const MSG& msg) {
    if (msg.message != WM_KEYDOWN || !g_editor) return false;
    if (msg.hwnd != g_editor && !IsChild(g_editor, msg.hwnd)) return false;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    WPARAM k = msg.wParam;
    if (!ctrl && !alt && (k == VK_UP || k == VK_DOWN || k == VK_PRIOR || k == VK_NEXT)) {
        for (auto& n : g_numEdits) {
            if (n.edit != msg.hwnd) continue;
            int d = (k == VK_UP || k == VK_PRIOR) ? 1 : -1;
            if (k == VK_PRIOR || k == VK_NEXT || (GetKeyState(VK_SHIFT) & 0x8000)) d *= 10;
            stepEdit(n.edit, n.lo, n.hi, d);
            return true;
        }
        return false;
    }
    if (!ctrl || alt) return false;
    for (int i = 0; i < HK_COUNT; ++i) if (msg.hwnd == g_hkCtrl[i]) return false;
    if (msg.wParam == 'Z' && msg.hwnd != g_nameEdit) { undoRedo(true); return true; }
    if (msg.wParam == 'Y' && msg.hwnd != g_nameEdit) { undoRedo(false); return true; }
    if (msg.wParam == 'S') { savePresetAs(getText(g_nameEdit)); return true; }
    return false;
}

// ---------------------------------------------------------------- editor creation

static HICON makeIcon(int size) {
    HDC sdc = GetDC(nullptr);
    HBITMAP color = CreateCompatibleBitmap(sdc, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    int t = std::max(2, size / 8), c = size / 2, gap = size / 8;
    RECT parts[5] = {
        { c - t / 2, 1, c - t / 2 + t, c - gap },             // top
        { c - t / 2, c + gap, c - t / 2 + t, size - 1 },      // bottom
        { 1, c - t / 2, c - gap, c - t / 2 + t },             // left
        { c + gap, c - t / 2, size - 1, c - t / 2 + t },      // right
        { c - t / 2, c - t / 2, c - t / 2 + t, c - t / 2 + t } // dot
    };
    HGDIOBJ old = SelectObject(dc, color);
    RECT all = { 0, 0, size, size };
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    HBRUSH cyan = CreateSolidBrush(RGB(0, 220, 255));
    for (auto& r : parts) FillRect(dc, &r, cyan);
    DeleteObject(cyan);
    SelectObject(dc, mask);
    FillRect(dc, &all, (HBRUSH)GetStockObject(WHITE_BRUSH));
    for (auto& r : parts) FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(dc, old);
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(nullptr, sdc);
    return icon;
}

static void createEditor() {
    // System-DPI aware: crisp on the main monitor, layout scaled once by g_dpi.
    DpiScope scope(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);
    auto font = [](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    g_font = font(9, FW_NORMAL);
    g_fontBold = font(9, FW_SEMIBOLD);
    g_fontTitle = font(14, FW_SEMIBOLD);

    loadPalette();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = editorProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_icon;
    wc.lpszClassName = L"CrosshairEditorWnd";
    RegisterClassExW(&wc);

    auto childClass = [](const wchar_t* name, WNDPROC proc, LPCWSTR cursor) {
        WNDCLASSEXW c{ sizeof(c) };
        c.lpfnWndProc = proc;
        c.hInstance = g_inst;
        c.hCursor = LoadCursor(nullptr, cursor);
        c.lpszClassName = name;
        RegisterClassExW(&c);
    };
    childClass(L"CrosshairPixelGrid", gridProc, IDC_CROSS);
    childClass(L"CrosshairSpin", spinProc, IDC_ARROW);
    childClass(L"CrosshairKeyBox", keyBoxProc, IDC_IBEAM);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, S(1010), S(690 + kHeader) };
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    g_editor = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Crosshair Editor", style,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, g_inst, nullptr);

    // ---- header bar (status is painted; these two buttons sit on the right)
    g_yOff = 0;
    g_toggleBtn = button(L"Hide crosshair", 770, 9, 110, ID_TOGGLE_OVERLAY, 30);
    HWND refreshBtn = button(L"Refresh overlay", 890, 9, 110, ID_REFRESH, 30);
    g_outside = { g_toggleBtn, refreshBtn };
    g_primary = { refreshBtn };
    g_yOff = kHeader;

    // ---- column 1: presets, components, display
    group(L"Preset", 10, 6, 320, 88);
    g_presetCombo = combo(20, 26, 300, ID_PRESET_COMBO);
    label(L"Name", 20, 59, 36);
    g_nameEdit = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 58, 58, 104, 22, ID_PRESET_NAME, WS_EX_CLIENTEDGE);
    g_primary.insert(button(L"Save", 166, 57, 48, ID_PRESET_SAVE));
    button(L"New", 218, 57, 48, ID_PRESET_NEW);
    button(L"Delete", 270, 57, 50, ID_PRESET_DELETE);

    group(L"Components (drawn top to bottom)", 10, 100, 320, 206);
    g_compList = mk(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 20, 120, 190, 176,
                    ID_COMP_LIST, WS_EX_CLIENTEDGE);
    g_compType = combo(216, 120, 104, ID_COMP_TYPE);
    for (int i = 0; i < kTypeCount; ++i) SendMessageW(g_compType, CB_ADDSTRING, 0, (LPARAM)kTypes[i].name);
    SendMessageW(g_compType, CB_SETCURSEL, 0, 0);
    button(L"Add", 216, 148, 104, ID_COMP_ADD);
    button(L"Duplicate", 216, 177, 104, ID_COMP_DUP);
    button(L"Remove", 216, 206, 104, ID_COMP_REMOVE);
    button(L"Move up", 216, 242, 104, ID_COMP_UP);
    button(L"Move down", 216, 271, 104, ID_COMP_DOWN);

    g_compGroupIdx = group(L"Selected component", 10, 312, 320, 200);
    g_cEnabled = check(L"Enabled", 20, 332, 90, ID_COMP_ENABLED);
    g_cSubtract = check(L"Subtract (erase pixels)", 120, 332, 200, ID_COMP_SUBTRACT);
    g_compCtrls = { g_cEnabled, g_cSubtract };
    for (int i = 0; i < 3; ++i) {
        g_pLabel[i] = label(L"", 20, 360 + i * 27, 150);
        g_pEdit[i] = numEdit(180, 360 + i * 27, 80, ID_COMP_P0 + i, 0, kMaxParam);
        g_compCtrls.insert(g_compCtrls.end(), { g_pLabel[i], g_pEdit[i].edit, g_pEdit[i].ud });
    }
    HWND lx = label(L"Offset X", 20, 441, 56);
    g_cX = numEdit(80, 441, 70, ID_COMP_X, -kMaxOffset, kMaxOffset);
    HWND ly = label(L"Y", 164, 441, 14);
    g_cY = numEdit(180, 441, 80, ID_COMP_Y, -kMaxOffset, kMaxOffset);
    g_compCtrls.insert(g_compCtrls.end(), { lx, ly, g_cX.edit, g_cX.ud, g_cY.edit, g_cY.ud });
    for (int i = 0; i < 4; ++i) {
        g_cFlag[i] = check(L"", 20 + (i % 2) * 150, 468 + (i / 2) * 20, 140, ID_COMP_F0 + i);
        g_compCtrls.push_back(g_cFlag[i]);
    }

    group(L"Display", 10, 518, 320, 128);
    label(L"Monitor", 20, 536, 58);
    g_monitorCombo = combo(80, 534, 240, ID_MONITOR);
    label(L"Update rate (fps, 0 = every refresh)", 20, 562, 210);
    g_fps = numEdit(240, 560, 80, ID_FPS, 0, 1000);
    label(L"Theme", 20, 588, 58);
    g_themeCombo = combo(80, 586, 240, ID_THEME);
    for (const wchar_t* t : { L"System (follow Windows)", L"Light", L"Dark" })
        SendMessageW(g_themeCombo, CB_ADDSTRING, 0, (LPARAM)t);
    g_overlayCheck = check(L"Show crosshair", 20, 614, 140, ID_OVERLAY_VISIBLE);
    g_startHidden = check(L"Start hidden in tray", 170, 614, 150, ID_START_HIDDEN);

    // ---- column 2: pixel editor, position
    group(L"Pixels", 340, 6, 380, 542);
    g_grid = mk(L"CrosshairPixelGrid", L"", 0, 350, 26, 360, 360, ID_GRID);
    g_hover = label(L"Hover the grid to see coordinates. Red square = centre pixel.", 350, 390, 360);
    g_mirrorH = check(L"Mirror left / right", 350, 414, 160, ID_MIRROR_H);
    g_mirrorV = check(L"Mirror up / down", 520, 414, 160, ID_MIRROR_V);
    label(L"View radius (0 = auto, mouse wheel zooms)", 350, 442, 250);
    g_viewR = numEdit(610, 440, 100, ID_VIEW_RADIUS, 0, 150);
    button(L"Undo", 350, 470, 80, ID_UNDO);
    button(L"Redo", 434, 470, 80, ID_REDO);
    button(L"Clear pixel edits", 518, 470, 192, ID_CLEAR_PIXELS);
    button(L"Bake components into pixels", 350, 499, 360, ID_BAKE);
    label(L"Left: draw   Right: erase   Shift+click: back to components", 350, 525, 360);

    group(L"Position (from screen centre, hotkeys nudge 1 px)", 340, 554, 380, 86);
    label(L"Offset X", 350, 580, 56);
    g_offX = numEdit(410, 578, 90, ID_OFF_X, -kMaxOffset, kMaxOffset);
    label(L"Y", 512, 580, 14);
    g_offY = numEdit(528, 578, 90, ID_OFF_Y, -kMaxOffset, kMaxOffset);
    button(L"Centre", 628, 577, 82, ID_CENTRE);
    g_legend = label(L"", 350, 608, 360);

    // ---- column 3: effects, hotkeys
    group(L"Effects (applied top to bottom)", 730, 6, 270, 294);
    for (int i = 0; i < FX_COUNT; ++i) {
        int y = 26 + i * 26;
        g_fxCheck[i] = check(kFx[i].label, 740, y, 170, ID_FX_CHECK + i);
        if (kFx[i].valKey) g_fxVal[i] = numEdit(915, y, 75, ID_FX_VAL + i, kFx[i].lo, kFx[i].hi);
    }
    {
        int y = 26 + FX_COUNT * 26;
        const wchar_t* rgbLabel[3] = { L"R", L"G", L"B" };
        for (int i = 0; i < 3; ++i) {
            label(rgbLabel[i], 758 + i * 78, y, 12);
            g_rgb[i] = numEdit(772 + i * 78, y, 60, ID_FX_RGB + i, 0, 255);
        }
    }

    group(L"Hotkeys (click a box, press the keys)", 730, 306, 270, 342);
    for (int i = 0; i < HK_COUNT; ++i) {
        int y = 326 + i * 24;
        label(kHk[i].label, 740, y, 110);
        g_hkCtrl[i] = mk(L"CrosshairKeyBox", L"", WS_TABSTOP, 850, y, 118, 22, ID_HK + i);
        button(L"\u00D7", 970, y, 22, ID_HK_CLEAR + i, 22);
    }
    button(L"Reset to defaults", 740, 326 + HK_COUNT * 24 + 4, 130, ID_HK_DEFAULTS);

    // ---- bottom bar
    g_status = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 12, 657, 768, 20, -1);
    g_outside.insert({ g_status, button(L"Hide to tray", 790, 652, 100, ID_HIDE, 28), button(L"Quit", 900, 652, 100, ID_QUIT, 28) });

    populateAll();
    populateHotkeys();
    fillMonitors();
    setInt(g_fps, g_set.fps);
    setInt(g_viewR, 0);
    setCheck(g_overlayCheck, g_visible);
    setCheck(g_startHidden, g_set.startHidden);
    SendMessageW(g_themeCombo, CB_SETCURSEL, std::clamp(g_set.theme, 0, 2), 0);
    applyTheme();
    SetTimer(g_editor, 2, 500, nullptr);   // header status
    g_ready = true;
}

// ---------------------------------------------------------------- startup

static void enableVisualStyles() {
    // Activation context for Common Controls v6 (themed controls) without a resource file.
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    fs::path p = fs::path(tmp) / L"crosshair_comctl6.manifest";
    {
        std::ofstream f(p, std::ios::trunc);
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
             "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
             "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" "
             "version=\"6.0.0.0\" processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
             "</dependentAssembly></dependency>\n</assembly>\n";
    }
    std::wstring src = p.wstring();
    ACTCTXW ac{};
    ac.cbSize = sizeof(ac);
    ac.lpSource = src.c_str();
    HANDLE h = CreateActCtxW(&ac);
    if (h != INVALID_HANDLE_VALUE) {
        ULONG_PTR cookie;
        ActivateActCtx(h, &cookie);
    }
    typedef BOOL (WINAPI *PFN_InitCC)(const INITCOMMONCONTROLSEX*);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    auto init = cc ? (PFN_InitCC)(void*)GetProcAddress(cc, "InitCommonControlsEx") : nullptr;
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    if (init) init(&icc);
}

static void loadAllPresets() {
    std::error_code ec;
    fs::create_directories(g_presetDir, ec);

    // Import an old single-file crosshair.ini (mode = lines / pattern) as preset "default".
    if (fs::exists(g_iniPath) && fileHasLegacyKeys(g_iniPath)) {
        Preset p;
        if (loadPreset(g_iniPath, p, &g_set)) {
            p.name = L"default";
            if (!fs::exists(presetPath(p.name))) writePreset(presetPath(p.name), p);
            fs::copy_file(g_iniPath, fs::path(g_iniPath).concat(L".bak"), fs::copy_options::overwrite_existing, ec);
            g_set.preset = p.name;
            saveSettings();
        }
    } else {
        loadSettings();
    }

    for (auto& e : fs::directory_iterator(g_presetDir, ec)) {
        if (!e.is_regular_file() || lower(e.path().extension().string()) != ".ini") continue;
        Preset p;
        if (loadPreset(e.path(), p, nullptr)) g_presets.push_back(p);
    }
    if (g_presets.empty()) {
        g_presets.push_back(defaultPreset(L"default"));
        writePreset(presetPath(L"default"), g_presets[0]);
    }
    sortPresets();
    int i = findPreset(g_set.preset);
    g_cur = i >= 0 ? i : 0;
    g_set.preset = cur().name;
    g_selComp = cur().comps.empty() ? -1 : 0;
    if (!fs::exists(g_iniPath)) saveSettings();
}

static bool dirWritable(const fs::path& d) {
    HANDLE h = CreateFileW((d / L"crosshair.write-test").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = inst;
    HANDLE single = CreateMutexW(nullptr, TRUE, L"InvertCrosshairOverlay_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: bring up its editor instead.
        if (HWND w = FindWindowW(L"CrosshairEditorWnd", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }

    // Physical pixels everywhere, so 1 config pixel = 1 screen pixel even with display scaling.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    pSetThreadDpiCtx = (PFN_SetThreadDpiAwarenessContext)(void*)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // Settings and presets next to the exe (portable), or in %APPDATA%\Crosshair when the exe
    // lives somewhere read-only such as Program Files.
    fs::path dataDir = fs::path(exe).parent_path();
    wchar_t appData[MAX_PATH];
    if (!dirWritable(dataDir) && GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
        dataDir = fs::path(appData) / L"Crosshair";
        std::error_code ec;
        fs::create_directories(dataDir, ec);
    }
    g_iniPath = dataDir / L"crosshair.ini";
    g_presetDir = dataDir / L"presets";

    enableVisualStyles();

    loadAllPresets();
    if (!loadMagnification() || !startOverlay()) {
        MessageBoxW(nullptr, L"Could not start the overlay (Windows Magnification API).", L"Crosshair", MB_ICONERROR);
        return 1;
    }

    int smallIcon = GetSystemMetrics(SM_CXSMICON), bigIcon = GetSystemMetrics(SM_CXICON);
    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, smallIcon, smallIcon, 0);   // from crosshair.rc
    g_iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, bigIcon, bigIcon, 0);
    if (!g_icon) g_icon = makeIcon(smallIcon >= 20 ? 32 : 16);
    if (!g_iconBig) g_iconBig = makeIcon(32);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    createEditor();
    addTray();
    rebuildOverlay();
    registerHotkeys();
    bool trayOnly = cmdLine && (wcsstr(cmdLine, L"/tray") || wcsstr(cmdLine, L"--tray"));
    if (!g_set.startHidden && !trayOnly) showEditor();
    setStatus(L"Preset: " + cur().name + L".  Closing this window keeps the crosshair running in the tray.");

    // The editor thread is purely event driven; the overlay runs its own frame loop.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && !msg.hwnd) onHotkey((int)msg.wParam - 1);
        else if (!handleEditorKeys(msg) && !(g_editor && IsDialogMessageW(g_editor, &msg))) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        syncHotkeyRegistration();
    }
    stopOverlay();
    unregisterHotkeys();
    if (single) { ReleaseMutex(single); CloseHandle(single); }
    return 0;
}
