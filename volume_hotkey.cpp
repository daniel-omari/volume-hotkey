// ============================================================================
// Volume Hotkey v2.1 - Per-App Volume Toggle for Windows
// ============================================================================
//
// Features:
//   - System tray icon (no console window) with right-click menu
//   - Config file (volume_hotkey.ini) for persistent settings
//   - Native Win32 config dialog with hotkey capture control
//   - Sound feedback (distinct beep tones) on toggle
//   - Global hotkey to duck/restore any app's volume
//
// Architecture:
//   - First run (no config): opens config dialog automatically
//   - Subsequent runs: reads config, goes straight to background mode
//   - Right-click tray icon -> "Configure" to change settings anytime
//   - Right-click tray icon -> "Quit" to exit
//
// BUILD:
//   MinGW:  g++ -o volume_hotkey.exe volume_hotkey.cpp -lole32 -loleaut32
//             -luser32 -lshell32 -lcomctl32 -lgdi32 -mwindows
//   MSVC:   cl /EHsc volume_hotkey.cpp /link /SUBSYSTEM:WINDOWS
//             ole32.lib oleaut32.lib user32.lib shell32.lib comctl32.lib
//             gdi32.lib
//
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>       // Shell_NotifyIcon (system tray)
#include <commctrl.h>       // Trackbar, Hotkey control
#include <mmdeviceapi.h>    // Audio device enumeration
#include <audiopolicy.h>    // Audio session control
#include <endpointvolume.h> // Volume endpoints
#include <comdef.h>         // COM helpers
#include <psapi.h>          // Process info

#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <map>

// Auto-link required libraries (works with both MSVC and MinGW)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")

// ============================================================================
// CONSTANTS AND IDS
// ============================================================================

// Tray icon
static const UINT WM_TRAYICON     = WM_APP + 1;
static const UINT TRAY_ID         = 1;

// Tray menu commands
static const UINT IDM_CONFIGURE   = 1001;
static const UINT IDM_QUIT        = 1002;
static const UINT IDM_STATUS      = 1003;

// Hotkey
static const int  HOTKEY_ID       = 1;

// Config dialog control IDs
static const int IDC_COMBO_PROCESS  = 2001;
static const int IDC_BTN_REFRESH    = 2002;
static const int IDC_HOTKEY_CTRL    = 2003;   // Hotkey capture control
static const int IDC_BTN_RESET_HK  = 2004;   // Reset hotkey button
static const int IDC_SLIDER_VOLUME  = 2005;
static const int IDC_LABEL_PERCENT  = 2006;
static const int IDC_CHECK_SOUND    = 2007;
static const int IDC_BTN_SAVE       = 2008;
static const int IDC_BTN_CANCEL     = 2009;


// ============================================================================
// CONFIGURATION DATA
// ============================================================================

struct Config {
    std::string targetProcess;   // e.g. "chrome.exe"
    UINT        hotkeyModifier;  // e.g. MOD_CONTROL | MOD_SHIFT
    UINT        hotkeyKey;       // e.g. 'M'
    int         duckPercent;     // e.g. 20
    bool        soundEnabled;    // e.g. true
};

// Default config for first run
Config g_config = { "", MOD_CONTROL | MOD_SHIFT, 'M', 20, true };

// Runtime state
struct ToggleState {
    DWORD       targetProcessId;
    std::string targetName;
    float       originalVolume;
    bool        isDucked;
    bool        isActive;        // true if target process was found
};

ToggleState g_state = { 0, "", 1.0f, false, false };

// Global handles
HWND            g_hwndHidden   = nullptr;   // Hidden window for messages
HINSTANCE       g_hInstance    = nullptr;
NOTIFYICONDATAA g_trayIcon     = {};


// ============================================================================
// AUDIO SESSION DATA
// ============================================================================

struct AudioSession {
    DWORD       processId;
    std::string processName;
    float       currentVolume;
};


// ============================================================================
// HELPER: Get process name from PID
// ============================================================================
std::string getProcessName(DWORD pid) {
    if (pid == 0) return "[System Sounds]";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return "[Unknown]";

    char path[MAX_PATH];
    DWORD pathSize = MAX_PATH;
    std::string name = "[Unknown]";

    if (QueryFullProcessImageNameA(hProcess, 0, path, &pathSize)) {
        std::string fullPath(path);
        size_t lastSlash = fullPath.find_last_of("\\/");
        name = (lastSlash != std::string::npos)
             ? fullPath.substr(lastSlash + 1)
             : fullPath;
    }

    CloseHandle(hProcess);
    return name;
}


