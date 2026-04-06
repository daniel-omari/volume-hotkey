// ============================================================================
// Volume Toggle Hotkey - Windows 11 Per-App Volume Controller
// ============================================================================
// A utility that lets you toggle a specific app's volume between its current
// level and a "ducked" level (e.g. 20%) using a global hotkey.
//
// HOW THIS WORKS:
//   1. On launch, it lists all apps currently producing audio
//   2. You pick which app to control
//   3. A global hotkey (Ctrl+Shift+M) toggles that app's volume
//
// BUILD INSTRUCTIONS (see bottom of file for full details):
//   g++ -o volume_toggle.exe volume_toggle.cpp -lole32 -loleaut32 -luser32
// ============================================================================

// --- Prevent min/max macros from Windows.h clashing with std::min/std::max ---
#define NOMINMAX

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <comdef.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <locale>
#include <codecvt>

// ============================================================================
// CONFIGURATION - Feel free to tweak these!
// ============================================================================
const float DUCKED_VOLUME   = 0.20f;  // 20% volume when "ducked"
const int   HOTKEY_ID       = 1;      // ID for registered hotkey
const UINT  HOTKEY_MODIFIER = MOD_CONTROL | MOD_SHIFT;  // Ctrl+Shift
const UINT  HOTKEY_KEY      = 'M';    // + M key

// ============================================================================
// DATA STRUCTURES
// ============================================================================

// Represents one audio session (one app producing sound)
struct AudioSession {
    DWORD       processId;
    std::string processName;
    float       currentVolume;  // 0.0 to 1.0
};

// Holds the state for our volume toggle
struct ToggleState {
    DWORD targetProcessId;
    std::string targetName;
    float originalVolume;
    bool  isDucked;
};

// ============================================================================
// HELPER: Convert wide string to narrow string (UTF-16 -> UTF-8)
// ============================================================================
std::string wideToNarrow(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

// ============================================================================
// HELPER: Get process name from PID
// ============================================================================
std::string getProcessName(DWORD pid) {
    if (pid == 0) return "[System Sounds]";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return "[Unknown PID: " + std::to_string(pid) + "]";

    char path[MAX_PATH];
    DWORD pathSize = MAX_PATH;

    std::string name = "[Unknown]";
    if (QueryFullProcessImageNameA(hProcess, 0, path, &pathSize)) {
        // Extract just the filename from the full path
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
// WINDOWS AUDIO API: Enumerate all audio sessions
// ----------------------------------------------------------------------------
// This is the "heavy lifting" part using COM and the Core Audio API.
//
// The chain of interfaces:
//   IMMDeviceEnumerator -> IMMDevice (default speaker)
//     -> IAudioSessionManager2 -> IAudioSessionEnumerator
//       -> IAudioSessionControl -> IAudioSessionControl2 (get PID)
//                               -> ISimpleAudioVolume    (get/set volume)
// ============================================================================
std::vector<AudioSession> enumerateAudioSessions() {
    std::vector<AudioSession> sessions;

    // Get the default audio output device
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create device enumerator (0x"
                  << std::hex << hr << ")\n";
        return sessions;
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) {
        std::cerr << "No default audio device found.\n";
        pEnumerator->Release();
        return sessions;
    }

    // Get the session manager for this device
    IAudioSessionManager2* pSessionManager = nullptr;
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL,
        nullptr, (void**)&pSessionManager
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to get session manager.\n";
        pDevice->Release();
        pEnumerator->Release();
        return sessions;
    }

    // Get the session enumerator
    IAudioSessionEnumerator* pSessionEnum = nullptr;
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr)) {
        std::cerr << "Failed to enumerate sessions.\n";
        pSessionManager->Release();
        pDevice->Release();
        pEnumerator->Release();
        return sessions;
    }

    int sessionCount = 0;
    pSessionEnum->GetCount(&sessionCount);

    for (int i = 0; i < sessionCount; i++) {
        IAudioSessionControl* pSessionControl = nullptr;
        hr = pSessionEnum->GetSession(i, &pSessionControl);
        if (FAILED(hr)) continue;

        // Query for the extended control interface (gives us the PID)
        IAudioSessionControl2* pSessionControl2 = nullptr;
        hr = pSessionControl->QueryInterface(
            __uuidof(IAudioSessionControl2), (void**)&pSessionControl2
        );
        if (FAILED(hr)) {
            pSessionControl->Release();
            continue;
        }

        // Query for the volume interface
        ISimpleAudioVolume* pVolume = nullptr;
        hr = pSessionControl->QueryInterface(
            __uuidof(ISimpleAudioVolume), (void**)&pVolume
        );
        if (FAILED(hr)) {
            pSessionControl2->Release();
            pSessionControl->Release();
            continue;
        }

        // Extract info
        DWORD pid = 0;
        pSessionControl2->GetProcessId(&pid);

        float volume = 1.0f;
        pVolume->GetMasterVolume(&volume);

        AudioSession session;
        session.processId     = pid;
        session.processName   = getProcessName(pid);
        session.currentVolume = volume;
        sessions.push_back(session);

        // Clean up COM objects for this iteration
        pVolume->Release();
        pSessionControl2->Release();
        pSessionControl->Release();
    }

    // Clean up
    pSessionEnum->Release();
    pSessionManager->Release();
    pDevice->Release();
    pEnumerator->Release();

    return sessions;
}

