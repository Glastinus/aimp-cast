// AIMP Cast engine: Winamp DSP module (x64) that streams AIMP's output to a Google Cast device,
// plus the engine API used by the AIMP plugin part (aimp_plugin.cpp) of the same DLL.
#include "cast.h"
#include "stream.h"
#include "engine_api.h"
#include "lang.h"
#include <shlobj.h>
#include <shellapi.h>
#include <deque>
#include <functional>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------- Winamp DSP ABI

struct winampDSPModule {
    char* description;
    HWND hwndParent;
    HINSTANCE hDllInstance;
    void (*Config)(winampDSPModule* this_mod);
    int (*Init)(winampDSPModule* this_mod);
    int (*ModifySamples)(winampDSPModule* this_mod, short int* samples, int numsamples, int bps, int nch, int srate);
    void (*Quit)(winampDSPModule* this_mod);
    void* userData;
};

struct winampDSPHeader {
    int version;
    char* description;
    winampDSPModule* (*getModule)(int);
    int (*sf)(int);
};

// ---------------------------------------------------------------- settings / log

static const uint16_t kPort = 45710;
static std::wstring g_dir;  // %APPDATA%\AIMP\aimp_cast
static std::mutex g_logMu;

static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static void FileLog(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMu);
    FILE* f = _wfopen((g_dir + L"\\aimp_cast.log").c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s.c_str());
    fclose(f);
}

static std::wstring IniPath() { return g_dir + L"\\aimp_cast.ini"; }
static std::string IniGet(const wchar_t* key, const char* def) {
    wchar_t buf[512];
    GetPrivateProfileStringW(L"Cast", key, Widen(def).c_str(), buf, 512, IniPath().c_str());
    return Narrow(buf);
}
static void IniSet(const wchar_t* key, const std::string& v) {
    WritePrivateProfileStringW(L"Cast", key, Widen(v).c_str(), IniPath().c_str());
}

// ---------------------------------------------------------------- controller

class Controller {
public:
    void Start() {
        running_ = true;
        worker_ = std::thread([this] { Run(); });
        server_.onFormatChanged = [this](int) { Post([this] { if (casting_) Load(); }); };
        EnsureServer();
        muteLocal_ = IniGet(L"MuteLocal", "1") == "1";
        server_.startBackMs = atoi(IniGet(L"StartBufferMs", "4000").c_str());
        discThr_ = std::thread([this] { DiscoveryLoop(); });
    }
    bool EnsureServer() {  // a busy port (e.g. a second engine copy) must not break casting
        if (server_.Port()) return true;
        for (uint16_t p = kPort; p < kPort + 10; p++)
            if (server_.Start(p)) return true;
        return false;
    }
    void Rescan() {
        { std::lock_guard<std::mutex> lk(dmu_); rescan_ = true; }
        dcv_.notify_all();
    }
    std::vector<CastDevice> Devices() {
        std::lock_guard<std::mutex> lk(dmu_);
        auto v = devices_;
        if (casting_) {  // keep the active device listed even if it missed the last scan
            std::lock_guard<std::mutex> lk2(stMu_);
            bool found = false;
            for (auto& d : v) if (d.id == current_.id) found = true;
            if (!found && !current_.id.empty()) v.push_back(current_);
        }
        return v;
    }
    bool CastToId(const std::string& id) {
        for (auto& d : Devices())
            if (d.id == id) { CastTo(d); return true; }
        return false;
    }
    void Stop() {
        { std::lock_guard<std::mutex> lk(dmu_); discRunning_ = false; }
        dcv_.notify_all();
        if (discThr_.joinable()) discThr_.join();
        Post([this] { StopCastNow(); });
        {
            std::lock_guard<std::mutex> lk(qmu_);
            running_ = false;
        }
        qcv_.notify_all();
        if (worker_.joinable()) worker_.join();
        server_.Stop();
    }
    void CastTo(const CastDevice& d) { Post([this, d] { StopCastNow(); StartCastNow(d); }); }
    void StopCast() { Post([this] { StopCastNow(); }); }
    // AIMP's volume slider (0..1). Applied after the DSP, so it must be mirrored on the speaker.
    void SetVolume(float v) {
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        volume_ = v;
        if (!volPending_.exchange(true))  // coalesce slider drags into one request at a time
            Post([this] { volPending_ = false; ApplyVolume(); });
    }

