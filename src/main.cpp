#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_AUDIO_DEVICE_CHANGED = WM_APP + 2;
constexpr UINT WM_AUDIO_SESSION_CREATED = WM_APP + 3;
constexpr UINT WM_SHOW_EXISTING_FLYOUT = WM_APP + 4;
constexpr UINT TRAY_ICON_ID = 1001;
constexpr UINT IDI_APP_ICON = 101;

constexpr UINT ID_MENU_STARTUP = 2002;
constexpr UINT ID_MENU_EXIT = 2003;
constexpr UINT ID_MENU_VIZ_OFF = 2100;
constexpr UINT ID_MENU_VIZ_HORIZON = 2101;
constexpr UINT ID_MENU_VIZ_TRACK = 2102;
constexpr UINT ID_MENU_VIZ_HALO = 2103;

enum VizMode : int {
    VIZ_OFF = 0,
    VIZ_HORIZON = 1,
    VIZ_TRACK_GLOW = 2,
    VIZ_THUMB_HALO = 3
};

constexpr UINT_PTR TIMER_UI_ANIM = 1;
constexpr UINT_PTR TIMER_HOVER_TIP = 2;
constexpr UINT_PTR TIMER_FLY_WIN = 3;
constexpr UINT_PTR TIMER_NOTICE_WIN = 4;

constexpr int SNAP_THRESHOLD = 3;
constexpr BYTE FLYOUT_MAX_ALPHA = 240; // ~94% subtle translucency
constexpr ULONGLONG ACTIVATE_GUARD_MS = 250;
constexpr size_t VIZ_BANDS = 20;

// Logical (96 DPI) flyout dimensions
constexpr int FLYOUT_W = 360;
constexpr int FLYOUT_H = 114;
constexpr int PAD_X = 18;
constexpr int TRACK_Y = 78;
constexpr int TRACK_H = 14;
constexpr int THUMB_R = 9;

// Startup notice popup dimensions
constexpr int NOTICE_W = 330;
constexpr int NOTICE_H = 136;
constexpr int OK_BTN_W = 76;
constexpr int OK_BTN_H = 28;

// Headphone icon hit box (top-left)
constexpr int HP_X = PAD_X - 4;
constexpr int HP_Y = 12;
constexpr int HP_SIZE = 26;

// Test button logical rect (top-right of flyout)
constexpr int BTN_W = 54;
constexpr int BTN_H = 22;
constexpr int BTN_X = FLYOUT_W - PAD_X - BTN_W;
constexpr int BTN_Y = 14;

const wchar_t* kAppName = L"SoundBalance";
const wchar_t* kRunRegPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kFlyoutClass = L"SoundBalanceFlyoutWnd";
const wchar_t* kHostClass = L"SoundBalanceHostWnd";
const wchar_t* kTipClass = L"SoundBalanceTipWnd";
const wchar_t* kNoticeClass = L"SoundBalanceNoticeWnd";

constexpr GUID kSubFormatFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
constexpr GUID kSubFormatPcm   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

class DeviceNotificationClient;
class SessionNotificationClient;

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND hostHwnd = nullptr;
    HWND flyoutHwnd = nullptr;
    HWND tipHwnd = nullptr;
    HWND noticeHwnd = nullptr;
    HICON trayIcon = nullptr;
    HICON trayIconBypassed = nullptr;
    ULONG_PTR gdiplusToken = 0;

    IMMDeviceEnumerator* enumerator = nullptr;
    DeviceNotificationClient* notifyClient = nullptr;
    IAudioSessionManager2* sessionMgr = nullptr;
    SessionNotificationClient* sessionNotify = nullptr;

    std::wstring currentDeviceId;
    std::wstring currentDeviceName = L"Connected audio";
    int balance = 0; // -100 (Left) .. 0 (Center) .. +100 (Right)
    bool bypassed = false;
    int vizMode = VIZ_OFF;

    // Smooth UI animation state
    float visualBalance = 0.0f;
    float thumbHoverAnim = 0.0f;
    float testHoverAnim = 0.0f;
    float bypassVisual = 0.0f;
    float slashProgress = 0.0f;
    bool hpAnimating = false;
    ULONGLONG hpAnimStartTick = 0;

    // Live audio visualizer state (captured via WASAPI loopback while flyout is open)
    std::atomic<bool> vizRunning{ false };
    std::thread vizThread;
    float vizTargetLeft = 0.0f;
    float vizTargetRight = 0.0f;
    float vizTargetOverall = 0.0f;
    std::array<float, VIZ_BANDS> vizTargetBands{};
    float vizLeft = 0.0f;
    float vizRight = 0.0f;
    float vizOverall = 0.0f;
    std::array<float, VIZ_BANDS> vizBands{};

    // Hover states
    bool hpHovered = false;
    bool sliderHovered = false;
    bool testHovered = false;
    bool trackingMouseLeave = false;

    // Flyout open/close glide state
    bool flyClosing = false;
    int flyTargetX = 0;
    int flyTargetY = 0;
    int flyTargetW = FLYOUT_W;
    int flyTargetH = FLYOUT_H;
    int flyGlideOffset = 10;
    BYTE flyAlpha = 0;
    ULONGLONG flyWinAnimStart = 0;

    // Startup notice fade/glide state
    bool noticeClosing = false;
    int noticeTargetX = 0;
    int noticeTargetY = 0;
    int noticeTargetW = NOTICE_W;
    int noticeTargetH = NOTICE_H;
    int noticeGlideOffset = 8;
    ULONGLONG noticeAnimStart = 0;

    bool isDraggingSlider = false;
    int soloSide = 0; // 0 = off, -1 = solo Left, +1 = solo Right
    bool isMovingWindow = false;
    POINT moveStartCursor{};
    POINT moveStartWinPos{};
    ULONGLONG lastHideTick = 0;

    std::atomic<bool> testPlaying{ false };
    std::thread testThread;

    std::map<std::wstring, int> deviceBalances;
    std::mutex stateMutex;
};

AppState g_app;

// -----------------------------------------------------------------------------
// UTF-8 / UTF-16 helpers & JSON persistence (%APPDATA%\SoundBalance\settings.json)
// -----------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string EscapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

std::wstring GetSettingsPath() {
    wchar_t* appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        dir = std::wstring(appData) + L"\\SoundBalance";
        CoTaskMemFree(appData);
    } else {
        dir = L".";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.json";
}

void LoadSettings() {
    std::ifstream ifs(GetSettingsPath(), std::ios::in);
    if (!ifs.is_open()) return;

    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string content = buf.str();

    std::lock_guard<std::mutex> lock(g_app.stateMutex);
    g_app.deviceBalances.clear();

    size_t vpos = content.find("\"viz_mode\"");
    if (vpos != std::string::npos) {
        size_t colon = content.find(':', vpos);
        if (colon != std::string::npos) {
            size_t p = colon + 1;
            while (p < content.size() && (content[p] == ' ' || content[p] == '\t')) ++p;
            if (p < content.size() && content[p] >= '0' && content[p] <= '3') {
                g_app.vizMode = content[p] - '0';
            }
        }
    }

    size_t pos = content.find("\"devices\"");
    if (pos == std::string::npos) return;
    pos = content.find('{', pos);
    if (pos == std::string::npos) return;
    ++pos;

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == '\t' || content[pos] == ',')) {
            ++pos;
        }
        if (pos >= content.size() || content[pos] == '}') break;
        if (content[pos] != '"') break;
        ++pos;

        std::string key;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                ++pos;
            }
            key += content[pos++];
        }
        if (pos >= content.size()) break;
        ++pos;

        pos = content.find(':', pos);
        if (pos == std::string::npos) break;
        ++pos;

        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) ++pos;
        size_t endNum = pos;
        if (endNum < content.size() && (content[endNum] == '-' || content[endNum] == '+')) ++endNum;
        while (endNum < content.size() && content[endNum] >= '0' && content[endNum] <= '9') ++endNum;

        if (endNum > pos) {
            int val = std::clamp(std::stoi(content.substr(pos, endNum - pos)), -100, 100);
            g_app.deviceBalances[Utf8ToWide(key)] = val;
            pos = endNum;
        } else {
            break;
        }
    }
}

void SaveSettings() {
    std::map<std::wstring, int> snapshot;
    int vizMode = VIZ_OFF;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        snapshot = g_app.deviceBalances;
        vizMode = g_app.vizMode;
    }

    std::ofstream ofs(GetSettingsPath(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return;

    ofs << "{\n  \"viz_mode\": " << vizMode << ",\n  \"devices\": {\n";
    size_t idx = 0;
    for (const auto& [devId, bal] : snapshot) {
        ofs << "    \"" << EscapeJson(WideToUtf8(devId)) << "\": " << bal;
        if (++idx < snapshot.size()) ofs << ",";
        ofs << "\n";
    }
    ofs << "  }\n}\n";
}

// -----------------------------------------------------------------------------
// Windows Startup (HKCU\Software\Microsoft\Windows\CurrentVersion\Run)
// -----------------------------------------------------------------------------
bool IsStartupEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LSTATUS res = RegQueryValueExW(hKey, kAppName, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0);
}

void SetStartupEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring quoted = std::wstring(L"\"") + exePath + L"\" --startup";
        RegSetValueExW(hKey, kAppName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, kAppName);
    }
    RegCloseKey(hKey);
}

// -----------------------------------------------------------------------------
// Windows Core Audio Session Channel Balance
// -----------------------------------------------------------------------------
class SessionNotificationClient final : public IAudioSessionNotification {
public:
    explicit SessionNotificationClient(HWND hostHwnd) : m_ref(1), m_hostHwnd(hostHwnd) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *ppvInterface = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl*) override {
        if (m_hostHwnd) {
            PostMessageW(m_hostHwnd, WM_AUDIO_SESSION_CREATED, 0, 0);
        }
        return S_OK;
    }

private:
    LONG m_ref;
    HWND m_hostHwnd;
};

bool GetDefaultEndpoint(IMMDevice** outDevice) {
    *outDevice = nullptr;
    if (!g_app.enumerator) return false;
    return SUCCEEDED(g_app.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, outDevice)) && (*outDevice != nullptr);
}

std::wstring GetDeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    return result;
}

std::wstring GetDeviceFriendlyName(IMMDevice* device) {
    IPropertyStore* props = nullptr;
    std::wstring name = L"Connected audio";
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR && varName.pwszVal) {
            name = varName.pwszVal;
        }
        PropVariantClear(&varName);
        props->Release();
    }
    return name;
}