// ============================================================================
// WINDOWS AUDIO API: Set the volume for a specific process
// ----------------------------------------------------------------------------
// Similar to enumeration, but we find a specific PID and set its volume.
// Returns true on success, false on failure.
// ============================================================================
bool setProcessVolume(DWORD targetPid, float volume) {
    // Clamp volume to valid range
    volume = std::max(0.0f, std::min(1.0f, volume));

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator
    );
    if (FAILED(hr)) return false;

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) { pEnumerator->Release(); return false; }

    IAudioSessionManager2* pSessionManager = nullptr;
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL,
        nullptr, (void**)&pSessionManager
    );
    if (FAILED(hr)) {
        pDevice->Release();
        pEnumerator->Release();
        return false;
    }

    IAudioSessionEnumerator* pSessionEnum = nullptr;
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr)) {
        pSessionManager->Release();
        pDevice->Release();
        pEnumerator->Release();
        return false;
    }

    int sessionCount = 0;
    pSessionEnum->GetCount(&sessionCount);
    bool success = false;

    for (int i = 0; i < sessionCount; i++) {
        IAudioSessionControl* pSessionControl = nullptr;
        if (FAILED(pSessionEnum->GetSession(i, &pSessionControl))) continue;

        IAudioSessionControl2* pSessionControl2 = nullptr;
        if (FAILED(pSessionControl->QueryInterface(
                __uuidof(IAudioSessionControl2), (void**)&pSessionControl2))) {
            pSessionControl->Release();
            continue;
        }

        DWORD pid = 0;
        pSessionControl2->GetProcessId(&pid);

        if (pid == targetPid) {
            ISimpleAudioVolume* pVolume = nullptr;
            if (SUCCEEDED(pSessionControl->QueryInterface(
                    __uuidof(ISimpleAudioVolume), (void**)&pVolume))) {
                // nullptr for the event context GUID means "no notification"
                hr = pVolume->SetMasterVolume(volume, nullptr);
                success = SUCCEEDED(hr);
                pVolume->Release();
            }
            pSessionControl2->Release();
            pSessionControl->Release();
            break;  // Found our target, no need to keep looking
        }

        pSessionControl2->Release();
        pSessionControl->Release();
    }

    pSessionEnum->Release();
    pSessionManager->Release();
    pDevice->Release();
    pEnumerator->Release();

    return success;
}

// ============================================================================
// WINDOWS AUDIO API: Get the current volume for a specific process
// Returns -1.0f if the process wasn't found.
// ============================================================================
float getProcessVolume(DWORD targetPid) {
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator
    );
    if (FAILED(hr)) return -1.0f;

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) { pEnumerator->Release(); return -1.0f; }

    IAudioSessionManager2* pSessionManager = nullptr;
    hr = pDevice->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL,
        nullptr, (void**)&pSessionManager
    );
    if (FAILED(hr)) {
        pDevice->Release();
        pEnumerator->Release();
        return -1.0f;
    }

    IAudioSessionEnumerator* pSessionEnum = nullptr;
    hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
    if (FAILED(hr)) {
        pSessionManager->Release();
        pDevice->Release();
        pEnumerator->Release();
        return -1.0f;
    }

    int sessionCount = 0;
    pSessionEnum->GetCount(&sessionCount);
    float result = -1.0f;

    for (int i = 0; i < sessionCount; i++) {
        IAudioSessionControl* pSessionControl = nullptr;
        if (FAILED(pSessionEnum->GetSession(i, &pSessionControl))) continue;

        IAudioSessionControl2* pSessionControl2 = nullptr;
        if (FAILED(pSessionControl->QueryInterface(
                __uuidof(IAudioSessionControl2), (void**)&pSessionControl2))) {
            pSessionControl->Release();
            continue;
        }

        DWORD pid = 0;
        pSessionControl2->GetProcessId(&pid);

        if (pid == targetPid) {
            ISimpleAudioVolume* pVolume = nullptr;
            if (SUCCEEDED(pSessionControl->QueryInterface(
                    __uuidof(ISimpleAudioVolume), (void**)&pVolume))) {
                pVolume->GetMasterVolume(&result);
                pVolume->Release();
            }
            pSessionControl2->Release();
            pSessionControl->Release();
            break;
        }

        pSessionControl2->Release();
        pSessionControl->Release();
    }

    pSessionEnum->Release();
    pSessionManager->Release();
    pDevice->Release();
    pEnumerator->Release();

    return result;
}