    AudioStreamServer& Server() { return server_; }
    bool Casting() const { return casting_; }
    // Hidden until our own initial SET_VOLUME has settled, so a stale level never reaches AIMP.
    float SpeakerVolume() const {
        return casting_ && GetTickCount64() >= volReadyAt_ ? client_.SpeakerVolume() : -1.0f;
    }
    std::string CurrentName() { std::lock_guard<std::mutex> lk(stMu_); return current_.name; }
    std::string CurrentId() { std::lock_guard<std::mutex> lk(stMu_); return current_.id; }
    std::atomic<bool> muteLocal_{true};

private:
    void DiscoveryLoop() {
        std::unique_lock<std::mutex> lk(dmu_);
        while (discRunning_) {
            lk.unlock();
            auto found = DiscoverCastDevices(2000);
            lk.lock();
            devices_ = found;
            rescan_ = false;
            dcv_.wait_for(lk, std::chrono::seconds(60), [&] { return rescan_ || !discRunning_; });
        }
    }
    void Post(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(qmu_);
        q_.push_back(std::move(f));
        qcv_.notify_all();
    }
    void Run() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                qcv_.wait_for(lk, std::chrono::seconds(3), [&] { return !q_.empty() || !running_; });
                if (!q_.empty()) { f = std::move(q_.front()); q_.pop_front(); }
                else if (!running_) return;
            }
            if (f) f();
            else Watchdog();
        }
    }
    void StartCastNow(const CastDevice& d) {
        { std::lock_guard<std::mutex> lk(stMu_); current_ = d; }
        CastLog("ctl: casting to %s (%s:%u)", d.name.c_str(), d.ip.c_str(), d.port);
        if (!EnsureServer()) { CastLog("ctl: no http port available, not casting"); return; }
        if (!client_.Connect(d.ip, d.port)) { CastLog("ctl: connect failed"); return; }
        url_ = "http://" + LocalIpFor(d.ip) + ":" + std::to_string(server_.Port()) + "/aimp.wav";
        casting_ = true;
        IniSet(L"LastDevice", d.id);
        volReadyAt_ = ~0ull;
        Load();
        ApplyVolume();
        volReadyAt_ = GetTickCount64() + 2000;
    }
    void ApplyVolume() {
        float v = volume_;
        if (!casting_ || v < 0) return;
        client_.SetVolume(v);
        CastLog("ctl: volume %.2f", v);
    }
    std::atomic<float> volume_{-1.0f};
    std::atomic<ULONGLONG> volReadyAt_{0};
    std::atomic<bool> volPending_{false};
    void Load() {
        bool ok = client_.LaunchAndLoad(url_, "audio/wav", "AIMP", true);
        CastLog("ctl: load %s -> %s", url_.c_str(), ok ? "OK" : "FAILED");
    }
    void StopCastNow() {
        if (!casting_) return;
        casting_ = false;
        client_.StopApp();
        Sleep(200);
        client_.Close();
        { std::lock_guard<std::mutex> lk(stMu_); current_ = CastDevice(); }
        CastLog("ctl: stopped");
    }
    void Watchdog() {
        // Someone stopped the app on the speaker (or connection dropped): end casting so local audio returns.
        if (casting_ && (!client_.IsConnected() || !client_.AppRunning()) && server_.ClientCount() == 0) {
            CastLog("ctl: receiver gone, stopping cast");
            StopCastNow();
        }
    }

    AudioStreamServer server_;
    CastClient client_;
    std::atomic<bool> casting_{false};
    std::mutex stMu_;
    CastDevice current_;
    std::string url_;
    std::thread worker_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::function<void()>> q_;
    bool running_ = false;
    std::thread discThr_;
    std::mutex dmu_;
    std::condition_variable dcv_;
    std::vector<CastDevice> devices_;
    bool rescan_ = false, discRunning_ = true;
};