void ReleaseSessionManager() {
    if (g_app.sessionMgr) {
        if (g_app.sessionNotify) {
            g_app.sessionMgr->UnregisterSessionNotification(g_app.sessionNotify);
            g_app.sessionNotify->Release();
            g_app.sessionNotify = nullptr;
        }
        g_app.sessionMgr->Release();
        g_app.sessionMgr = nullptr;
    }
}

void AttachSessionManager(IMMDevice* device) {
    ReleaseSessionManager();
    if (!device) return;

    if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_app.sessionMgr))) && g_app.sessionMgr) {
        g_app.sessionNotify = new SessionNotificationClient(g_app.hostHwnd);
        g_app.sessionMgr->RegisterSessionNotification(g_app.sessionNotify);
    }
}

void ApplyBalanceToAllSessions(int effectiveBalance) {
    float leftScale = 1.0f;
    float rightScale = 1.0f;
    if (effectiveBalance < 0) {
        rightScale = std::clamp(1.0f + (static_cast<float>(effectiveBalance) / 100.0f), 0.0f, 1.0f);
    } else if (effectiveBalance > 0) {
        leftScale = std::clamp(1.0f - (static_cast<float>(effectiveBalance) / 100.0f), 0.0f, 1.0f);
    }

    if (!g_app.sessionMgr) {
        IMMDevice* dev = nullptr;
        if (GetDefaultEndpoint(&dev)) {
            AttachSessionManager(dev);
            dev->Release();
        }
    }
    if (!g_app.sessionMgr) return;

    IAudioSessionEnumerator* sessionEnum = nullptr;
    if (SUCCEEDED(g_app.sessionMgr->GetSessionEnumerator(&sessionEnum)) && sessionEnum) {
        int count = 0;
        if (SUCCEEDED(sessionEnum->GetCount(&count))) {
            for (int i = 0; i < count; ++i) {
                IAudioSessionControl* ctrl = nullptr;
                if (SUCCEEDED(sessionEnum->GetSession(i, &ctrl)) && ctrl) {
                    IChannelAudioVolume* chanVol = nullptr;
                    if (SUCCEEDED(ctrl->QueryInterface(__uuidof(IChannelAudioVolume), reinterpret_cast<void**>(&chanVol))) && chanVol) {
                        UINT chCount = 0;
                        if (SUCCEEDED(chanVol->GetChannelCount(&chCount)) && chCount >= 2) {
                            chanVol->SetChannelVolume(0, leftScale, nullptr);
                            chanVol->SetChannelVolume(1, rightScale, nullptr);
                        }
                        chanVol->Release();
                    }
                    ctrl->Release();
                }
            }
        }
        sessionEnum->Release();
    }
}

int GetEffectiveBalance() {
    std::lock_guard<std::mutex> lock(g_app.stateMutex);
    return g_app.bypassed ? 0 : g_app.balance;
}

void EnsureUiAnimTimer() {
    if (g_app.flyoutHwnd) {
        SetTimer(g_app.flyoutHwnd, TIMER_UI_ANIM, 16, nullptr);
    }
}

// -----------------------------------------------------------------------------
// WASAPI Loopback Live Audio Capture (runs ONLY while flyout is open & viz != Off)
// -----------------------------------------------------------------------------
void StopVizCapture() {
    g_app.vizRunning.store(false);
    if (g_app.vizThread.joinable()) {
        g_app.vizThread.join();
    }
    std::lock_guard<std::mutex> lock(g_app.stateMutex);
    g_app.vizTargetLeft = 0.0f;
    g_app.vizTargetRight = 0.0f;
    g_app.vizTargetOverall = 0.0f;
    g_app.vizTargetBands.fill(0.0f);
    g_app.vizLeft = 0.0f;
    g_app.vizRight = 0.0f;
    g_app.vizOverall = 0.0f;
    g_app.vizBands.fill(0.0f);
}

void StartVizCapture() {
    StopVizCapture();
    int mode = VIZ_OFF;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        mode = g_app.vizMode;
    }
    if (mode == VIZ_OFF) return;

    g_app.vizRunning.store(true);
    EnsureUiAnimTimer();

    g_app.vizThread = std::thread([]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* enumr = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* audioClient = nullptr;
        IAudioCaptureClient* captureClient = nullptr;
        WAVEFORMATEX* pwfx = nullptr;

        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumr))) &&
            SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient))) &&
            SUCCEEDED(audioClient->GetMixFormat(&pwfx)) && pwfx) {

            if (SUCCEEDED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 400000, 0, pwfx, nullptr)) &&
                SUCCEEDED(audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient)))) {

                bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                bool isPcm16 = (pwfx->wFormatTag == WAVE_FORMAT_PCM && pwfx->wBitsPerSample == 16);
                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
                    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                    if (IsEqualGUID(ext->SubFormat, kSubFormatFloat)) isFloat = true;
                    else if (IsEqualGUID(ext->SubFormat, kSubFormatPcm) && pwfx->wBitsPerSample == 16) isPcm16 = true;
                }

                if (SUCCEEDED(audioClient->Start())) {
                    while (g_app.vizRunning.load()) {
                        UINT32 packetLen = 0;
                        if (FAILED(captureClient->GetNextPacketSize(&packetLen))) break;

                        float peakL = 0.0f;
                        float peakR = 0.0f;
                        std::array<float, VIZ_BANDS> bands{};
                        bool gotAudio = false;

                        while (packetLen > 0 && g_app.vizRunning.load()) {
                            BYTE* data = nullptr;
                            UINT32 numFrames = 0;
                            DWORD flags = 0;
                            if (FAILED(captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) break;

                            if (numFrames > 0 && data && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                                gotAudio = true;
                                UINT32 ch = pwfx->nChannels;
                                for (UINT32 i = 0; i < numFrames; ++i) {
                                    float sL = 0.0f;
                                    float sR = 0.0f;
                                    if (isFloat) {
                                        auto* fb = reinterpret_cast<float*>(data);
                                        sL = std::abs(fb[i * ch]);
                                        sR = (ch >= 2) ? std::abs(fb[i * ch + 1]) : sL;
                                    } else if (isPcm16) {
                                        auto* ib = reinterpret_cast<int16_t*>(data);
                                        sL = std::abs(static_cast<float>(ib[i * ch]) / 32768.0f);
                                        sR = (ch >= 2) ? std::abs(static_cast<float>(ib[i * ch + 1]) / 32768.0f) : sL;
                                    }
                                    if (sL > peakL) peakL = sL;
                                    if (sR > peakR) peakR = sR;
                                    size_t bIdx = (static_cast<size_t>(i) * VIZ_BANDS) / (std::max)(1u, numFrames);
                                    if (bIdx >= VIZ_BANDS) bIdx = VIZ_BANDS - 1;
                                    float mono = 0.5f * (sL + sR);
                                    if (mono > bands[bIdx]) bands[bIdx] = mono;
                                }
                            }
                            captureClient->ReleaseBuffer(numFrames);
                            if (FAILED(captureClient->GetNextPacketSize(&packetLen))) break;
                        }

                        {
                            std::lock_guard<std::mutex> lock(g_app.stateMutex);
                            if (gotAudio) {
                                auto shape = [](float x) {
                                    return std::clamp(std::pow(x, 0.6f) * 1.15f, 0.0f, 1.0f);
                                };
                                g_app.vizTargetLeft = shape(peakL);
                                g_app.vizTargetRight = shape(peakR);
                                g_app.vizTargetOverall = shape(0.5f * (peakL + peakR));
                                for (size_t b = 0; b < VIZ_BANDS; ++b) {
                                    g_app.vizTargetBands[b] = shape(bands[b]);
                                }
                            } else {
                                g_app.vizTargetLeft *= 0.7f;
                                g_app.vizTargetRight *= 0.7f;
                                g_app.vizTargetOverall *= 0.7f;
                                for (size_t b = 0; b < VIZ_BANDS; ++b) g_app.vizTargetBands[b] *= 0.7f;
                            }
                        }
                        Sleep(16);
                    }
                    audioClient->Stop();
                }
            }
        }

        if (pwfx) CoTaskMemFree(pwfx);
        if (captureClient) captureClient->Release();
        if (audioClient) audioClient->Release();
        if (dev) dev->Release();
        if (enumr) enumr->Release();
        CoUninitialize();
    });
}

// -----------------------------------------------------------------------------
// Built-in Mono Pink-Noise Calibration Tone (WASAPI Shared Render)
// -----------------------------------------------------------------------------
void StopTestTone() {
    g_app.testPlaying.store(false);
    if (g_app.testThread.joinable()) {
        g_app.testThread.join();
    }
    if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd)) {
        InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
    }
}

void StartTestTone() {
    StopTestTone();
    g_app.testPlaying.store(true);
    EnsureUiAnimTimer();

    g_app.testThread = std::thread([]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator* enumr = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* audioClient = nullptr;
        IAudioRenderClient* renderClient = nullptr;
        WAVEFORMATEX* pwfx = nullptr;

        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumr))) &&
            SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient))) &&
            SUCCEEDED(audioClient->GetMixFormat(&pwfx)) && pwfx) {

            REFERENCE_TIME hnsRequestedDuration = 1000000;
            if (SUCCEEDED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsRequestedDuration, 0, pwfx, nullptr)) &&
                SUCCEEDED(audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient)))) {

                UINT32 bufferFrames = 0;
                audioClient->GetBufferSize(&bufferFrames);

                bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                bool isPcm16 = (pwfx->wFormatTag == WAVE_FORMAT_PCM && pwfx->wBitsPerSample == 16);
                if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
                    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
                    if (IsEqualGUID(ext->SubFormat, kSubFormatFloat)) isFloat = true;
                    else if (IsEqualGUID(ext->SubFormat, kSubFormatPcm) && pwfx->wBitsPerSample == 16) isPcm16 = true;
                }

                double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
                uint32_t rng = 0x12345678u;
                double fade = 0.0;
                const double fadeStep = 1.0 / (std::max)(1.0, pwfx->nSamplesPerSec * 0.03);

                auto nextPink = [&]() -> float {
                    rng = rng * 1664525u + 1013904223u;
                    double white = (static_cast<double>(rng) / 2147483648.0) - 1.0;
                    b0 = 0.99886 * b0 + white * 0.0555179;
                    b1 = 0.99332 * b1 + white * 0.0750759;
                    b2 = 0.96900 * b2 + white * 0.1538520;
                    b3 = 0.86650 * b3 + white * 0.3104856;
                    b4 = 0.55000 * b4 + white * 0.5329522;
                    b5 = -0.7616 * b5 - white * 0.0168980;
                    double pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.035;
                    b6 = white * 0.115926;
                    fade = (std::min)(1.0, fade + fadeStep);
                    return static_cast<float>(std::clamp(pink * fade, -0.5, 0.5));
                };

                if (SUCCEEDED(audioClient->Start())) {
                    if (g_app.hostHwnd) {
                        PostMessageW(g_app.hostHwnd, WM_AUDIO_SESSION_CREATED, 0, 0);
                    }

                    while (g_app.testPlaying.load()) {
                        UINT32 padding = 0;
                        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;
                        UINT32 avail = bufferFrames - padding;
                        if (avail > 0) {
                            BYTE* data = nullptr;
                            if (SUCCEEDED(renderClient->GetBuffer(avail, &data)) && data) {
                                UINT32 ch = pwfx->nChannels;
                                if (isFloat) {
                                    auto* fbuf = reinterpret_cast<float*>(data);
                                    for (UINT32 i = 0; i < avail; ++i) {
                                        float s = nextPink();
                                        for (UINT32 c = 0; c < ch; ++c) {
                                            fbuf[i * ch + c] = (c < 2) ? s : 0.0f;
                                        }
                                    }
                                } else if (isPcm16) {
                                    auto* ibuf = reinterpret_cast<int16_t*>(data);
                                    for (UINT32 i = 0; i < avail; ++i) {
                                        int16_t s = static_cast<int16_t>(nextPink() * 32767.0f);
                                        for (UINT32 c = 0; c < ch; ++c) {
                                            ibuf[i * ch + c] = (c < 2) ? s : 0;
                                        }
                                    }
                                }
                                renderClient->ReleaseBuffer(avail, 0);
                            }
                        }
                        Sleep(15);
                    }
                    audioClient->Stop();
                }
            }
        }

        if (pwfx) CoTaskMemFree(pwfx);
        if (renderClient) renderClient->Release();
        if (audioClient) audioClient->Release();
        if (dev) dev->Release();
        if (enumr) enumr->Release();
        CoUninitialize();
    });

    if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd)) {
        InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
    }
}

