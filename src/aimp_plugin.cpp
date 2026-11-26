// AIMP Cast - AIMP plugin part. Adds a "Play on" submenu (main menu, tray menu and the Stop
// button menu) and keeps AIMP's volume and the speaker's volume in sync.
// The audio engine (engine.cpp) lives in the same DLL. AIMP's DSP manager reaches it through
// aimp_cast_dsp.dll (dsp_forward.cpp), which loads this file, so there is one engine.
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cmath>
#include "../third_party/aimp_sdk/apiPlugin.h"
#include "../third_party/aimp_sdk/apiCore.h"
#include "../third_party/aimp_sdk/apiMenu.h"
#include "../third_party/aimp_sdk/apiActions.h"
#include "../third_party/aimp_sdk/apiMessages.h"
#include "engine_api.h"
#include "lang.h"

// ---------------------------------------------------------------- engine

static struct Engine {
    void (*Attach)() = AimpCast_Attach;
    void (*Detach)() = AimpCast_Detach;
    void (*Rescan)() = AimpCast_Rescan;
    void (*StopCast)() = AimpCast_StopCast;
    int (*CastTo)(const char*) = AimpCast_CastTo;
    int (*GetDevices)(AimpCastDevice*, int) = AimpCast_GetDevices;
    int (*GetState)(char*, int) = AimpCast_GetState;
    void (*SetVolume)(float) = AimpCast_SetVolume;
    float (*GetSpeakerVolume)() = AimpCast_GetSpeakerVolume;
    int (*MuteLocal)(int) = AimpCast_MuteLocal;
    bool attached = false;

    bool Load() { Attach(); attached = true; return true; }
    void Unload() { if (attached) { Detach(); attached = false; } }
} g_engine;

static std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// ---------------------------------------------------------------- tiny COM helpers

template <class I>
class ComObj : public I {
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == Iid()) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    virtual ~ComObj() = default;
protected:
    virtual REFIID Iid() = 0;
    LONG ref_ = 1;
};

class EventHandler : public ComObj<IAIMPActionEvent> {
public:
    explicit EventHandler(std::function<void()> f) : f_(std::move(f)) {}
    void WINAPI OnExecute(IUnknown*) override { if (f_) f_(); }
protected:
    REFIID Iid() override { return IID_IAIMPActionEvent; }
private:
    std::function<void()> f_;
};

static IAIMPCore* g_core = nullptr;

static IAIMPString* MakeString(const std::wstring& s) {
    IAIMPString* str = nullptr;
    if (SUCCEEDED(g_core->CreateObject(IID_IAIMPString, (void**)&str)))
        str->SetData((TChar*)s.c_str(), (int)s.size());
    return str;
}

static void SetStr(IAIMPMenuItem* m, int prop, const std::wstring& s) {
    IAIMPString* str = MakeString(s);
    if (str) { m->SetValueAsObject(prop, str); str->Release(); }
}

static void SetEvent(IAIMPMenuItem* m, int prop, std::function<void()> f) {
    auto* e = new EventHandler(std::move(f));
    m->SetValueAsObject(prop, e);
    e->Release();
}

static IAIMPMenuItem* NewItem(const std::wstring& id, const std::wstring& caption, IUnknown* parent) {
    IAIMPMenuItem* m = nullptr;
    if (FAILED(g_core->CreateObject(IID_IAIMPMenuItem, (void**)&m))) return nullptr;
    SetStr(m, AIMP_MENUITEM_PROPID_ID, id);
    SetStr(m, AIMP_MENUITEM_PROPID_CAPTION, caption);
    m->SetValueAsObject(AIMP_MENUITEM_PROPID_PARENT, parent);
    return m;
}

// ---------------------------------------------------------------- volume
// AIMP applies its volume slider after DSP plugins, so the stream is always full scale.
// Mirror the slider on the speaker instead.

static IAIMPServiceMessageDispatcher* g_msg = nullptr;

static bool g_applyingSpeakerVol = false;  // AIMP volume being set from the speaker: don't echo back
static ULONGLONG g_lastLocalPush = 0;
static float g_lastSpeakerVol = -1;
static UINT_PTR g_volTimer = 0;

static void PushVolume() {
    if (!g_msg || !g_engine.SetVolume || g_applyingSpeakerVol) return;
    float v = -1;
    if (SUCCEEDED(g_msg->Send(AIMP_MSG_PROPERTY_VOLUME, AIMP_MSG_PROPVALUE_GET, &v)) && v >= 0) {
        g_engine.SetVolume(v);
        g_lastLocalPush = GetTickCount64();
    }
}