// ============================================================================
// AUDIO API: Enumerate audio sessions
// ============================================================================
std::vector<AudioSession> enumerateAudioSessions() {
    std::vector<AudioSession> sessions;

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return sessions;

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) { pEnumerator->Release(); return sessions; }

    IAudioSessionManager2* pSessionManager = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                           nullptr, (void**)&pSessionManager);
    if (FAILED(hr)) {
        pDevice->Release(); pEnumerator->Release(); return sessions;
    }

    IAudioSessionEnumerator* pSessionEnum = nullptr;
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr)) {
        pSessionManager->Release(); pDevice->Release();
        pEnumerator->Release(); return sessions;
    }

    int count = 0;
    pSessionEnum->GetCount(&count);

    for (int i = 0; i < count; i++) {
        IAudioSessionControl* pCtrl = nullptr;
        if (FAILED(pSessionEnum->GetSession(i, &pCtrl))) continue;

        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                         (void**)&pCtrl2))) {
            pCtrl->Release(); continue;
        }

        ISimpleAudioVolume* pVol = nullptr;
        if (FAILED(pCtrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                         (void**)&pVol))) {
            pCtrl2->Release(); pCtrl->Release(); continue;
        }

        DWORD pid = 0;
        pCtrl2->GetProcessId(&pid);

        float vol = 1.0f;
        pVol->GetMasterVolume(&vol);

        AudioSession s;
        s.processId     = pid;
        s.processName   = getProcessName(pid);
        s.currentVolume = vol;
        sessions.push_back(s);

        pVol->Release(); pCtrl2->Release(); pCtrl->Release();
    }

    pSessionEnum->Release(); pSessionManager->Release();
    pDevice->Release(); pEnumerator->Release();
    return sessions;
}


// ============================================================================
// AUDIO API: Set volume for a process by PID
// ============================================================================
bool setProcessVolume(DWORD targetPid, float volume) {
    volume = std::max(0.0f, std::min(1.0f, volume));

    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
               CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
        return false;

    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev)))
        { pEnum->Release(); return false; }

    IAudioSessionManager2* pMgr = nullptr;
    if (FAILED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                              nullptr, (void**)&pMgr)))
        { pDev->Release(); pEnum->Release(); return false; }

    IAudioSessionEnumerator* pSessions = nullptr;
    if (FAILED(pMgr->GetSessionEnumerator(&pSessions)))
        { pMgr->Release(); pDev->Release(); pEnum->Release(); return false; }

    int count = 0;
    pSessions->GetCount(&count);
    bool success = false;

    for (int i = 0; i < count; i++) {
        IAudioSessionControl* pCtrl = nullptr;
        if (FAILED(pSessions->GetSession(i, &pCtrl))) continue;

        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                         (void**)&pCtrl2))) {
            pCtrl->Release(); continue;
        }

        DWORD pid = 0;
        pCtrl2->GetProcessId(&pid);

        if (pid == targetPid) {
            ISimpleAudioVolume* pVol = nullptr;
            if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                (void**)&pVol))) {
                success = SUCCEEDED(pVol->SetMasterVolume(volume, nullptr));
                pVol->Release();
            }
            pCtrl2->Release(); pCtrl->Release();
            break;
        }
        pCtrl2->Release(); pCtrl->Release();
    }

    pSessions->Release(); pMgr->Release();
    pDev->Release(); pEnum->Release();
    return success;
}


// ============================================================================
// AUDIO API: Get current volume for a process by PID
// ============================================================================
float getProcessVolume(DWORD targetPid) {
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
               CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)))
        return -1.0f;

    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev)))
        { pEnum->Release(); return -1.0f; }

    IAudioSessionManager2* pMgr = nullptr;
    if (FAILED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                              nullptr, (void**)&pMgr)))
        { pDev->Release(); pEnum->Release(); return -1.0f; }

    IAudioSessionEnumerator* pSessions = nullptr;
    if (FAILED(pMgr->GetSessionEnumerator(&pSessions)))
        { pMgr->Release(); pDev->Release(); pEnum->Release(); return -1.0f; }

    int count = 0;
    pSessions->GetCount(&count);
    float result = -1.0f;

    for (int i = 0; i < count; i++) {
        IAudioSessionControl* pCtrl = nullptr;
        if (FAILED(pSessions->GetSession(i, &pCtrl))) continue;

        IAudioSessionControl2* pCtrl2 = nullptr;
        if (FAILED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                         (void**)&pCtrl2))) {
            pCtrl->Release(); continue;
        }

        DWORD pid = 0;
        pCtrl2->GetProcessId(&pid);

        if (pid == targetPid) {
            ISimpleAudioVolume* pVol = nullptr;
            if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                (void**)&pVol))) {
                pVol->GetMasterVolume(&result);
                pVol->Release();
            }
            pCtrl2->Release(); pCtrl->Release();
            break;
        }
        pCtrl2->Release(); pCtrl->Release();
    }

    pSessions->Release(); pMgr->Release();
    pDev->Release(); pEnum->Release();
    return result;
}


// ============================================================================
// AUDIO API: Find PID of a process by name among active audio sessions
// Returns 0 if not found.
// ============================================================================
DWORD findAudioProcessByName(const std::string& name) {
    auto sessions = enumerateAudioSessions();
    for (const auto& s : sessions) {
        // Case-insensitive comparison
        std::string a = s.processName;
        std::string b = name;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (a == b) return s.processId;
    }
    return 0;
}


// ============================================================================
// CONFIG FILE (INI FORMAT)
// ============================================================================
//
//   [target]
//   process=chrome.exe
//
//   [hotkey]
//   modifier=ctrl+shift
//   key=M
//
//   [volume]
//   duck_level=20
//
//   [feedback]
//   sound_enabled=true
//
// ============================================================================