static Controller* g_ctl = nullptr;
static std::mutex g_refMu;
static int g_refs = 0;
static std::atomic<bool> g_dspActive{false};

static void Acquire() {
    std::lock_guard<std::mutex> lk(g_refMu);
    if (g_refs++ == 0) {
        g_ctl = new Controller();
        g_ctl->Start();
        CastLog("engine: started");
    }
}
static void Release() {
    std::lock_guard<std::mutex> lk(g_refMu);
    if (g_refs > 0 && --g_refs == 0) {
        CastLog("engine: stopping");
        g_ctl->Stop();
        delete g_ctl;
        g_ctl = nullptr;
    }
}

// ---------------------------------------------------------------- UI

static void ShowMenu(HWND parent) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    auto devs = DiscoverCastDevices(2000);
    SetCursor(old);

    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, parent, nullptr, nullptr, nullptr);
    HMENU m = CreatePopupMenu();
    std::wstring status = g_ctl->Casting() ? std::wstring(Tr(L"Casting to: ", L"Caster til: ")) + Widen(g_ctl->CurrentName()) : Tr(L"AIMP Cast: not active", L"AIMP Cast: ikke aktiv");
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::string cur = g_ctl->CurrentId();
    if (devs.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, Tr(L"(no Cast devices found)", L"(fant ingen Cast-enheter)"));
    for (size_t i = 0; i < devs.size(); i++) {
        std::wstring label = Widen(devs[i].name);
        if (!devs[i].model.empty()) label += L"  (" + Widen(devs[i].model) + L")";
        UINT fl = MF_STRING | (g_ctl->Casting() && devs[i].id == cur ? MF_CHECKED : 0);
        AppendMenuW(m, fl, 100 + (UINT)i, label.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_ctl->Casting() ? 0 : MF_GRAYED), 1, Tr(L"Stop casting", L"Stopp casting"));
    AppendMenuW(m, MF_STRING | (g_ctl->muteLocal_ ? MF_CHECKED : 0), 2, Tr(L"Mute local audio while casting", L"Demp lokal lyd under casting"));
    AppendMenuW(m, MF_STRING, 3, Tr(L"Open log file", L"Åpne loggfil"));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(w);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, nullptr);
    DestroyMenu(m);
    DestroyWindow(w);

    if (cmd >= 100 && cmd - 100 < devs.size()) g_ctl->CastTo(devs[cmd - 100]);
    else if (cmd == 1) g_ctl->StopCast();
    else if (cmd == 2) {
        g_ctl->muteLocal_ = !g_ctl->muteLocal_;
        IniSet(L"MuteLocal", g_ctl->muteLocal_ ? "1" : "0");
    } else if (cmd == 3) ShellExecuteW(nullptr, L"open", (g_dir + L"\\aimp_cast.log").c_str(), nullptr, nullptr, SW_SHOW);
}

// ---------------------------------------------------------------- DSP callbacks

static void DspConfig(winampDSPModule* mod) { if (g_ctl) ShowMenu(mod->hwndParent); }

static int DspInit(winampDSPModule*) {
    if (!g_dspActive.exchange(true)) {
        CastLog("dsp: init");
        Acquire();
    }
    return 0;
}

static int DspModify(winampDSPModule*, short int* samples, int numsamples, int bps, int nch, int srate) {
    if (!g_ctl) return numsamples;
    g_ctl->Server().Push(samples, numsamples, bps, nch, srate);
    if (g_ctl->muteLocal_ && g_ctl->Casting() && g_ctl->Server().ClientCount() > 0)
        memset(samples, 0, (size_t)numsamples * nch * (bps / 8));
    return numsamples;
}