// ============================================================================
// DISPLAY: Show the list of audio sessions to the user
// ============================================================================
void displaySessions(const std::vector<AudioSession>& sessions) {
    std::cout << "\n";
    std::cout << "  ======================================\n";
    std::cout << "    Apps Currently Producing Audio\n";
    std::cout << "  ======================================\n\n";

    if (sessions.empty()) {
        std::cout << "  (No audio sessions found. Play some audio first!)\n";
        return;
    }

    for (size_t i = 0; i < sessions.size(); i++) {
        int volumePercent = static_cast<int>(sessions[i].currentVolume * 100);
        std::cout << "  [" << (i + 1) << "] "
                  << sessions[i].processName
                  << "  (PID: " << sessions[i].processId
                  << ", Volume: " << volumePercent << "%)\n";
    }
    std::cout << "\n";
}


// ============================================================================
// TODO 1: Let the user pick which app to control
// ============================================================================
// GOAL: Display audio sessions, ask the user to pick one by number,
//       and return the chosen session's index.
//
// RETURN: The index (0-based) into the sessions vector, or -1 if invalid.

int getUserChoice(const std::vector<AudioSession>& sessions) {
    int appChoice;

    if (sessions.empty()) {
        return -1;
    }

    displaySessions(sessions);

    std::cout << "  Pick an app to control [1-" << sessions.size() << "]: ";

    std::cin >> appChoice;

    if (appChoice < 1 || appChoice > static_cast<int>(sessions.size())) {
        std::cout << "  Invalid choice!\n";
        return -1;
    }

    return appChoice - 1;
}


// ============================================================================
// TODO 2: Toggle the volume for the target app
// ============================================================================
// GOAL: If we're NOT ducked, save the current volume, then set it to
//       DUCKED_VOLUME. If we ARE ducked, restore it to originalVolume.
//       Update the ToggleState accordingly.

void toggleVolume(ToggleState& state) {
    if (!state.isDucked) {
        state.originalVolume = getProcessVolume(state.targetProcessId);
        setProcessVolume(state.targetProcessId, DUCKED_VOLUME);
        state.isDucked = true;
        std::cout << "  [DUCKED] " << state.targetName
                  << " -> " << static_cast<int>(DUCKED_VOLUME * 100) << "%\n";
    } else {
        setProcessVolume(state.targetProcessId, state.originalVolume);
        state.isDucked = false;
        std::cout << "  [RESTORED] " << state.targetName
                  << " -> " << static_cast<int>(state.originalVolume * 100) << "%\n";
    }
}


// ============================================================================
// TODO 3: The main function
// ============================================================================
// GOAL: Initialize COM, enumerate sessions, let the user pick an app,
//       register a hotkey, and run a message loop that calls toggleVolume
//       when the hotkey is pressed.

int main() {

    // Step 1: Initialize COM (required for all Windows Audio API calls)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "  Failed to initialize COM (0x"
                  << std::hex << hr << ")\n";
        return 1;
    }

    // Step 2: Welcome banner
    std::cout << "\n";
    std::cout << "  =============================================\n";
    std::cout << "     Volume Hotkey v1.0\n";
    std::cout << "     Per-App Volume Toggle for Windows\n";
    std::cout << "  =============================================\n";

    // Step 3: Enumerate audio sessions
    auto sessions = enumerateAudioSessions();

    // Step 4: Let the user pick an app
    int choice = getUserChoice(sessions);
    if (choice == -1) {
        std::cerr << "  No valid app selected. Exiting.\n";
        CoUninitialize();
        return 1;
    }

    // Step 5: Initialize toggle state from the chosen session
    ToggleState state;
    state.targetProcessId = sessions[choice].processId;
    state.targetName      = sessions[choice].processName;
    state.originalVolume  = sessions[choice].currentVolume;
    state.isDucked        = false;

    // Step 6: Register the global hotkey (Ctrl+Shift+M)
    if (!RegisterHotKey(nullptr, HOTKEY_ID, HOTKEY_MODIFIER, HOTKEY_KEY)) {
        std::cerr << "  Failed to register hotkey! Is another instance running?\n";
        CoUninitialize();
        return 1;
    }

    // Step 7: Print instructions
    std::cout << "\n";
    std::cout << "  Targeting: " << state.targetName
              << " (PID " << state.targetProcessId << ")\n";
    std::cout << "  Hotkey:    Ctrl+Shift+M  (toggles "
              << static_cast<int>(DUCKED_VOLUME * 100) << "% <-> original)\n";
    std::cout << "\n";
    std::cout << "  Listening for hotkey... (close this window to quit)\n";
    std::cout << "  -------------------------------------------------\n";

    // Step 8: Windows message loop - waits for hotkey events
    // GetMessage blocks until a message arrives, so this uses zero CPU while idle.
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_ID) {
            toggleVolume(state);
        }
    }

    // Step 9: Clean up
    UnregisterHotKey(nullptr, HOTKEY_ID);
    CoUninitialize();

    return 0;
}