// --- Get the path to config.ini next to our .exe ---
std::string getConfigPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        path = path.substr(0, lastSlash + 1);
    return path + "volume_hotkey.ini";
}

// --- Modifier string <-> UINT conversion ---
struct ModifierEntry {
    const char* name;
    UINT        flags;
};

static const ModifierEntry MODIFIER_TABLE[] = {
    { "ctrl",           MOD_CONTROL },
    { "alt",            MOD_ALT },
    { "shift",          MOD_SHIFT },
    { "ctrl+alt",       MOD_CONTROL | MOD_ALT },
    { "ctrl+shift",     MOD_CONTROL | MOD_SHIFT },
    { "alt+shift",      MOD_ALT     | MOD_SHIFT },
    { "ctrl+alt+shift", MOD_CONTROL | MOD_ALT | MOD_SHIFT },
};
static const int MODIFIER_COUNT = sizeof(MODIFIER_TABLE) / sizeof(MODIFIER_TABLE[0]);

std::string modifierToString(UINT mod) {
    for (int i = 0; i < MODIFIER_COUNT; i++) {
        if (MODIFIER_TABLE[i].flags == mod) return MODIFIER_TABLE[i].name;
    }
    return "ctrl+shift";
}

UINT stringToModifier(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (int i = 0; i < MODIFIER_COUNT; i++) {
        if (lower == MODIFIER_TABLE[i].name) return MODIFIER_TABLE[i].flags;
    }
    return MOD_CONTROL | MOD_SHIFT;
}

// --- Key string <-> VK code conversion ---
std::string keyToString(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F12)
        return "F" + std::to_string(vk - VK_F1 + 1);
    return "M";
}

UINT stringToKey(const std::string& str) {
    if (str.empty()) return 'M';
    if (str[0] == 'F' || str[0] == 'f') {
        try {
            int n = std::stoi(str.substr(1));
            if (n >= 1 && n <= 12) return VK_F1 + (n - 1);
        } catch (...) {}
    }
    if (str.length() == 1) {
        char c = (char)toupper(str[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    return 'M';
}

// --- Human-readable hotkey string like "Ctrl+Shift+M" ---
std::string hotkeyDisplayString(UINT mod, UINT key) {
    std::string result;
    if (mod & MOD_CONTROL) result += "Ctrl+";
    if (mod & MOD_ALT)     result += "Alt+";
    if (mod & MOD_SHIFT)   result += "Shift+";
    result += keyToString(key);
    return result;
}

// --- Conversion between HOTKEYF_* flags (hotkey control) and MOD_* flags ---
// The Windows hotkey capture control (msctls_hotkey32) uses HOTKEYF_*
// while RegisterHotKey uses MOD_*. They have different bit values!
//   HOTKEYF_SHIFT   = 0x01    MOD_ALT     = 0x0001
//   HOTKEYF_CONTROL = 0x02    MOD_CONTROL = 0x0002
//   HOTKEYF_ALT     = 0x04    MOD_SHIFT   = 0x0004

BYTE modToHotkeyF(UINT mod) {
    BYTE hk = 0;
    if (mod & MOD_CONTROL) hk |= HOTKEYF_CONTROL;
    if (mod & MOD_ALT)     hk |= HOTKEYF_ALT;
    if (mod & MOD_SHIFT)   hk |= HOTKEYF_SHIFT;
    return hk;
}

UINT hotkeyFToMod(BYTE hk) {
    UINT mod = 0;
    if (hk & HOTKEYF_CONTROL) mod |= MOD_CONTROL;
    if (hk & HOTKEYF_ALT)     mod |= MOD_ALT;
    if (hk & HOTKEYF_SHIFT)   mod |= MOD_SHIFT;
    return mod;
}

// --- Simple INI helpers ---
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

// --- Read config from INI file ---
bool readConfig(const std::string& path, Config& cfg) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line, section;
    std::map<std::string, std::map<std::string, std::string>> ini;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                section = line.substr(1, end - 1);
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));
            ini[section][key] = val;
        }
    }

    cfg.targetProcess  = ini["target"]["process"];
    cfg.hotkeyModifier = stringToModifier(
        ini.count("hotkey") && ini["hotkey"].count("modifier")
        ? ini["hotkey"]["modifier"] : "ctrl+shift");
    cfg.hotkeyKey      = stringToKey(
        ini.count("hotkey") && ini["hotkey"].count("key")
        ? ini["hotkey"]["key"] : "M");
    cfg.duckPercent    = 20;
    if (ini.count("volume") && ini["volume"].count("duck_level")) {
        try { cfg.duckPercent = std::stoi(ini["volume"]["duck_level"]); }
        catch (...) {}
    }
    cfg.duckPercent = std::max(0, std::min(100, cfg.duckPercent));

    cfg.soundEnabled = true;
    if (ini.count("feedback") && ini["feedback"].count("sound_enabled")) {
        std::string val = ini["feedback"]["sound_enabled"];
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        cfg.soundEnabled = (val == "true" || val == "1" || val == "yes");
    }

    return !cfg.targetProcess.empty();
}