void SyncActiveDeviceAndApplySaved() {
    IMMDevice* device = nullptr;
    if (!GetDefaultEndpoint(&device)) {
        ReleaseSessionManager();
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.currentDeviceId.clear();
        g_app.currentDeviceName = L"No audio output";
        g_app.balance = 0;
        g_app.visualBalance = 0.0f;
        return;
    }

    std::wstring id = GetDeviceId(device);
    std::wstring name = GetDeviceFriendlyName(device);
    AttachSessionManager(device);
    device->Release();

    int targetBal = 0;
    bool isBypassed = false;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.currentDeviceId = id;
        g_app.currentDeviceName = name;
        auto it = g_app.deviceBalances.find(id);
        if (it != g_app.deviceBalances.end()) {
            targetBal = it->second;
        } else if (!id.empty()) {
            g_app.deviceBalances[id] = 0;
        }
        g_app.balance = targetBal;
        g_app.visualBalance = static_cast<float>(targetBal);
        isBypassed = g_app.bypassed;
    }

    ApplyBalanceToAllSessions(isBypassed ? 0 : targetBal);
}

class DeviceNotificationClient final : public IMMNotificationClient {
public:
    explicit DeviceNotificationClient(HWND hostHwnd) : m_ref(1), m_hostHwnd(hostHwnd) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppvInterface) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppvInterface = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && m_hostHwnd) {
            PostMessageW(m_hostHwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (m_hostHwnd) PostMessageW(m_hostHwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG m_ref;
    HWND m_hostHwnd;
};

// -----------------------------------------------------------------------------
// DPI, Theme, Color Lerp, Hit-Testing & GDI+ Drawing Helpers
// -----------------------------------------------------------------------------
float GetScaleForWindow(HWND hwnd) {
    UINT dpi = 96;
    if (hwnd) {
        dpi = GetDpiForWindow(hwnd);
    }
    return static_cast<float>(dpi) / 96.0f;
}

bool IsLightTheme() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD val = 0;
    DWORD size = sizeof(val);
    bool light = false;
    if (RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
        light = (val != 0);
    } else if (RegQueryValueExW(hKey, L"SystemUsesLightTheme", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS) {
        light = (val != 0);
    }
    RegCloseKey(hKey);
    return light;
}

void ApplyDwmThemeToWindow(HWND hwnd) {
    if (!hwnd) return;
    BOOL darkMode = IsLightTheme() ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
}

Gdiplus::Color LerpColor(const Gdiplus::Color& a, const Gdiplus::Color& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    BYTE al = static_cast<BYTE>(std::lround(a.GetA() + (b.GetA() - a.GetA()) * t));
    BYTE r  = static_cast<BYTE>(std::lround(a.GetR() + (b.GetR() - a.GetR()) * t));
    BYTE g  = static_cast<BYTE>(std::lround(a.GetG() + (b.GetG() - a.GetG()) * t));
    BYTE bl = static_cast<BYTE>(std::lround(a.GetB() + (b.GetB() - a.GetB()) * t));
    return Gdiplus::Color(al, r, g, bl);
}

RECT GetHeadphoneHitRect(HWND hwnd) {
    float s = GetScaleForWindow(hwnd);
    return {
        static_cast<LONG>(std::lround(HP_X * s)),
        static_cast<LONG>(std::lround(HP_Y * s)),
        static_cast<LONG>(std::lround((HP_X + HP_SIZE) * s)),
        static_cast<LONG>(std::lround((HP_Y + HP_SIZE) * s))
    };
}

bool IsPointInHeadphoneIcon(HWND hwnd, int x, int y) {
    RECT r = GetHeadphoneHitRect(hwnd);
    POINT pt{ x, y };
    return PtInRect(&r, pt) != FALSE;
}

bool IsPointInTestButton(HWND hwnd, int x, int y) {
    float s = GetScaleForWindow(hwnd);
    float bx = BTN_X * s;
    float by = BTN_Y * s;
    float bw = BTN_W * s;
    float bh = BTN_H * s;
    return (x >= bx - 2 * s && x <= bx + bw + 2 * s && y >= by - 2 * s && y <= by + bh + 2 * s);
}


bool IsPointInLeftLabel(HWND hwnd, int x, int y) {
    float s = GetScaleForWindow(hwnd);
    return (x >= PAD_X * s && x <= (PAD_X + 54) * s && y >= 42 * s && y <= 66 * s);
}

bool IsPointInRightLabel(HWND hwnd, int x, int y) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float s = GetScaleForWindow(hwnd);
    float w = static_cast<float>(rc.right - rc.left);
    return (x >= w - (PAD_X + 54) * s && x <= w - PAD_X * s && y >= 42 * s && y <= 66 * s);
}

bool IsPointInSliderTrack(HWND hwnd, int x, int y) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float s = GetScaleForWindow(hwnd);
    float left = (PAD_X - THUMB_R) * s;
    float right = static_cast<float>(rc.right - rc.left) - (PAD_X - THUMB_R) * s;
    float top = (TRACK_Y - 10) * s;
    float bottom = (TRACK_Y + TRACK_H + 10) * s;
    return (x >= left && x <= right && y >= top && y <= bottom);
}

void ShowHoverTip() {
    if (!g_app.tipHwnd || !g_app.flyoutHwnd || !IsWindowVisible(g_app.flyoutHwnd)) return;
    float s = GetScaleForWindow(g_app.flyoutHwnd);
    int tipW = static_cast<int>(std::lround(58 * s));
    int tipH = static_cast<int>(std::lround(24 * s));

    RECT hp = GetHeadphoneHitRect(g_app.flyoutHwnd);
    POINT pt{ (hp.left + hp.right) / 2, hp.top };
    ClientToScreen(g_app.flyoutHwnd, &pt);

    int x = pt.x - tipW / 2;
    int y = pt.y - tipH - static_cast<int>(std::lround(4 * s));
    SetWindowPos(g_app.tipHwnd, HWND_TOPMOST, x, y, tipW, tipH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_app.tipHwnd, nullptr, FALSE);
}

void HideHoverTip() {
    if (g_app.flyoutHwnd) KillTimer(g_app.flyoutHwnd, TIMER_HOVER_TIP);
    if (g_app.tipHwnd && IsWindowVisible(g_app.tipHwnd)) {
        ShowWindow(g_app.tipHwnd, SW_HIDE);
    }
}


HICON CreateBypassedTrayIcon(HICON baseIcon) {
    const int sz = GetSystemMetrics(SM_CXSMICON);
    Gdiplus::Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (baseIcon) {
            HDC hdc = g.GetHDC();
            DrawIconEx(hdc, 0, 0, baseIcon, sz, sz, 0, nullptr, DI_NORMAL);
            g.ReleaseHDC(hdc);
        }

        // Subtle dark dim + crisp amber '/' slash to indicate Original (Bypassed)
        Gdiplus::SolidBrush dim(Gdiplus::Color(90, 20, 20, 20));
        g.FillEllipse(&dim, 1.0f, 1.0f, static_cast<float>(sz - 2), static_cast<float>(sz - 2));

        float pad = sz * 0.18f;
        Gdiplus::Pen halo(Gdiplus::Color(230, 24, 23, 21), (std::max)(2.4f, sz * 0.16f));
        halo.SetStartCap(Gdiplus::LineCapRound);
        halo.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&halo, static_cast<float>(sz) - pad, pad, pad, static_cast<float>(sz) - pad);

        Gdiplus::Pen slash(Gdiplus::Color(255, 230, 171, 36), (std::max)(1.4f, sz * 0.09f));
        slash.SetStartCap(Gdiplus::LineCapRound);
        slash.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&slash, static_cast<float>(sz) - pad, pad, pad, static_cast<float>(sz) - pad);
    }
    HICON out = nullptr;
    bmp.GetHICON(&out);
    return out;
}

void UpdateTrayIcon() {
    if (!g_app.hostHwnd) return;
    bool byp = false;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        byp = g_app.bypassed;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hostHwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = (byp && g_app.trayIconBypassed) ? g_app.trayIconBypassed : g_app.trayIcon;
    wcscpy_s(nid.szTip, byp ? L"Sound Balance (Off)" : L"Sound Balance");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

HICON CreateAppTrayIcon() {
    const int sz = 32;
    Gdiplus::Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 28, 27, 25));
    g.FillEllipse(&bgBrush, 1.0f, 1.0f, 30.0f, 30.0f);
    Gdiplus::Pen ringPen(Gdiplus::Color(255, 229, 169, 34), 2.0f);
    g.DrawEllipse(&ringPen, 2.0f, 2.0f, 28.0f, 28.0f);

    HICON hIcon = nullptr;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