// Speaker -> AIMP: runs on AIMP's main thread, so setting the property is safe here.
static void CALLBACK SpeakerVolumeTimer(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_msg || !g_engine.GetSpeakerVolume) return;
    float sv = g_engine.GetSpeakerVolume();
    if (sv < 0) { g_lastSpeakerVol = -1; return; }
    bool changed = g_lastSpeakerVol < 0 || fabsf(sv - g_lastSpeakerVol) > 0.004f;
    g_lastSpeakerVol = sv;
    // Status echoes of our own slider drags arrive late; ignore them so the two don't fight.
    if (!changed || GetTickCount64() - g_lastLocalPush < 1500) return;
    float av = -1;
    if (FAILED(g_msg->Send(AIMP_MSG_PROPERTY_VOLUME, AIMP_MSG_PROPVALUE_GET, &av)) || fabsf(av - sv) <= 0.004f) return;
    g_applyingSpeakerVol = true;
    g_msg->Send(AIMP_MSG_PROPERTY_VOLUME, AIMP_MSG_PROPVALUE_SET, &sv);
    g_applyingSpeakerVol = false;
}

class VolumeHook : public ComObj<IAIMPMessageHook> {
public:
    void WINAPI CoreMessage(LongWord msg, int p1, void*, HRESULT*) override {
        if (msg == AIMP_MSG_EVENT_PROPERTY_VALUE && p1 == AIMP_MSG_PROPERTY_VOLUME) PushVolume();
    }
protected:
    REFIID Iid() override { return IID_IAIMPMessageHook; }
};
static VolumeHook* g_volHook = nullptr;

// ---------------------------------------------------------------- menu

static const int kSlots = 8;

struct CastMenu {
    IAIMPMenuItem* root = nullptr;
    IAIMPMenuItem* local = nullptr;
    IAIMPMenuItem* slots[kSlots] = {};
    IAIMPMenuItem* hint = nullptr;
    IAIMPMenuItem* rescan = nullptr;
    IAIMPMenuItem* mute = nullptr;
    std::vector<AimpCastDevice> shown;
};
static std::vector<CastMenu*> g_menus;

static void Refresh(CastMenu* cm) {
    AimpCastDevice devs[kSlots];
    int n = g_engine.GetDevices(devs, kSlots);
    char cur[160] = {0};
    int st = g_engine.GetState(cur, sizeof cur);
    bool casting = st & 1, dsp = (st & 2) != 0;
    cm->shown.assign(devs, devs + n);

    cm->local->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, casting ? 0 : 1);
    for (int i = 0; i < kSlots; i++) {
        IAIMPMenuItem* s = cm->slots[i];
        s->BeginUpdate();
        if (i < n) {
            std::wstring cap = Widen(devs[i].name);
            if (devs[i].model[0]) cap += L"  (" + Widen(devs[i].model) + L")";
            SetStr(s, AIMP_MENUITEM_PROPID_CAPTION, cap);
            s->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 1);
            s->SetValueAsInt32(AIMP_MENUITEM_PROPID_ENABLED, dsp ? 1 : 0);
            s->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, casting && !strcmp(cur, devs[i].id) ? 1 : 0);
        } else {
            s->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 0);
        }
        s->EndUpdate();
    }
    cm->mute->SetValueAsInt32(AIMP_MENUITEM_PROPID_CHECKED, g_engine.MuteLocal(-1) ? 1 : 0);
    if (!dsp) {
        SetStr(cm->hint, AIMP_MENUITEM_PROPID_CAPTION, Tr(L"(Add aimp_cast_dsp.dll in AIMP's DSP manager)", L"(Legg til aimp_cast_dsp.dll i DSP-behandleren)"));
        cm->hint->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 1);
    } else if (n == 0) {
        SetStr(cm->hint, AIMP_MENUITEM_PROPID_CAPTION, Tr(L"(Searching for speakers ...)", L"(Søker etter høyttalere …)"));
        cm->hint->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 1);
    } else {
        cm->hint->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 0);
    }
}