// --- Write config to INI file ---
bool writeConfig(const std::string& path, const Config& cfg) {
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "; Volume Hotkey configuration\n";
    file << "; Edit this file or use the Configure dialog\n\n";
    file << "[target]\n";
    file << "process=" << cfg.targetProcess << "\n\n";
    file << "[hotkey]\n";
    file << "modifier=" << modifierToString(cfg.hotkeyModifier) << "\n";
    file << "key=" << keyToString(cfg.hotkeyKey) << "\n\n";
    file << "[volume]\n";
    file << "duck_level=" << cfg.duckPercent << "\n\n";
    file << "[feedback]\n";
    file << "sound_enabled=" << (cfg.soundEnabled ? "true" : "false") << "\n";

    return true;
}


// ============================================================================
// SOUND FEEDBACK
// ============================================================================
// Uses Beep() in a background thread for non-blocking audio feedback.
// Two distinct tones so you can tell duck from restore by ear.
// ============================================================================

void playDuckSound() {
    if (!g_config.soundEnabled) return;
    // Quick descending tone: "going down"
    std::thread([] {
        Beep(700, 80);
        Beep(450, 120);
    }).detach();
}

void playRestoreSound() {
    if (!g_config.soundEnabled) return;
    // Quick ascending tone: "coming back up"
    std::thread([] {
        Beep(450, 80);
        Beep(700, 120);
    }).detach();
}

void playErrorSound() {
    if (!g_config.soundEnabled) return;
    std::thread([] { Beep(200, 300); }).detach();
}


// ============================================================================
// TOGGLE LOGIC
// ============================================================================

void toggleVolume() {
    // If we don't have an active target, try to find it
    if (!g_state.isActive) {
        DWORD pid = findAudioProcessByName(g_config.targetProcess);
        if (pid == 0) {
            playErrorSound();
            return;
        }
        g_state.targetProcessId = pid;
        g_state.targetName      = g_config.targetProcess;
        g_state.isActive        = true;
    }

    float duckLevel = g_config.duckPercent / 100.0f;

    if (!g_state.isDucked) {
        // Save current volume, then duck
        float current = getProcessVolume(g_state.targetProcessId);
        if (current < 0.0f) {
            // Process may have closed - reset and try again next press
            g_state.isActive = false;
            playErrorSound();
            return;
        }
        g_state.originalVolume = current;
        setProcessVolume(g_state.targetProcessId, duckLevel);
        g_state.isDucked = true;
        playDuckSound();

        // Update tray tooltip to show ducked state
        std::string tip = "Volume Hotkey - " + g_state.targetName
                        + " [DUCKED " + std::to_string(g_config.duckPercent) + "%]";
        strncpy(g_trayIcon.szTip, tip.c_str(), sizeof(g_trayIcon.szTip) - 1);
        Shell_NotifyIconA(NIM_MODIFY, &g_trayIcon);

    } else {
        // Restore original volume
        setProcessVolume(g_state.targetProcessId, g_state.originalVolume);
        g_state.isDucked = false;
        playRestoreSound();

        // Update tray tooltip
        std::string tip = "Volume Hotkey - " + g_state.targetName + " [Normal]";
        strncpy(g_trayIcon.szTip, tip.c_str(), sizeof(g_trayIcon.szTip) - 1);
        Shell_NotifyIconA(NIM_MODIFY, &g_trayIcon);
    }
}


// ============================================================================
// CONFIG DIALOG (Native Win32 UI)
// ============================================================================
// Uses the built-in Windows HOTKEY_CLASS control ("msctls_hotkey32") which
// lets the user click the field and press their desired key combination.
// The control automatically displays modifiers + key as the user types.
//
// No mouse buttons are capturable via HOTKEY_CLASS (keyboard only),
// which prevents accidental assignment of left/right click.
// ============================================================================

// Handles to dialog controls
static HWND g_dlgProcess  = nullptr;
static HWND g_dlgHotkey   = nullptr;  // The hotkey capture control
static HWND g_dlgSlider   = nullptr;
static HWND g_dlgPercent  = nullptr;
static HWND g_dlgSound    = nullptr;