static void DspQuit(winampDSPModule*) {
    if (g_dspActive.exchange(false)) {
        CastLog("dsp: quit");
        Release();
    }
}

// ---------------------------------------------------------------- engine API (engine_api.h)

static void CopyStr(char* dst, size_t n, const std::string& s) {
    strncpy_s(dst, n, s.c_str(), _TRUNCATE);
}

extern "C" int AimpCast_ApiVersion() { return 1; }
extern "C" void AimpCast_Attach() { Acquire(); }
extern "C" void AimpCast_Detach() { Release(); }
extern "C" void AimpCast_Rescan() { if (g_ctl) g_ctl->Rescan(); }
extern "C" void AimpCast_StopCast() { if (g_ctl) g_ctl->StopCast(); }
extern "C" void AimpCast_SetVolume(float level) { if (g_ctl) g_ctl->SetVolume(level); }
// Speaker's own volume (changed in Google Home / on the device), -1 when not casting or unknown.
extern "C" float AimpCast_GetSpeakerVolume() { return g_ctl ? g_ctl->SpeakerVolume() : -1.0f; }
extern "C" int AimpCast_MuteLocal(int set) {
    if (!g_ctl) return 0;
    if (set == 0 || set == 1) {
        g_ctl->muteLocal_ = set == 1;
        IniSet(L"MuteLocal", set ? "1" : "0");
    }
    return g_ctl->muteLocal_ ? 1 : 0;
}
extern "C" int AimpCast_CastTo(const char* id) {
    return g_ctl && g_ctl->CastToId(id) ? 0 : -1;
}
extern "C" int AimpCast_GetDevices(AimpCastDevice* out, int max) {
    if (!g_ctl) return 0;
    auto v = g_ctl->Devices();
    int n = 0;
    for (auto& d : v) {
        if (n >= max) break;
        CopyStr(out[n].id, sizeof out[n].id, d.id);
        CopyStr(out[n].name, sizeof out[n].name, d.name);
        CopyStr(out[n].model, sizeof out[n].model, d.model);
        n++;
    }
    return n;
}
// bit 0: casting, bit 1: DSP receives audio from AIMP
extern "C" int AimpCast_GetState(char* curId, int n) {
    int st = g_dspActive ? 2 : 0;
    if (g_ctl && g_ctl->Casting()) {
        st |= 1;
        if (curId && n > 0) CopyStr(curId, (size_t)n, g_ctl->CurrentId());
    } else if (curId && n > 0) curId[0] = 0;
    return st;
}

static char g_desc[] = "AIMP Cast (Google Cast) v0.2";
static char g_modDesc[] = "AIMP Cast - stream to Google Cast";
static winampDSPModule g_mod = {g_modDesc, nullptr, nullptr, DspConfig, DspInit, DspModify, DspQuit, nullptr};

static winampDSPModule* GetModule(int i) { return i == 0 ? &g_mod : nullptr; }
static winampDSPHeader g_hdr = {0x20, g_desc, GetModule, nullptr};

// Winamp DSP entry point, reached through aimp_cast_dsp.dll (see dsp_forward.cpp).
extern "C" void* AimpCast_DspHeader(HWND) { return &g_hdr; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_mod.hDllInstance = inst;
        wchar_t* appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            std::wstring aimp = std::wstring(appdata) + L"\\AIMP";
            CoTaskMemFree(appdata);
            g_dir = aimp + L"\\aimp_cast";
            CreateDirectoryW(g_dir.c_str(), nullptr);
            // keep settings from the earlier two-DLL prototype
            CopyFileW((aimp + L"\\dsp_cast\\dsp_cast.ini").c_str(), IniPath().c_str(), TRUE);
        }
        g_castLog = FileLog;
    }
    return TRUE;
}