void DrawHeadphoneGlyph(Gdiplus::Graphics& g, float cx, float cy, float size, const Gdiplus::Color& color, float slashProg, float animT, bool animating, float scale) {
    if (animating && animT > 0.0f && animT < 1.0f) {
        float r1 = (7.0f + 13.0f * animT) * scale;
        int alpha1 = static_cast<int>((1.0f - animT) * 145.0f);
        Gdiplus::Pen rip1(Gdiplus::Color(alpha1, 230, 171, 36), 1.6f * scale);
        g.DrawEllipse(&rip1, cx - r1, cy - r1, r1 * 2.0f, r1 * 2.0f);

        if (animT > 0.18f) {
            float t2 = (animT - 0.18f) / 0.82f;
            float r2 = (6.0f + 11.0f * t2) * scale;
            int alpha2 = static_cast<int>((1.0f - t2) * 90.0f);
            Gdiplus::Pen rip2(Gdiplus::Color(alpha2, 242, 240, 236), 1.1f * scale);
            g.DrawEllipse(&rip2, cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f);
        }
    }

    float iconScale = 1.0f;
    float yOffset = 0.0f;
    if (animating && animT > 0.0f && animT < 1.0f) {
        float damped = std::exp(-5.0f * animT) * std::sin(animT * 12.566f);
        iconScale = 1.0f - 0.16f * damped;
        yOffset = 1.8f * scale * std::exp(-5.0f * animT) * std::sin(animT * 6.283f);
    }

    float curSize = size * iconScale;
    float x = cx - curSize * 0.5f;
    float y = cy - curSize * 0.5f + yOffset;

    Gdiplus::Pen pen(color, curSize * 0.11f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    g.DrawArc(&pen, x + curSize * 0.12f, y + curSize * 0.10f, curSize * 0.76f, curSize * 0.78f, 180.0f, 180.0f);

    float cupW = curSize * 0.22f;
    float cupH = curSize * 0.36f;
    float cupY = y + curSize * 0.48f;

    Gdiplus::GraphicsPath leftCup;
    float r = cupW * 0.4f;
    float lx = x + curSize * 0.10f;
    leftCup.AddArc(lx, cupY, r * 2, r * 2, 180, 90);
    leftCup.AddArc(lx + cupW - r * 2, cupY, r * 2, r * 2, 270, 90);
    leftCup.AddArc(lx + cupW - r * 2, cupY + cupH - r * 2, r * 2, r * 2, 0, 90);
    leftCup.AddArc(lx, cupY + cupH - r * 2, r * 2, r * 2, 90, 90);
    leftCup.CloseFigure();
    g.DrawPath(&pen, &leftCup);

    Gdiplus::GraphicsPath rightCup;
    float rx = x + curSize * 0.68f;
    rightCup.AddArc(rx, cupY, r * 2, r * 2, 180, 90);
    rightCup.AddArc(rx + cupW - r * 2, cupY, r * 2, r * 2, 270, 90);
    rightCup.AddArc(rx + cupW - r * 2, cupY + cupH - r * 2, r * 2, r * 2, 0, 90);
    rightCup.AddArc(rx, cupY + cupH - r * 2, r * 2, r * 2, 90, 90);
    rightCup.CloseFigure();
    g.DrawPath(&pen, &rightCup);

    if (slashProg > 0.01f) {
        float halfLen = (size * 0.58f) * slashProg;
        float x1 = cx + halfLen;
        float y1 = cy + yOffset - halfLen;
        float x2 = cx - halfLen;
        float y2 = cy + yOffset + halfLen;
        int alpha = static_cast<int>(std::clamp(slashProg, 0.0f, 1.0f) * 255.0f);

        Gdiplus::Pen haloPen(Gdiplus::Color(alpha, 28, 27, 25), 3.6f * scale);
        haloPen.SetStartCap(Gdiplus::LineCapRound);
        haloPen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&haloPen, x1, y1, x2, y2);

        Gdiplus::Pen slashPen(Gdiplus::Color(alpha, 230, 171, 36), 2.0f * scale);
        slashPen.SetStartCap(Gdiplus::LineCapRound);
        slashPen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&slashPen, x1, y1, x2, y2);
    }
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void PaintFlyout(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    {
        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        float s = GetScaleForWindow(hwnd);
        bool light = IsLightTheme();

        Gdiplus::Color cBg          = light ? Gdiplus::Color(255, 246, 244, 240) : Gdiplus::Color(255, 28, 27, 25);
        Gdiplus::Color cBorder      = light ? Gdiplus::Color(255, 221, 216, 206) : Gdiplus::Color(255, 48, 45, 40);
        Gdiplus::Color cTitle       = light ? Gdiplus::Color(255, 28, 27, 25)    : Gdiplus::Color(255, 242, 240, 236);
        Gdiplus::Color cLabel       = light ? Gdiplus::Color(255, 92, 88, 82)    : Gdiplus::Color(255, 198, 194, 186);
        Gdiplus::Color cHpActive    = light ? Gdiplus::Color(255, 44, 42, 38)    : Gdiplus::Color(255, 232, 229, 224);
        Gdiplus::Color cHpBypassed  = light ? Gdiplus::Color(255, 156, 150, 140) : Gdiplus::Color(255, 145, 140, 134);
        Gdiplus::Color cTrackAct    = light ? Gdiplus::Color(255, 234, 217, 183) : Gdiplus::Color(255, 102, 74, 23);
        Gdiplus::Color cTrackByp    = light ? Gdiplus::Color(255, 224, 220, 212) : Gdiplus::Color(255, 64, 56, 42);
        Gdiplus::Color cDotAct      = light ? Gdiplus::Color(255, 181, 132, 32)  : Gdiplus::Color(255, 158, 114, 34);
        Gdiplus::Color cDotByp      = light ? Gdiplus::Color(255, 178, 173, 165) : Gdiplus::Color(255, 102, 92, 74);
        Gdiplus::Color cRingAct     = light ? Gdiplus::Color(255, 200, 138, 11)  : Gdiplus::Color(255, 230, 171, 36);
        Gdiplus::Color cRingByp     = light ? Gdiplus::Color(255, 168, 162, 152) : Gdiplus::Color(255, 130, 118, 94);
        Gdiplus::Color cReadAct     = light ? Gdiplus::Color(255, 163, 111, 8)   : Gdiplus::Color(255, 214, 173, 84);
        Gdiplus::Color cReadMuted   = light ? Gdiplus::Color(255, 120, 115, 106) : Gdiplus::Color(255, 148, 144, 136);

        Gdiplus::SolidBrush bgBrush(cBg);
        g.FillRectangle(&bgBrush, 0, 0, w, h);

        std::wstring devName;
        int bal = 0;
        int vizMode = VIZ_OFF;
        float visBal = 0.0f;
        float thumbSwell = 0.0f;
        float testHover = 0.0f;
        float bypassT = 0.0f;
        bool isBypassed = false;
        float slashProg = 0.0f;
        bool animating = false;
        ULONGLONG animStart = 0;
        float vLeft = 0.0f, vRight = 0.0f, vOverall = 0.0f;
        std::array<float, VIZ_BANDS> vBands{};
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            devName = g_app.currentDeviceName;
            bal = g_app.balance;
            vizMode = g_app.vizMode;
            visBal = g_app.visualBalance;
            thumbSwell = g_app.thumbHoverAnim;
            testHover = g_app.testHoverAnim;
            bypassT = g_app.bypassVisual;
            isBypassed = g_app.bypassed;
            slashProg = g_app.slashProgress;
            animating = g_app.hpAnimating;
            animStart = g_app.hpAnimStartTick;
            vLeft = g_app.vizLeft;
            vRight = g_app.vizRight;
            vOverall = g_app.vizOverall;
            vBands = g_app.vizBands;
        }

        float padX = PAD_X * s;

        // Viz Mode 1: Subtle Background Horizon behind the lower card
        if (vizMode == VIZ_HORIZON) {
            float areaW = static_cast<float>(w) - padX * 2.0f;
            float step = areaW / static_cast<float>(VIZ_BANDS);
            float barW = (std::max)(2.0f, step * 0.56f);
            float maxH = 38.0f * s;
            BYTE alpha = light ? 42 : 34;
            Gdiplus::Color barBase = LerpColor(cRingAct, cRingByp, bypassT);
            Gdiplus::SolidBrush barBrush(Gdiplus::Color(alpha, barBase.GetR(), barBase.GetG(), barBase.GetB()));

            for (size_t i = 0; i < VIZ_BANDS; ++i) {
                float bh = (2.0f * s) + vBands[i] * maxH;
                float bx = padX + static_cast<float>(i) * step + (step - barW) * 0.5f;
                float by = static_cast<float>(h) - bh;
                Gdiplus::GraphicsPath bp;
                float br = barW * 0.5f;
                bp.AddArc(bx, by, barW, barW, 180, 180);
                bp.AddLine(bx + barW, by + br, bx + barW, static_cast<float>(h));
                bp.AddLine(bx, static_cast<float>(h), bx, by + br);
                bp.CloseFigure();
                g.FillPath(&barBrush, &bp);
            }
        }

        Gdiplus::Pen borderPen(cBorder, 1.0f);
        g.DrawRectangle(&borderPen, 0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);

        float animT = 0.0f;
        if (animating) {
            animT = std::clamp(static_cast<float>(GetTickCount64() - animStart) / 360.0f, 0.0f, 1.0f);
        }

        float hpCx = padX + 9.0f * s;
        float hpCy = 25.0f * s;
        Gdiplus::Color hpColor = LerpColor(cHpActive, cHpBypassed, bypassT);
        DrawHeadphoneGlyph(g, hpCx, hpCy, 18.0f * s, hpColor, slashProg, animT, animating, s);

        Gdiplus::FontFamily fontFam(L"Segoe UI");
        Gdiplus::Font titleFont(&fontFam, 14.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(cTitle);
        Gdiplus::StringFormat titleFmt;
        titleFmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        titleFmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        float btnX = BTN_X * s;
        float btnY = BTN_Y * s;
        float btnW = BTN_W * s;
        float btnH = BTN_H * s;

        Gdiplus::RectF titleRect(padX + 28.0f * s, 15.0f * s, btnX - (padX + 36.0f * s), 22.0f * s);
        g.DrawString(devName.c_str(), -1, &titleFont, titleRect, &titleFmt, &titleBrush);

        bool playing = g_app.testPlaying.load();
        Gdiplus::GraphicsPath btnPath;
        AddRoundedRect(btnPath, btnX, btnY, btnW, btnH, btnH * 0.5f);

        Gdiplus::Color idleFill = light
            ? LerpColor(Gdiplus::Color(255, 234, 230, 222), Gdiplus::Color(255, 223, 218, 208), testHover)
            : LerpColor(Gdiplus::Color(255, 42, 40, 36), Gdiplus::Color(255, 58, 54, 48), testHover);
        Gdiplus::Color idleBorder = light
            ? LerpColor(Gdiplus::Color(255, 204, 198, 186), Gdiplus::Color(255, 160, 142, 104), testHover)
            : LerpColor(Gdiplus::Color(255, 72, 68, 60), Gdiplus::Color(255, 122, 108, 76), testHover);
        Gdiplus::Color activeBorder = cRingAct;
        if (playing) {
            float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTickCount64()) * 0.005f);
            activeBorder = light
                ? LerpColor(Gdiplus::Color(255, 180, 124, 12), Gdiplus::Color(255, 224, 156, 18), pulse)
                : LerpColor(Gdiplus::Color(255, 185, 134, 30), Gdiplus::Color(255, 248, 205, 92), pulse);
        }

        Gdiplus::Color activeFill = light ? Gdiplus::Color(255, 245, 230, 196) : Gdiplus::Color(255, 102, 74, 23);
        Gdiplus::Color activeTxt  = light ? Gdiplus::Color(255, 138, 92, 0)    : Gdiplus::Color(255, 245, 200, 82);
        Gdiplus::Color idleTxt    = light ? Gdiplus::Color(255, 43, 41, 38)    : Gdiplus::Color(255, 214, 210, 202);

        Gdiplus::SolidBrush btnFill(playing ? activeFill : idleFill);
        g.FillPath(&btnFill, &btnPath);
        Gdiplus::Pen btnBorder(playing ? activeBorder : idleBorder, playing ? 1.4f : 1.0f);
        g.DrawPath(&btnBorder, &btnPath);

        Gdiplus::Font btnFont(&fontFam, 11.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush btnTextBrush(playing ? activeTxt : idleTxt);
        Gdiplus::StringFormat btnFmt;
        btnFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        btnFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(playing ? L"Stop" : L"Test", -1, &btnFont, Gdiplus::RectF(btnX, btnY - 0.5f * s, btnW, btnH), &btnFmt, &btnTextBrush);

        Gdiplus::Font labelFont(&fontFam, 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush labelBrush(cLabel);

        Gdiplus::SolidBrush soloBrush(cRingAct);
        Gdiplus::StringFormat leftFmt;
        leftFmt.SetAlignment(Gdiplus::StringAlignmentNear);
        g.DrawString(L"Left", -1, &labelFont, Gdiplus::RectF(padX, 46.0f * s, 60.0f * s, 18.0f * s), &leftFmt, (g_app.soloSide == -1) ? &soloBrush : &labelBrush);

        int leftPct = (bal > 0) ? (100 - bal) : 100;
        int rightPct = (bal < 0) ? (100 + bal) : 100;
        std::wstring readout;
        if (g_app.soloSide == -1) {
            readout = L"Solo Left";
        } else if (g_app.soloSide == 1) {
            readout = L"Solo Right";
        } else if (isBypassed) {
            readout = L"Off";
        } else if (bal == 0) {
            readout = L"Center";
        } else {
            readout = L"L " + std::to_wstring(leftPct) + L"% \u00B7 R " + std::to_wstring(rightPct) + L"%";
        }

        Gdiplus::Color readoutClr = (g_app.soloSide != 0) ? cReadAct : ((bal == 0) ? cReadMuted : LerpColor(cReadAct, cReadMuted, bypassT));

        Gdiplus::Font readoutFont(&fontFam, 11.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush readoutBrush(readoutClr);
        Gdiplus::StringFormat centerFmt;
        centerFmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(readout.c_str(), -1, &readoutFont, Gdiplus::RectF(padX + 60.0f * s, 46.0f * s, static_cast<float>(w) - (padX + 60.0f * s) * 2.0f, 18.0f * s), &centerFmt, &readoutBrush);

        Gdiplus::StringFormat rightFmt;
        rightFmt.SetAlignment(Gdiplus::StringAlignmentFar);
        g.DrawString(L"Right", -1, &labelFont, Gdiplus::RectF(static_cast<float>(w) - padX - 60.0f * s, 46.0f * s, 60.0f * s, 18.0f * s), &rightFmt, (g_app.soloSide == 1) ? &soloBrush : &labelBrush);

        float trackY = TRACK_Y * s;
        float trackH = TRACK_H * s;
        float trackLeft = padX;
        float trackRight = static_cast<float>(w) - padX;
        float capR = trackH * 0.5f;

        Gdiplus::GraphicsPath trackPath;
        trackPath.AddArc(trackLeft, trackY, trackH, trackH, 90, 180);
        trackPath.AddArc(trackRight - trackH, trackY, trackH, trackH, 270, 180);
        trackPath.CloseFigure();

        Gdiplus::SolidBrush trackBrush(LerpColor(cTrackAct, cTrackByp, bypassT));
        g.FillPath(&trackBrush, &trackPath);

        float centerY = trackY + trackH * 0.5f;
        float usableLeft = trackLeft + capR;
        float usableRight = trackRight - capR;
        float usableMid = (usableLeft + usableRight) * 0.5f;

        // Viz Mode 2: Inside-the-track L/R Glow radiating from center
        if (vizMode == VIZ_TRACK_GLOW && (vLeft > 0.01f || vRight > 0.01f)) {
            g.SetClip(&trackPath);
            Gdiplus::Color glowClr = LerpColor(cRingAct, cRingByp, bypassT);
            BYTE glowAlpha = light ? 110 : 120;
            Gdiplus::Color cStrong(glowAlpha, glowClr.GetR(), glowClr.GetG(), glowClr.GetB());
            Gdiplus::Color cFade(0, glowClr.GetR(), glowClr.GetG(), glowClr.GetB());

            float halfSpan = usableMid - trackLeft;
            if (vLeft > 0.01f) {
                float lx = usableMid - halfSpan * vLeft;
                Gdiplus::LinearGradientBrush lGrad(Gdiplus::PointF(usableMid, centerY), Gdiplus::PointF(lx, centerY), cStrong, cFade);
                g.FillRectangle(&lGrad, lx, trackY, usableMid - lx, trackH);
            }
            if (vRight > 0.01f) {
                float rx = usableMid + halfSpan * vRight;
                Gdiplus::LinearGradientBrush rGrad(Gdiplus::PointF(usableMid, centerY), Gdiplus::PointF(rx, centerY), cStrong, cFade);
                g.FillRectangle(&rGrad, usableMid, trackY, rx - usableMid, trackH);
            }
            g.ResetClip();
        }

        Gdiplus::SolidBrush dotBrush(LerpColor(cDotAct, cDotByp, bypassT));
        float dotR = 2.6f * s;
        g.FillEllipse(&dotBrush, usableLeft - dotR, centerY - dotR, dotR * 2.0f, dotR * 2.0f);
        g.FillEllipse(&dotBrush, usableMid - dotR, centerY - dotR, dotR * 2.0f, dotR * 2.0f);
        g.FillEllipse(&dotBrush, usableRight - dotR, centerY - dotR, dotR * 2.0f, dotR * 2.0f);

        float norm = (visBal + 100.0f) / 200.0f;
        float thumbX = usableLeft + norm * (usableRight - usableLeft);
        float thumbR = (THUMB_R + 1.8f * thumbSwell) * s;

        // Viz Mode 3: Subtle Thumb Beat Halo
        if (vizMode == VIZ_THUMB_HALO && vOverall > 0.02f) {
            Gdiplus::Color haloClr = LerpColor(cRingAct, cRingByp, bypassT);
            float haloR = thumbR + (3.0f + 9.0f * vOverall) * s;
            int haloAlpha = static_cast<int>(std::clamp(vOverall * 85.0f, 0.0f, 85.0f));
            Gdiplus::Pen haloPen(Gdiplus::Color(haloAlpha, haloClr.GetR(), haloClr.GetG(), haloClr.GetB()), 2.0f * s);
            g.DrawEllipse(&haloPen, thumbX - haloR, centerY - haloR, haloR * 2.0f, haloR * 2.0f);
        }

        Gdiplus::SolidBrush thumbFill(cBg);
        g.FillEllipse(&thumbFill, thumbX - thumbR, centerY - thumbR, thumbR * 2.0f, thumbR * 2.0f);

        Gdiplus::Pen thumbRing(LerpColor(cRingAct, cRingByp, bypassT), (2.4f + 0.3f * thumbSwell) * s);
        g.DrawEllipse(&thumbRing, thumbX - thumbR, centerY - thumbR, thumbR * 2.0f, thumbR * 2.0f);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

void PaintTipWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    {
        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        float s = GetScaleForWindow(hwnd);
        bool light = IsLightTheme();

        Gdiplus::SolidBrush bg(light ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 36, 34, 31));
        g.FillRectangle(&bg, 0, 0, w, h);

        Gdiplus::Pen border(light ? Gdiplus::Color(255, 212, 207, 197) : Gdiplus::Color(255, 70, 66, 58), 1.0f);
        g.DrawRectangle(&border, 0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);

        bool isBypassed = false;
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            isBypassed = g_app.bypassed;
        }

        Gdiplus::FontFamily fam(L"Segoe UI");
        Gdiplus::Font font(&fam, 11.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush txt(light ? Gdiplus::Color(255, 28, 27, 25) : Gdiplus::Color(255, 236, 233, 228));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(isBypassed ? L"Start" : L"Stop", -1, &font, Gdiplus::RectF(0, -0.5f * s, static_cast<float>(w), static_cast<float>(h)), &fmt, &txt);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

bool IsPointInOkButton(HWND hwnd, int x, int y) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float s = GetScaleForWindow(hwnd);
    float bx = static_cast<float>(rc.right - rc.left) - PAD_X * s - OK_BTN_W * s;
    float by = static_cast<float>(rc.bottom - rc.top) - 16.0f * s - OK_BTN_H * s;
    return (x >= bx && x <= bx + OK_BTN_W * s && y >= by && y <= by + OK_BTN_H * s);
}

void PaintNoticeWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

    {
        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        float s = GetScaleForWindow(hwnd);
        bool light = IsLightTheme();

        Gdiplus::SolidBrush bg(light ? Gdiplus::Color(255, 246, 244, 240) : Gdiplus::Color(255, 28, 27, 25));
        g.FillRectangle(&bg, 0, 0, w, h);
        Gdiplus::Pen border(light ? Gdiplus::Color(255, 221, 216, 206) : Gdiplus::Color(255, 56, 52, 46), 1.0f);
        g.DrawRectangle(&border, 0.5f, 0.5f, static_cast<float>(w) - 1.0f, static_cast<float>(h) - 1.0f);

        float padX = PAD_X * s;
        Gdiplus::Color hpClr = light ? Gdiplus::Color(255, 200, 138, 11) : Gdiplus::Color(255, 230, 171, 36);
        DrawHeadphoneGlyph(g, padX + 9.0f * s, 26.0f * s, 18.0f * s, hpClr, 0.0f, 0.0f, false, s);

        Gdiplus::FontFamily fam(L"Segoe UI");
        Gdiplus::Font titleFont(&fam, 14.5f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush titleBrush(light ? Gdiplus::Color(255, 28, 27, 25) : Gdiplus::Color(255, 242, 240, 236));
        g.DrawString(L"Sound Balance is active", -1, &titleFont,
                     Gdiplus::RectF(padX + 28.0f * s, 16.0f * s, static_cast<float>(w) - padX * 2.0f - 28.0f * s, 22.0f * s),
                     nullptr, &titleBrush);

        Gdiplus::Font bodyFont(&fam, 12.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush bodyBrush(light ? Gdiplus::Color(255, 92, 88, 82) : Gdiplus::Color(255, 198, 194, 186));
        g.DrawString(L"Click the icon in your system tray to open the slider.", -1, &bodyFont,
                     Gdiplus::RectF(padX, 46.0f * s, static_cast<float>(w) - padX * 2.0f, 38.0f * s),
                     nullptr, &bodyBrush);

        float bx = static_cast<float>(w) - padX - OK_BTN_W * s;
        float by = static_cast<float>(h) - 16.0f * s - OK_BTN_H * s;
        float bw = OK_BTN_W * s;
        float bh = OK_BTN_H * s;

        Gdiplus::GraphicsPath btnPath;
        AddRoundedRect(btnPath, bx, by, bw, bh, bh * 0.5f);
        Gdiplus::SolidBrush btnFill(light ? Gdiplus::Color(255, 245, 230, 196) : Gdiplus::Color(255, 102, 74, 23));
        g.FillPath(&btnFill, &btnPath);
        Gdiplus::Pen btnBorder(hpClr, 1.0f);
        g.DrawPath(&btnBorder, &btnPath);

        Gdiplus::Font btnFont(&fam, 12.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush btnTxt(light ? Gdiplus::Color(255, 138, 92, 0) : Gdiplus::Color(255, 245, 200, 82));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        g.DrawString(L"OK", -1, &btnFont, Gdiplus::RectF(bx, by - 0.5f * s, bw, bh), &fmt, &btnTxt);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

// -----------------------------------------------------------------------------
// Slider Interaction, Bypass Toggle & Flyout Positioning
// -----------------------------------------------------------------------------
void ToggleBypass(HWND hwnd) {
    bool nowBypassed = false;
    int savedBal = 0;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        g_app.bypassed = !g_app.bypassed;
        nowBypassed = g_app.bypassed;
        savedBal = g_app.balance;
        g_app.hpAnimating = true;
        g_app.hpAnimStartTick = GetTickCount64();
    }

    ApplyBalanceToAllSessions(nowBypassed ? 0 : savedBal);
    UpdateTrayIcon();
    if (g_app.tipHwnd && IsWindowVisible(g_app.tipHwnd)) {
        InvalidateRect(g_app.tipHwnd, nullptr, FALSE);
    }
    if (hwnd) {
        EnsureUiAnimTimer();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void SetBalanceValue(HWND hwnd, int newBal, bool save, bool immediateVisual) {
    newBal = std::clamp(newBal, -100, 100);
    bool wasBypassed = false;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        if (g_app.bypassed) {
            g_app.bypassed = false;
            wasBypassed = true;
            g_app.hpAnimating = true;
            g_app.hpAnimStartTick = GetTickCount64();
        }
        g_app.balance = newBal;
        if (immediateVisual) {
            g_app.visualBalance = static_cast<float>(newBal);
        }
        if (!g_app.currentDeviceId.empty()) {
            g_app.deviceBalances[g_app.currentDeviceId] = newBal;
        }
    }
    if (wasBypassed) UpdateTrayIcon();
    ApplyBalanceToAllSessions(newBal);
    if (save) SaveSettings();
    if (hwnd && IsWindowVisible(hwnd)) {
        if (wasBypassed || !immediateVisual) {
            EnsureUiAnimTimer();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void UpdateBalanceFromMouseX(HWND hwnd, int mouseX) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float s = GetScaleForWindow(hwnd);
    float padX = PAD_X * s;
    float capR = (TRACK_H * s) * 0.5f;
    float usableLeft = padX + capR;
    float usableRight = static_cast<float>(rc.right - rc.left) - padX - capR;

    float norm = (static_cast<float>(mouseX) - usableLeft) / (std::max)(1.0f, usableRight - usableLeft);
    norm = std::clamp(norm, 0.0f, 1.0f);
    int newBal = static_cast<int>(std::lround(norm * 200.0f - 100.0f));

    bool snapped = false;
    if (std::abs(newBal) <= SNAP_THRESHOLD) {
        newBal = 0;
        snapped = true;
    }

    SetBalanceValue(hwnd, newBal, false, !snapped);
}

void ResetBalanceToCenter() {
    SetBalanceValue(g_app.flyoutHwnd, 0, true, false);
}

void ShowFlyoutAtTray() {
    if (!g_app.flyoutHwnd) return;

    SyncActiveDeviceAndApplySaved();

    NOTIFYICONIDENTIFIER nii{};
    nii.cbSize = sizeof(nii);
    nii.hWnd = g_app.hostHwnd;
    nii.uID = TRAY_ICON_ID;

    RECT iconRect{};
    POINT anchor{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&nii, &iconRect))) {
        anchor.x = (iconRect.left + iconRect.right) / 2;
        anchor.y = iconRect.top;
    } else {
        GetCursorPos(&anchor);
    }

    HMONITOR hMon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);

    SetWindowPos(g_app.flyoutHwnd, nullptr, anchor.x, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    float s = GetScaleForWindow(g_app.flyoutHwnd);
    int winW = static_cast<int>(std::lround(FLYOUT_W * s));
    int winH = static_cast<int>(std::lround(FLYOUT_H * s));
    int margin = static_cast<int>(std::lround(12 * s));

    LONG safeBottom = mi.rcWork.bottom;
    HWND trayWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (trayWnd) {
        RECT trayRect{};
        if (GetWindowRect(trayWnd, &trayRect) && MonitorFromWindow(trayWnd, MONITOR_DEFAULTTONEAREST) == hMon) {
            LONG monMidY = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
            if (trayRect.top > monMidY) {
                safeBottom = (std::min)(safeBottom, trayRect.top);
            }
        }
    }

    const wchar_t* overflowClasses[] = {
        L"TopLevelWindowForOverflowXamlIsland",
        L"NotifyIconOverflowWindow"
    };
    for (const wchar_t* cls : overflowClasses) {
        HWND ovWnd = FindWindowW(cls, nullptr);
        if (ovWnd && IsWindowVisible(ovWnd)) {
            RECT ovRect{};
            if (GetWindowRect(ovWnd, &ovRect) && MonitorFromWindow(ovWnd, MONITOR_DEFAULTTONEAREST) == hMon) {
                safeBottom = (std::min)(safeBottom, ovRect.top);
            }
        }
    }
    if (iconRect.right > iconRect.left && iconRect.bottom > iconRect.top) {
        POINT iconMid{ (iconRect.left + iconRect.right) / 2, (iconRect.top + iconRect.bottom) / 2 };
        HWND hostRoot = GetAncestor(WindowFromPoint(iconMid), GA_ROOT);
        if (hostRoot && hostRoot != g_app.flyoutHwnd && IsWindowVisible(hostRoot)) {
            RECT hr{};
            if (GetWindowRect(hostRoot, &hr) && MonitorFromWindow(hostRoot, MONITOR_DEFAULTTONEAREST) == hMon) {
                safeBottom = (std::min)(safeBottom, hr.top);
            }
        }
    }

    int x = anchor.x - winW / 2;
    int y = safeBottom - winH - margin;

    if (x + winW > mi.rcWork.right - margin) x = mi.rcWork.right - winW - margin;
    if (x < mi.rcWork.left + margin) x = mi.rcWork.left + margin;
    if (y < mi.rcWork.top + margin) y = mi.rcWork.top + margin;

    g_app.flyClosing = false;
    g_app.flyTargetX = x;
    g_app.flyTargetY = y;
    g_app.flyTargetW = winW;
    g_app.flyTargetH = winH;
    g_app.flyGlideOffset = static_cast<int>(std::lround(10 * s));
    g_app.flyAlpha = 0;
    g_app.flyWinAnimStart = GetTickCount64();

    ApplyDwmThemeToWindow(g_app.flyoutHwnd);
    ApplyDwmThemeToWindow(g_app.tipHwnd);
    SetLayeredWindowAttributes(g_app.flyoutHwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(g_app.flyoutHwnd, HWND_TOPMOST, x, y + g_app.flyGlideOffset, winW, winH, SWP_SHOWWINDOW);
    SetForegroundWindow(g_app.flyoutHwnd);
    SetTimer(g_app.flyoutHwnd, TIMER_FLY_WIN, 16, nullptr);
    StartVizCapture();
    InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
}

void HideFlyout() {
    StopTestTone();
    StopVizCapture();
    HideHoverTip();
    g_app.hpHovered = false;
    g_app.sliderHovered = false;
    g_app.testHovered = false;
    if (g_app.soloSide != 0) {
        g_app.soloSide = 0;
        ApplyBalanceToAllSessions(GetEffectiveBalance());
    }
    g_app.isDraggingSlider = false;
    g_app.isMovingWindow = false;
    if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd) && !g_app.flyClosing) {
        RECT wr{};
        GetWindowRect(g_app.flyoutHwnd, &wr);
        g_app.flyTargetX = wr.left;
        g_app.flyTargetY = wr.top;
        g_app.flyClosing = true;
        g_app.flyWinAnimStart = GetTickCount64();
        g_app.lastHideTick = GetTickCount64();
        SaveSettings();
        SetTimer(g_app.flyoutHwnd, TIMER_FLY_WIN, 16, nullptr);
    }
}

void ShowNoticePopup() {
    if (!g_app.noticeHwnd) return;
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    SetWindowPos(g_app.noticeHwnd, nullptr, mi.rcWork.left, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    float s = GetScaleForWindow(g_app.noticeHwnd);
    int w = static_cast<int>(std::lround(NOTICE_W * s));
    int h = static_cast<int>(std::lround(NOTICE_H * s));
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    g_app.noticeClosing = false;
    g_app.noticeTargetX = x;
    g_app.noticeTargetY = y;
    g_app.noticeTargetW = w;
    g_app.noticeTargetH = h;
    g_app.noticeGlideOffset = static_cast<int>(std::lround(8 * s));
    g_app.noticeAnimStart = GetTickCount64();

    ApplyDwmThemeToWindow(g_app.noticeHwnd);
    SetLayeredWindowAttributes(g_app.noticeHwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(g_app.noticeHwnd, HWND_TOPMOST, x, y + g_app.noticeGlideOffset, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g_app.noticeHwnd);
    SetTimer(g_app.noticeHwnd, TIMER_NOTICE_WIN, 16, nullptr);
    InvalidateRect(g_app.noticeHwnd, nullptr, FALSE);
}

void CloseNoticePopup() {
    if (g_app.noticeHwnd && IsWindowVisible(g_app.noticeHwnd) && !g_app.noticeClosing) {
        g_app.noticeClosing = true;
        g_app.noticeAnimStart = GetTickCount64();
        SetTimer(g_app.noticeHwnd, TIMER_NOTICE_WIN, 16, nullptr);
    }
}

void ShowTrayContextMenu() {
    POINT pt{};
    GetCursorPos(&pt);

    int curViz = VIZ_OFF;
    {
        std::lock_guard<std::mutex> lock(g_app.stateMutex);
        curViz = g_app.vizMode;
    }

    HMENU vizMenu = CreatePopupMenu();
    AppendMenuW(vizMenu, MF_STRING | ((curViz == VIZ_OFF) ? MF_CHECKED : MF_UNCHECKED), ID_MENU_VIZ_OFF, L"Off");
    AppendMenuW(vizMenu, MF_STRING | ((curViz == VIZ_HORIZON) ? MF_CHECKED : MF_UNCHECKED), ID_MENU_VIZ_HORIZON, L"Background Horizon");
    AppendMenuW(vizMenu, MF_STRING | ((curViz == VIZ_TRACK_GLOW) ? MF_CHECKED : MF_UNCHECKED), ID_MENU_VIZ_TRACK, L"Track L/R Glow");
    AppendMenuW(vizMenu, MF_STRING | ((curViz == VIZ_THUMB_HALO) ? MF_CHECKED : MF_UNCHECKED), ID_MENU_VIZ_HALO, L"Thumb Beat Halo");

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(vizMenu), L"Audio Graph");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_MENU_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");

    SetForegroundWindow(g_app.hostHwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hostHwnd, nullptr);
    PostMessageW(g_app.hostHwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= ID_MENU_VIZ_OFF && cmd <= ID_MENU_VIZ_HALO) {
        int newMode = static_cast<int>(cmd - ID_MENU_VIZ_OFF);
        {
            std::lock_guard<std::mutex> lock(g_app.stateMutex);
            g_app.vizMode = newMode;
        }
        SaveSettings();
        if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd)) {
            if (newMode == VIZ_OFF) StopVizCapture();
            else StartVizCapture();
            InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
        }
    } else if (cmd == ID_MENU_STARTUP) {
        SetStartupEnabled(!IsStartupEnabled());
    } else if (cmd == ID_MENU_EXIT) {
        StopTestTone();
        StopVizCapture();
        SaveSettings();
        ApplyBalanceToAllSessions(0);
        PostQuitMessage(0);
    }
}

// -----------------------------------------------------------------------------
// Window Procedures
// -----------------------------------------------------------------------------
LRESULT CALLBACK TipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PaintTipWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK NoticeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            PaintNoticeWindow(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_NOTICE_WIN) {
                ULONGLONG elapsed = GetTickCount64() - g_app.noticeAnimStart;
                if (!g_app.noticeClosing) {
                    float t = std::clamp(static_cast<float>(elapsed) / 160.0f, 0.0f, 1.0f);
                    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
                    BYTE alpha = static_cast<BYTE>(std::lround(ease * 255.0f));
                    int curY = g_app.noticeTargetY + static_cast<int>(std::lround(g_app.noticeGlideOffset * (1.0f - ease)));
                    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
                    SetWindowPos(hwnd, nullptr, g_app.noticeTargetX, curY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    if (t >= 1.0f) KillTimer(hwnd, TIMER_NOTICE_WIN);
                } else {
                    float t = std::clamp(static_cast<float>(elapsed) / 110.0f, 0.0f, 1.0f);
                    BYTE alpha = static_cast<BYTE>(std::lround((1.0f - t) * 255.0f));
                    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
                    if (t >= 1.0f) {
                        KillTimer(hwnd, TIMER_NOTICE_WIN);
                        ShowWindow(hwnd, SW_HIDE);
                    }
                }
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, IsPointInOkButton(hwnd, pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
            if (IsPointInOkButton(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
                CloseNoticePopup();
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_ESCAPE) {
                CloseNoticePopup();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            PaintFlyout(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_FLY_WIN) {
                ULONGLONG elapsed = GetTickCount64() - g_app.flyWinAnimStart;
                if (!g_app.flyClosing) {
                    float t = std::clamp(static_cast<float>(elapsed) / 160.0f, 0.0f, 1.0f);
                    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
                    g_app.flyAlpha = static_cast<BYTE>(std::lround(ease * static_cast<float>(FLYOUT_MAX_ALPHA)));
                    int curY = g_app.flyTargetY + static_cast<int>(std::lround(g_app.flyGlideOffset * (1.0f - ease)));
                    SetLayeredWindowAttributes(hwnd, 0, g_app.flyAlpha, LWA_ALPHA);
                    SetWindowPos(hwnd, nullptr, g_app.flyTargetX, curY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    if (t >= 1.0f) KillTimer(hwnd, TIMER_FLY_WIN);
                } else {
                    float t = std::clamp(static_cast<float>(elapsed) / 110.0f, 0.0f, 1.0f);
                    BYTE alpha = static_cast<BYTE>(std::lround((1.0f - t) * static_cast<float>(g_app.flyAlpha)));
                    int curY = g_app.flyTargetY + static_cast<int>(std::lround(6.0f * t));
                    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
                    SetWindowPos(hwnd, nullptr, g_app.flyTargetX, curY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    if (t >= 1.0f) {
                        KillTimer(hwnd, TIMER_FLY_WIN);
                        g_app.flyClosing = false;
                        ShowWindow(hwnd, SW_HIDE);
                    }
                }
                return 0;
            }

            if (wParam == TIMER_UI_ANIM) {
                bool keepRunning = false;
                {
                    std::lock_guard<std::mutex> lock(g_app.stateMutex);

                    float balTarget = static_cast<float>(g_app.balance);
                    if (std::abs(balTarget - g_app.visualBalance) > 0.05f) {
                        g_app.visualBalance += (balTarget - g_app.visualBalance) * 0.34f;
                        keepRunning = true;
                    } else {
                        g_app.visualBalance = balTarget;
                    }

                    float thumbTarget = (g_app.isDraggingSlider || g_app.sliderHovered) ? 1.0f : 0.0f;
                    if (std::abs(thumbTarget - g_app.thumbHoverAnim) > 0.01f) {
                        g_app.thumbHoverAnim += (thumbTarget - g_app.thumbHoverAnim) * 0.28f;
                        keepRunning = true;
                    } else {
                        g_app.thumbHoverAnim = thumbTarget;
                    }

                    float testTarget = g_app.testHovered ? 1.0f : 0.0f;
                    if (std::abs(testTarget - g_app.testHoverAnim) > 0.01f) {
                        g_app.testHoverAnim += (testTarget - g_app.testHoverAnim) * 0.28f;
                        keepRunning = true;
                    } else {
                        g_app.testHoverAnim = testTarget;
                    }

                    float bypassTarget = g_app.bypassed ? 1.0f : 0.0f;
                    if (std::abs(bypassTarget - g_app.bypassVisual) > 0.01f) {
                        g_app.bypassVisual += (bypassTarget - g_app.bypassVisual) * 0.22f;
                        keepRunning = true;
                    } else {
                        g_app.bypassVisual = bypassTarget;
                    }

                    if (g_app.hpAnimating) {
                        float t = std::clamp(static_cast<float>(GetTickCount64() - g_app.hpAnimStartTick) / 360.0f, 0.0f, 1.0f);
                        float u = 1.0f - t;
                        float easeOut = 1.0f - u * u * u;
                        g_app.slashProgress = g_app.bypassed ? easeOut : (1.0f - easeOut);
                        if (t >= 1.0f) {
                            g_app.slashProgress = g_app.bypassed ? 1.0f : 0.0f;
                            g_app.hpAnimating = false;
                        } else {
                            keepRunning = true;
                        }
                    }

                    // Smooth audio visualizer attack/decay
                    if (g_app.vizMode != VIZ_OFF && g_app.vizRunning.load()) {
                        auto smoothStep = [](float cur, float tgt) {
                            float k = (tgt > cur) ? 0.45f : 0.14f;
                            return cur + (tgt - cur) * k;
                        };
                        g_app.vizLeft = smoothStep(g_app.vizLeft, g_app.vizTargetLeft);
                        g_app.vizRight = smoothStep(g_app.vizRight, g_app.vizTargetRight);
                        g_app.vizOverall = smoothStep(g_app.vizOverall, g_app.vizTargetOverall);
                        for (size_t b = 0; b < VIZ_BANDS; ++b) {
                            g_app.vizBands[b] = smoothStep(g_app.vizBands[b], g_app.vizTargetBands[b]);
                        }
                        keepRunning = true;
                    }
                }

                if (g_app.testPlaying.load() && IsWindowVisible(hwnd)) {
                    keepRunning = true;
                }

                if (!keepRunning) {
                    KillTimer(hwnd, TIMER_UI_ANIM);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (wParam == TIMER_HOVER_TIP) {
                KillTimer(hwnd, TIMER_HOVER_TIP);
                if (g_app.hpHovered) {
                    ShowHoverTip();
                }
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (IsPointInHeadphoneIcon(hwnd, pt.x, pt.y) ||
                    IsPointInTestButton(hwnd, pt.x, pt.y) ||
                    IsPointInLeftLabel(hwnd, pt.x, pt.y) ||
                    IsPointInRightLabel(hwnd, pt.x, pt.y) ||
                    IsPointInSliderTrack(hwnd, pt.x, pt.y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN: {
            HideHoverTip();
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            if (IsPointInHeadphoneIcon(hwnd, mx, my)) {
                ToggleBypass(hwnd);
                return 0;
            }
            if (IsPointInTestButton(hwnd, mx, my)) {
                if (g_app.testPlaying.load()) StopTestTone();
                else StartTestTone();
                return 0;
            }
            if (IsPointInLeftLabel(hwnd, mx, my)) {
                g_app.soloSide = -1;
                SetCapture(hwnd);
                ApplyBalanceToAllSessions(-100);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (IsPointInRightLabel(hwnd, mx, my)) {
                g_app.soloSide = 1;
                SetCapture(hwnd);
                ApplyBalanceToAllSessions(100);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (IsPointInSliderTrack(hwnd, mx, my)) {
                g_app.isDraggingSlider = true;
                SetCapture(hwnd);
                EnsureUiAnimTimer();
                UpdateBalanceFromMouseX(hwnd, mx);
                return 0;
            }

            float s = GetScaleForWindow(hwnd);
            if (my < static_cast<int>(std::lround((TRACK_Y - 12) * s))) {
                g_app.isMovingWindow = true;
                GetCursorPos(&g_app.moveStartCursor);
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                g_app.moveStartWinPos = { wr.left, wr.top };
                SetCapture(hwnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);

            if (!g_app.trackingMouseLeave) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                g_app.trackingMouseLeave = true;
            }

            if (g_app.isDraggingSlider) {
                UpdateBalanceFromMouseX(hwnd, mx);
            } else if (g_app.isMovingWindow) {
                HideHoverTip();
                POINT cur{};
                GetCursorPos(&cur);
                int nx = g_app.moveStartWinPos.x + (cur.x - g_app.moveStartCursor.x);
                int ny = g_app.moveStartWinPos.y + (cur.y - g_app.moveStartCursor.y);
                SetWindowPos(hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                bool overHp = IsPointInHeadphoneIcon(hwnd, mx, my);
                if (overHp && !g_app.hpHovered) {
                    g_app.hpHovered = true;
                    SetTimer(hwnd, TIMER_HOVER_TIP, 350, nullptr);
                } else if (!overHp && g_app.hpHovered) {
                    g_app.hpHovered = false;
                    HideHoverTip();
                }

                bool overSlider = IsPointInSliderTrack(hwnd, mx, my);
                bool overTest = IsPointInTestButton(hwnd, mx, my);
                if (overSlider != g_app.sliderHovered || overTest != g_app.testHovered) {
                    g_app.sliderHovered = overSlider;
                    g_app.testHovered = overTest;
                    EnsureUiAnimTimer();
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            g_app.trackingMouseLeave = false;
            g_app.hpHovered = false;
            if (g_app.sliderHovered || g_app.testHovered) {
                g_app.sliderHovered = false;
                g_app.testHovered = false;
                EnsureUiAnimTimer();
            }
            HideHoverTip();
            return 0;

        case WM_LBUTTONUP:
            if (g_app.soloSide != 0) {
                g_app.soloSide = 0;
                ReleaseCapture();
                ApplyBalanceToAllSessions(GetEffectiveBalance());
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (g_app.isDraggingSlider) {
                g_app.isDraggingSlider = false;
                ReleaseCapture();
                UpdateBalanceFromMouseX(hwnd, GET_X_LPARAM(lParam));
                SaveSettings();
                EnsureUiAnimTimer();
            } else if (g_app.isMovingWindow) {
                g_app.isMovingWindow = false;
                ReleaseCapture();
            }
            return 0;

        case WM_LBUTTONDBLCLK: {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            if (IsPointInLeftLabel(hwnd, mx, my)) {
                g_app.soloSide = -1;
                SetCapture(hwnd);
                ApplyBalanceToAllSessions(-100);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (IsPointInRightLabel(hwnd, mx, my)) {
                g_app.soloSide = 1;
                SetCapture(hwnd);
                ApplyBalanceToAllSessions(100);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (IsPointInSliderTrack(hwnd, mx, my)) {
                ResetBalanceToCenter();
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            int step = (delta > 0) ? 1 : -1;
            int cur = 0;
            {
                std::lock_guard<std::mutex> lock(g_app.stateMutex);
                cur = g_app.balance;
            }
            SetBalanceValue(hwnd, cur + step, true, false);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                HideFlyout();
                return 0;
            }
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                int cur = 0;
                {
                    std::lock_guard<std::mutex> lock(g_app.stateMutex);
                    cur = g_app.balance;
                }
                int step = (wParam == VK_RIGHT) ? 1 : -1;
                SetBalanceValue(hwnd, cur + step, true, false);
                return 0;
            }
            if (wParam == VK_HOME || wParam == '0') {
                ResetBalanceToCenter();
                return 0;
            }
            break;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && !g_app.isDraggingSlider && !g_app.isMovingWindow && g_app.soloSide == 0) {
                HideFlyout();
                return 0;
            }
            break;

        case WM_DPICHANGED:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON: {
            UINT ev = LOWORD(lParam);
            if (ev == NIN_SELECT || ev == NIN_KEYSELECT) {
                if (IsWindowVisible(g_app.flyoutHwnd) && !g_app.flyClosing) {
                    HideFlyout();
                } else if (GetTickCount64() - g_app.lastHideTick > ACTIVATE_GUARD_MS) {
                    ShowFlyoutAtTray();
                }
                return 0;
            }
            if (ev == WM_CONTEXTMENU) {
                ShowTrayContextMenu();
                return 0;
            }
            break;
        }

        case WM_AUDIO_DEVICE_CHANGED:
            StopTestTone();
            StopVizCapture();
            SyncActiveDeviceAndApplySaved();
            if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd)) {
                StartVizCapture();
                InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
            }
            return 0;

        case WM_AUDIO_SESSION_CREATED:
            ApplyBalanceToAllSessions(GetEffectiveBalance());
            return 0;

        case WM_SETTINGCHANGE:
            ApplyDwmThemeToWindow(g_app.flyoutHwnd);
            ApplyDwmThemeToWindow(g_app.tipHwnd);
            ApplyDwmThemeToWindow(g_app.noticeHwnd);
            if (g_app.flyoutHwnd && IsWindowVisible(g_app.flyoutHwnd)) InvalidateRect(g_app.flyoutHwnd, nullptr, FALSE);
            if (g_app.noticeHwnd && IsWindowVisible(g_app.noticeHwnd)) InvalidateRect(g_app.noticeHwnd, nullptr, FALSE);
            return 0;

        case WM_SHOW_EXISTING_FLYOUT:
            ShowFlyoutAtTray();
            return 0;

        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
            StopTestTone();
            StopVizCapture();
            SaveSettings();
            ApplyBalanceToAllSessions(0);
            return TRUE;

        case WM_CLOSE:
        case WM_DESTROY:
            StopTestTone();
            StopVizCapture();
            SaveSettings();
            ApplyBalanceToAllSessions(0);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    HANDLE singleMutex = CreateMutexW(nullptr, TRUE, L"Local\\SoundBalance_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kHostClass, kAppName);
        if (existing) {
            PostMessageW(existing, WM_SHOW_EXISTING_FLYOUT, 0, 0);
        }
        if (singleMutex) CloseHandle(singleMutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&g_app.gdiplusToken, &gdiInput, nullptr);

    g_app.hInst = hInstance;
    LoadSettings();

    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));

    WNDCLASSEXW hostWc{};
    hostWc.cbSize = sizeof(hostWc);
    hostWc.lpfnWndProc = HostWndProc;
    hostWc.hInstance = hInstance;
    hostWc.hIcon = appIcon;
    hostWc.lpszClassName = kHostClass;
    RegisterClassExW(&hostWc);

    WNDCLASSEXW flyWc{};
    flyWc.cbSize = sizeof(flyWc);
    flyWc.style = CS_DBLCLKS;
    flyWc.lpfnWndProc = FlyoutWndProc;
    flyWc.hInstance = hInstance;
    flyWc.hIcon = appIcon;
    flyWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    flyWc.lpszClassName = kFlyoutClass;
    RegisterClassExW(&flyWc);

    WNDCLASSEXW tipWc{};
    tipWc.cbSize = sizeof(tipWc);
    tipWc.lpfnWndProc = TipWndProc;
    tipWc.hInstance = hInstance;
    tipWc.lpszClassName = kTipClass;
    RegisterClassExW(&tipWc);

    WNDCLASSEXW noticeWc{};
    noticeWc.cbSize = sizeof(noticeWc);
    noticeWc.lpfnWndProc = NoticeWndProc;
    noticeWc.hInstance = hInstance;
    noticeWc.hIcon = appIcon;
    noticeWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    noticeWc.lpszClassName = kNoticeClass;
    RegisterClassExW(&noticeWc);

    g_app.hostHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    g_app.flyoutHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kFlyoutClass, L"", WS_POPUP,
        0, 0, FLYOUT_W, FLYOUT_H,
        nullptr, nullptr, hInstance, nullptr);
    g_app.tipHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kTipClass, L"", WS_POPUP,
        0, 0, 58, 24,
        nullptr, nullptr, hInstance, nullptr);
    g_app.noticeHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kNoticeClass, L"Sound Balance", WS_POPUP,
        0, 0, NOTICE_W, NOTICE_H,
        nullptr, nullptr, hInstance, nullptr);

    DWORD cornerPref = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_app.flyoutHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(g_app.tipHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    DwmSetWindowAttribute(g_app.noticeHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    ApplyDwmThemeToWindow(g_app.flyoutHwnd);
    ApplyDwmThemeToWindow(g_app.tipHwnd);
    ApplyDwmThemeToWindow(g_app.noticeHwnd);

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&g_app.enumerator)))) {
        g_app.notifyClient = new DeviceNotificationClient(g_app.hostHwnd);
        g_app.enumerator->RegisterEndpointNotificationCallback(g_app.notifyClient);
        SyncActiveDeviceAndApplySaved();
    }

    g_app.trayIcon = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!g_app.trayIcon) {
        g_app.trayIcon = CreateAppTrayIcon();
    }
    g_app.trayIconBypassed = CreateBypassedTrayIcon(g_app.trayIcon);

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app.hostHwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_app.trayIcon;
    wcscpy_s(nid.szTip, L"Sound Balance");
    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    bool isAutoStart = (lpCmdLine && wcsstr(lpCmdLine, L"--startup") != nullptr);
    if (!isAutoStart) {
        ShowNoticePopup();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopTestTone();
    StopVizCapture();
    ApplyBalanceToAllSessions(0);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_app.trayIcon) DestroyIcon(g_app.trayIcon);
    if (g_app.trayIconBypassed) DestroyIcon(g_app.trayIconBypassed);
    ReleaseSessionManager();
    if (g_app.enumerator) {
        if (g_app.notifyClient) {
            g_app.enumerator->UnregisterEndpointNotificationCallback(g_app.notifyClient);
            g_app.notifyClient->Release();
        }
        g_app.enumerator->Release();
    }
    Gdiplus::GdiplusShutdown(g_app.gdiplusToken);
    CoUninitialize();
    if (singleMutex) CloseHandle(singleMutex);
    return 0;
}