// Helper: create a static label
static HWND createLabel(HWND parent, const char* text,
                        int x, int y, int w, int h) {
    return CreateWindowExA(0, "STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr, g_hInstance, nullptr);
}

// Populate the process combobox with active audio sessions
static void populateProcessCombo(HWND combo) {
    // Save what the user currently has typed/selected
    char currentText[256] = {};
    GetWindowTextA(combo, currentText, sizeof(currentText));

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    auto sessions = enumerateAudioSessions();
    for (const auto& s : sessions) {
        if (s.processId == 0) continue;
        // Avoid duplicate process names in the list
        if (SendMessageA(combo, CB_FINDSTRINGEXACT, -1,
                         (LPARAM)s.processName.c_str()) == CB_ERR) {
            SendMessageA(combo, CB_ADDSTRING, 0,
                         (LPARAM)s.processName.c_str());
        }
    }

    // Restore previous selection/text
    if (currentText[0] != '\0') {
        int idx = (int)SendMessageA(combo, CB_FINDSTRINGEXACT, -1,
                                    (LPARAM)currentText);
        if (idx != CB_ERR) {
            SendMessageA(combo, CB_SETCURSEL, idx, 0);
        } else {
            SetWindowTextA(combo, currentText);
        }
    } else if (!g_config.targetProcess.empty()) {
        int idx = (int)SendMessageA(combo, CB_FINDSTRINGEXACT, -1,
                                    (LPARAM)g_config.targetProcess.c_str());
        if (idx != CB_ERR) {
            SendMessageA(combo, CB_SETCURSEL, idx, 0);
        } else {
            SetWindowTextA(combo, g_config.targetProcess.c_str());
        }
    }
}

// Update the percentage label next to the slider
static void updatePercentLabel(HWND label, int value) {
    std::string text = std::to_string(value) + "%";
    SetWindowTextA(label, text.c_str());
}

// Read all values from the dialog into a Config struct
static Config readDialogValues() {
    Config cfg;

    // Process name
    char buf[256] = {};
    GetWindowTextA(g_dlgProcess, buf, sizeof(buf));
    cfg.targetProcess = buf;

    // Hotkey - read from the hotkey capture control
    LRESULT hkResult = SendMessageA(g_dlgHotkey, HKM_GETHOTKEY, 0, 0);
    BYTE vk  = LOBYTE(LOWORD(hkResult));   // Virtual key code
    BYTE mod = HIBYTE(LOWORD(hkResult));    // HOTKEYF_* modifiers

    if (vk == 0) {
        // User cleared the control - fall back to default
        cfg.hotkeyKey      = 'M';
        cfg.hotkeyModifier = MOD_CONTROL | MOD_SHIFT;
    } else {
        cfg.hotkeyKey      = (UINT)vk;
        cfg.hotkeyModifier = hotkeyFToMod(mod);
    }

    // Duck volume
    cfg.duckPercent = (int)SendMessageA(g_dlgSlider, TBM_GETPOS, 0, 0);
    cfg.duckPercent = std::max(0, std::min(100, cfg.duckPercent));

    // Sound
    cfg.soundEnabled = (SendMessageA(g_dlgSound, BM_GETCHECK, 0, 0)
                        == BST_CHECKED);

    return cfg;
}

// --- Config dialog window procedure ---
static LRESULT CALLBACK configWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        int y  = 20;     // Current vertical position
        int lx = 20;     // Label X
        int cx = 170;    // Control X
        int lw = 140;    // Label width
        int cw = 220;    // Control width
        int rh = 28;     // Row height
        int gap = 38;    // Gap between rows

        // --- Title ---
        HWND title = createLabel(hwnd, "Volume Hotkey - Configuration",
                                 lx, y, 380, 24);
        HFONT hBold = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
        SendMessageA(title, WM_SETFONT, (WPARAM)hBold, TRUE);
        y += 40;

        // --- Target Process ---
        HWND lbl1 = createLabel(hwnd, "Target Application:", lx, y + 4, lw, 20);
        SendMessageA(lbl1, WM_SETFONT, (WPARAM)hFont, TRUE);

        // CBS_DROPDOWN = user can type a custom name OR pick from list
        g_dlgProcess = CreateWindowExA(0, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
            cx, y, cw - 80, 200, hwnd, (HMENU)(INT_PTR)IDC_COMBO_PROCESS,
            g_hInstance, nullptr);
        SendMessageA(g_dlgProcess, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND btnRefresh = CreateWindowExA(0, "BUTTON", "Refresh",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + cw - 70, y, 70, rh, hwnd, (HMENU)(INT_PTR)IDC_BTN_REFRESH,
            g_hInstance, nullptr);
        SendMessageA(btnRefresh, WM_SETFONT, (WPARAM)hFont, TRUE);

        populateProcessCombo(g_dlgProcess);
        y += gap + 4;

        // --- Hotkey Capture ---
        HWND lbl2 = createLabel(hwnd, "Hotkey:", lx, y + 4, lw, 20);
        SendMessageA(lbl2, WM_SETFONT, (WPARAM)hFont, TRUE);

        // The HOTKEY_CLASS control ("msctls_hotkey32") is a built-in Windows
        // control. Click it, then press your desired key combination - it
        // captures and displays it automatically. Keyboard only (no mouse).
        g_dlgHotkey = CreateWindowExA(
            WS_EX_CLIENTEDGE, HOTKEY_CLASSA, "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            cx, y, cw - 80, rh, hwnd, (HMENU)(INT_PTR)IDC_HOTKEY_CTRL,
            g_hInstance, nullptr);
        SendMessageA(g_dlgHotkey, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Set current hotkey into the control
        BYTE hkMod = modToHotkeyF(g_config.hotkeyModifier);
        SendMessageA(g_dlgHotkey, HKM_SETHOTKEY,
                     MAKEWORD(g_config.hotkeyKey, hkMod), 0);

        // Rule: if user presses a bare key with no modifier, auto-add
        // Ctrl+Shift (prevents accidentally setting 'A' as a hotkey)
        SendMessageA(g_dlgHotkey, HKM_SETRULES,
                     HKCOMB_NONE | HKCOMB_S,  // bare key and shift-only invalid
                     MAKELPARAM(HOTKEYF_CONTROL | HOTKEYF_SHIFT, 0));

        HWND btnReset = CreateWindowExA(0, "BUTTON", "Reset",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + cw - 70, y, 70, rh, hwnd, (HMENU)(INT_PTR)IDC_BTN_RESET_HK,
            g_hInstance, nullptr);
        SendMessageA(btnReset, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += gap;

        // --- Hotkey hint label ---
        HWND lblHint = createLabel(hwnd,
            "(Click the field above, then press your desired combo)",
            cx, y, cw + 10, 16);
        SendMessageA(lblHint, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 28;

        // --- Duck Volume Slider ---
        HWND lbl4 = createLabel(hwnd, "Duck Volume:", lx, y + 4, lw, 20);
        SendMessageA(lbl4, WM_SETFONT, (WPARAM)hFont, TRUE);

        g_dlgSlider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            cx, y, cw - 50, 30, hwnd, (HMENU)(INT_PTR)IDC_SLIDER_VOLUME,
            g_hInstance, nullptr);
        SendMessageA(g_dlgSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageA(g_dlgSlider, TBM_SETTICFREQ, 10, 0);
        SendMessageA(g_dlgSlider, TBM_SETPOS, TRUE, g_config.duckPercent);

        g_dlgPercent = createLabel(hwnd,
            (std::to_string(g_config.duckPercent) + "%").c_str(),
            cx + cw - 40, y + 4, 50, 20);
        SendMessageA(g_dlgPercent, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += gap + 4;

        // --- Sound Enabled Checkbox ---
        g_dlgSound = CreateWindowExA(0, "BUTTON",
            "Play sound feedback on toggle",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            lx, y, 350, 24, hwnd, (HMENU)(INT_PTR)IDC_CHECK_SOUND,
            g_hInstance, nullptr);
        SendMessageA(g_dlgSound, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(g_dlgSound, BM_SETCHECK,
                     g_config.soundEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        y += gap + 10;

        // --- Buttons ---
        HWND btnSave = CreateWindowExA(0, "BUTTON", "Save && Start",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            cx, y, 105, 34, hwnd, (HMENU)(INT_PTR)IDC_BTN_SAVE,
            g_hInstance, nullptr);
        SendMessageA(btnSave, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND btnCancel = CreateWindowExA(0, "BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            cx + 115, y, 105, 34, hwnd, (HMENU)(INT_PTR)IDC_BTN_CANCEL,
            g_hInstance, nullptr);
        SendMessageA(btnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        return 0;
    }

    case WM_HSCROLL: {
        if ((HWND)lParam == g_dlgSlider) {
            int pos = (int)SendMessageA(g_dlgSlider, TBM_GETPOS, 0, 0);
            updatePercentLabel(g_dlgPercent, pos);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == IDC_BTN_REFRESH) {
            populateProcessCombo(g_dlgProcess);
            return 0;
        }

        if (id == IDC_BTN_RESET_HK) {
            // Reset hotkey control to default: Ctrl+Shift+M
            BYTE hkMod = HOTKEYF_CONTROL | HOTKEYF_SHIFT;
            SendMessageA(g_dlgHotkey, HKM_SETHOTKEY,
                         MAKEWORD('M', hkMod), 0);
            return 0;
        }

        if (id == IDC_BTN_SAVE) {
            Config newCfg = readDialogValues();

            if (newCfg.targetProcess.empty()) {
                MessageBoxA(hwnd,
                    "Please select or type a target application name.",
                    "Volume Hotkey", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // Validate hotkey has a modifier
            if (newCfg.hotkeyModifier == 0) {
                MessageBoxA(hwnd,
                    "Please set a hotkey with at least one modifier key\n"
                    "(Ctrl, Alt, or Shift + a key).",
                    "Volume Hotkey", MB_OK | MB_ICONWARNING);
                return 0;
            }

            // Save to file
            std::string path = getConfigPath();
            if (!writeConfig(path, newCfg)) {
                MessageBoxA(hwnd,
                    "Failed to save config file! Check write permissions.",
                    "Volume Hotkey", MB_OK | MB_ICONERROR);
                return 0;
            }

            // Apply to globals
            g_config = newCfg;

            DestroyWindow(hwnd);
            return 0;
        }

        if (id == IDC_BTN_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

// Register the config dialog window class
static bool registerConfigClass() {
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = configWndProc;
    wc.hInstance      = g_hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "VolumeHotkeyConfig";
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    return RegisterClassExA(&wc) != 0;
}

// Show the config dialog. Returns true if user saved, false if cancelled.
bool showConfigDialog() {
    static bool classRegistered = false;
    if (!classRegistered) {
        registerConfigClass();
        classRegistered = true;
    }

    int dlgW = 430;
    int dlgH = 400;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - dlgW) / 2;
    int y = (screenH - dlgH) / 2;

    Config beforeCfg = g_config;

    HWND hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "VolumeHotkeyConfig", "Volume Hotkey - Configuration",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        nullptr, nullptr, g_hInstance, nullptr);

    if (!hwnd) return false;

    // Run a local message loop for this dialog
    MSG msg;
    while (IsWindow(hwnd) && GetMessage(&msg, nullptr, 0, 0)) {
        // Still handle hotkeys while dialog is open
        if (msg.message == WM_HOTKEY && msg.wParam == (WPARAM)HOTKEY_ID) {
            toggleVolume();
            continue;
        }
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (g_config.targetProcess != beforeCfg.targetProcess ||
            g_config.hotkeyModifier != beforeCfg.hotkeyModifier ||
            g_config.hotkeyKey != beforeCfg.hotkeyKey ||
            g_config.duckPercent != beforeCfg.duckPercent ||
            g_config.soundEnabled != beforeCfg.soundEnabled);
}


// ============================================================================
// SYSTEM TRAY ICON
// ============================================================================
// Creates a hidden window + tray icon. Right-click shows a context menu.
// The hidden window receives WM_HOTKEY messages because we register the
// hotkey with g_hwndHidden (NOT nullptr).
// ============================================================================

// Show the tray context menu
static void showTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();

    // Status line (grayed out, just informational)
    std::string status;
    if (g_state.isActive) {
        status = g_state.targetName;
        if (g_state.isDucked)
            status += " [DUCKED " + std::to_string(g_config.duckPercent) + "%]";
        else
            status += " [Normal]";
    } else {
        status = "Waiting for " + g_config.targetProcess + "...";
    }
    AppendMenuA(menu, MF_STRING | MF_GRAYED, IDM_STATUS, status.c_str());
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);

    std::string hotkeyInfo = "Hotkey: " +
        hotkeyDisplayString(g_config.hotkeyModifier, g_config.hotkeyKey);
    AppendMenuA(menu, MF_STRING | MF_GRAYED, 0, hotkeyInfo.c_str());
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuA(menu, MF_STRING, IDM_CONFIGURE, "Configure...");
    AppendMenuA(menu, MF_STRING, IDM_QUIT,      "Quit");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Hidden window procedure
static LRESULT CALLBACK hiddenWndProc(HWND hwnd, UINT msg,
                                       WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            showTrayMenu(hwnd);
        }
        return 0;

    case WM_HOTKEY:
        if ((int)wParam == HOTKEY_ID) {
            toggleVolume();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case IDM_CONFIGURE: {
            // Unregister hotkey while configuring (will re-register after)
            UnregisterHotKey(g_hwndHidden, HOTKEY_ID);

            bool changed = showConfigDialog();

            if (changed) {
                // Reset state since target may have changed
                g_state.isActive = false;
                g_state.isDucked = false;

                std::string tip = "Volume Hotkey - " + g_config.targetProcess;
                strncpy(g_trayIcon.szTip, tip.c_str(),
                        sizeof(g_trayIcon.szTip) - 1);
                Shell_NotifyIconA(NIM_MODIFY, &g_trayIcon);
            }

            // Re-register hotkey (possibly with new key combo)
            if (!RegisterHotKey(g_hwndHidden, HOTKEY_ID,
                                g_config.hotkeyModifier, g_config.hotkeyKey)) {
                std::string errmsg = "Failed to register hotkey "
                    + hotkeyDisplayString(g_config.hotkeyModifier,
                                          g_config.hotkeyKey)
                    + "!\nAnother app may be using it.";
                MessageBoxA(nullptr, errmsg.c_str(), "Volume Hotkey",
                            MB_OK | MB_ICONWARNING);
            }
            return 0;
        }

        case IDM_QUIT:
            // Restore volume before quitting if ducked
            if (g_state.isDucked && g_state.isActive) {
                setProcessVolume(g_state.targetProcessId,
                                 g_state.originalVolume);
            }
            PostQuitMessage(0);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconA(NIM_DELETE, &g_trayIcon);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

// Set up the hidden window and tray icon
bool setupTray() {
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = hiddenWndProc;
    wc.hInstance      = g_hInstance;
    wc.lpszClassName = "VolumeHotkeyHidden";
    if (!RegisterClassExA(&wc)) return false;

    // Message-only window: invisible, only receives messages
    g_hwndHidden = CreateWindowExA(
        0, "VolumeHotkeyHidden", "Volume Hotkey",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, g_hInstance, nullptr);
    if (!g_hwndHidden) return false;

    ZeroMemory(&g_trayIcon, sizeof(g_trayIcon));
    g_trayIcon.cbSize           = sizeof(g_trayIcon);
    g_trayIcon.hWnd             = g_hwndHidden;
    g_trayIcon.uID              = TRAY_ID;
    g_trayIcon.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);

    std::string tip = "Volume Hotkey - " + g_config.targetProcess;
    strncpy(g_trayIcon.szTip, tip.c_str(), sizeof(g_trayIcon.szTip) - 1);

    Shell_NotifyIconA(NIM_ADD, &g_trayIcon);
    return true;
}


// ============================================================================
// ENTRY POINT
// ============================================================================
// Uses WinMain (not main) so we compile without a console window.
//
// Flow:
//   1. Initialize COM and common controls
//   2. Try to read config file
//   3. If no config -> show config dialog (first-run experience)
//   4. Set up tray icon
//   5. Register hotkey with g_hwndHidden (FIX: not nullptr!)
//   6. Enter message loop
//   7. Clean up on exit
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_hInstance = hInstance;

    // Prevent multiple instances
    HANDLE hMutex = CreateMutexA(nullptr, TRUE, "VolumeHotkeyMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(nullptr,
            "Volume Hotkey is already running!\n"
            "Check your system tray (bottom-right of taskbar).",
            "Volume Hotkey", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Initialize COM (apartment-threaded for UI + Shell_NotifyIcon)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(nullptr, "Failed to initialize COM.", "Error", MB_OK);
        return 1;
    }

    // Initialize common controls (needed for trackbar + hotkey control)
    INITCOMMONCONTROLSEX icc = {
        sizeof(icc), ICC_BAR_CLASSES | ICC_HOTKEY_CLASS
    };
    InitCommonControlsEx(&icc);

    // Try to read existing config
    std::string configPath = getConfigPath();
    bool hasConfig = readConfig(configPath, g_config);

    // First run: show config dialog
    if (!hasConfig) {
        g_config.hotkeyModifier = MOD_CONTROL | MOD_SHIFT;
        g_config.hotkeyKey      = 'M';
        g_config.duckPercent    = 20;
        g_config.soundEnabled   = true;

        showConfigDialog();

        if (g_config.targetProcess.empty()) {
            CoUninitialize();
            return 0;
        }
    }

    // Set up system tray (creates g_hwndHidden)
    if (!setupTray()) {
        MessageBoxA(nullptr, "Failed to create system tray icon.",
                    "Error", MB_OK);
        CoUninitialize();
        return 1;
    }

    // Register global hotkey with g_hwndHidden so WM_HOTKEY is delivered
    // to hiddenWndProc (NOT nullptr which would lose messages!)
    if (!RegisterHotKey(g_hwndHidden, HOTKEY_ID,
                        g_config.hotkeyModifier, g_config.hotkeyKey)) {
        std::string msg = "Failed to register hotkey "
            + hotkeyDisplayString(g_config.hotkeyModifier, g_config.hotkeyKey)
            + "!\n\nAnother application may be using this key combination.\n"
              "Right-click the tray icon to choose a different hotkey.";
        MessageBoxA(nullptr, msg.c_str(), "Volume Hotkey",
                    MB_OK | MB_ICONWARNING);
    }

    // Try to find target process immediately
    DWORD pid = findAudioProcessByName(g_config.targetProcess);
    if (pid != 0) {
        g_state.targetProcessId = pid;
        g_state.targetName      = g_config.targetProcess;
        g_state.isActive        = true;
    }
    // If not found, toggleVolume() will retry on each hotkey press

    // Main message loop - blocks here until WM_QUIT
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Clean up
    UnregisterHotKey(g_hwndHidden, HOTKEY_ID);
    Shell_NotifyIconA(NIM_DELETE, &g_trayIcon);
    CoUninitialize();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}


// ============================================================================
//
// BUILD INSTRUCTIONS:
//
// OPTION 1: MinGW (g++ via MSYS2 or standalone MinGW-w64)
//   g++ -o volume_hotkey.exe volume_hotkey.cpp -lole32 -loleaut32 -luser32 -lshell32 -lcomctl32 -lgdi32 -mwindows
//
// OPTION 2: MSVC (Developer Command Prompt)
//   cl /EHsc volume_hotkey.cpp /link /SUBSYSTEM:WINDOWS ole32.lib oleaut32.lib user32.lib shell32.lib comctl32.lib gdi32.lib
//
// OPTION 3: Visual Studio IDE
//   1. Create a "Windows Desktop Application" project (NOT Console App)
//   2. Replace the generated .cpp with this file
//   3. Build (Ctrl+B) and run
//
// OPTION 4: CMakeLists.txt
//   cmake_minimum_required(VERSION 3.10)
//   project(VolumeHotkey)
//   add_executable(volume_hotkey WIN32 volume_hotkey.cpp)
//   target_link_libraries(volume_hotkey ole32 oleaut32 user32 shell32 comctl32 gdi32)
//
// AFTER BUILDING:
//   1. Just double-click volume_hotkey.exe
//   2. First run: config dialog appears - pick your app, set hotkey, save
//   3. A tray icon appears (bottom-right, may need to click ^ to see it)
//   4. Play your game, press your hotkey to toggle volume!
//   5. Right-click tray icon to reconfigure or quit
//
// SHARING WITH FRIENDS:
//   Just send them the .exe file. No installer needed.
//   On first run it creates volume_hotkey.ini next to the .exe.
//
// CHANGELOG v2.1:
//   - FIX: Hotkey now actually works! RegisterHotKey targets g_hwndHidden
//     instead of nullptr, so WM_HOTKEY messages are properly dispatched.
//   - NEW: Hotkey capture control - click the field and press your combo
//     instead of picking from dropdowns. Uses Windows built-in
//     msctls_hotkey32 control.
//   - NEW: Reset button to restore default hotkey (Ctrl+Shift+M)
//   - NEW: Validation prevents bare keys (no modifier) as hotkeys
//   - NEW: Auto-restores volume when you quit via tray menu
//
// ============================================================================