static void BuildMenu(int builtInId, const std::wstring& tag) {
    IAIMPServiceMenuManager* mm = nullptr;
    if (FAILED(g_core->QueryInterface(IID_IAIMPServiceMenuManager, (void**)&mm))) return;
    IAIMPMenuItem* parent = nullptr;
    HRESULT hr = mm->GetBuiltIn(builtInId, &parent);
    mm->Release();
    if (FAILED(hr) || !parent) return;

    auto* cm = new CastMenu();
    std::wstring base = L"aimp_cast." + tag;
    cm->root = NewItem(base, Tr(L"Play on", L"Spill av på"), parent);
    parent->Release();
    if (!cm->root) { delete cm; return; }
    SetEvent(cm->root, AIMP_MENUITEM_PROPID_EVENT_ONSHOW, [cm] { Refresh(cm); });
    g_core->RegisterExtension(IID_IAIMPServiceMenuManager, cm->root);

    cm->local = NewItem(base + L".local", Tr(L"This PC (local)", L"Denne PC-en (lokalt)"), cm->root);
    cm->local->SetValueAsInt32(AIMP_MENUITEM_PROPID_STYLE, AIMP_MENUITEM_STYLE_RADIOBOX);
    SetEvent(cm->local, AIMP_MENUITEM_PROPID_EVENT, [] { g_engine.StopCast(); });
    g_core->RegisterExtension(IID_IAIMPServiceMenuManager, cm->local);

    for (int i = 0; i < kSlots; i++) {
        IAIMPMenuItem* s = NewItem(base + L".dev" + std::to_wstring(i), L" ", cm->root);
        s->SetValueAsInt32(AIMP_MENUITEM_PROPID_STYLE, AIMP_MENUITEM_STYLE_RADIOBOX);
        s->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 0);
        SetEvent(s, AIMP_MENUITEM_PROPID_EVENT, [cm, i] {
            if (i < (int)cm->shown.size()) { PushVolume(); g_engine.CastTo(cm->shown[i].id); }
        });
        g_core->RegisterExtension(IID_IAIMPServiceMenuManager, s);
        cm->slots[i] = s;
    }

    cm->hint = NewItem(base + L".hint", L"", cm->root);
    cm->hint->SetValueAsInt32(AIMP_MENUITEM_PROPID_ENABLED, 0);
    cm->hint->SetValueAsInt32(AIMP_MENUITEM_PROPID_VISIBLE, 0);
    g_core->RegisterExtension(IID_IAIMPServiceMenuManager, cm->hint);

    cm->rescan = NewItem(base + L".rescan", Tr(L"Search for speakers again", L"Søk etter høyttalere på nytt"), cm->root);
    SetEvent(cm->rescan, AIMP_MENUITEM_PROPID_EVENT, [] { g_engine.Rescan(); });
    g_core->RegisterExtension(IID_IAIMPServiceMenuManager, cm->rescan);

    cm->mute = NewItem(base + L".mute", Tr(L"Mute this PC while casting", L"Demp PC-en under casting"), cm->root);
    cm->mute->SetValueAsInt32(AIMP_MENUITEM_PROPID_STYLE, AIMP_MENUITEM_STYLE_CHECKBOX);
    SetEvent(cm->mute, AIMP_MENUITEM_PROPID_EVENT, [] { g_engine.MuteLocal(g_engine.MuteLocal(-1) ? 0 : 1); });
    g_core->RegisterExtension(IID_IAIMPServiceMenuManager, cm->mute);

    g_menus.push_back(cm);
    Refresh(cm);
}

// ---------------------------------------------------------------- plugin

class CastPlugin : public ComObj<IAIMPPlugin> {
public:
    TChar* WINAPI InfoGet(int index) override {
        switch (index) {
            case AIMP_PLUGIN_INFO_NAME: return (TChar*)L"AIMP Cast";
            case AIMP_PLUGIN_INFO_AUTHOR: return (TChar*)L"AIMP Cast contributors";
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return (TChar*)Tr(L"Play AIMP on Google Cast speakers", L"Spill av AIMP på Google Cast-høyttalere");
            default: return nullptr;
        }
    }
    LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }
    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        g_core = core;
        g_core->AddRef();
        g_engine.Load();
        BuildMenu(AIMP_MENUID_PLAYER_MAIN_FUNCTIONS, L"main");
        BuildMenu(AIMP_MENUID_PLAYER_TRAY, L"tray");
        BuildMenu(AIMP_MENUID_PLAYER_STOP_OPTIONS, L"stop");  // menu on the Stop button, next to play/pause
        if (SUCCEEDED(g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&g_msg))) {
            g_volHook = new VolumeHook();
            g_msg->Hook(g_volHook);
            PushVolume();
            g_volTimer = SetTimer(nullptr, 0, 400, SpeakerVolumeTimer);
        }
        return S_OK;
    }
    HRESULT WINAPI Finalize() override {
        for (auto* cm : g_menus) {
            IAIMPMenuItem* all[] = {cm->mute, cm->rescan, cm->hint, cm->local, cm->root};
            for (auto* s : cm->slots) if (s) { g_core->UnregisterExtension(s); s->Release(); }
            for (auto* m : all) if (m) { g_core->UnregisterExtension(m); m->Release(); }
            delete cm;
        }
        g_menus.clear();
        if (g_volTimer) { KillTimer(nullptr, g_volTimer); g_volTimer = 0; }
        if (g_msg) {
            if (g_volHook) { g_msg->Unhook(g_volHook); g_volHook->Release(); g_volHook = nullptr; }
            g_msg->Release();
            g_msg = nullptr;
        }
        g_engine.Unload();
        if (g_core) { g_core->Release(); g_core = nullptr; }
        return S_OK;
    }
    void WINAPI SystemNotification(int, IUnknown*) override {}
protected:
    REFIID Iid() override {
        static const GUID none = {};
        return none;  // IAIMPPlugin has no IID; AIMP only calls it through the header export
    }
};

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    *header = new CastPlugin();
    return S_OK;
}
