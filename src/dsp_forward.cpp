// aimp_cast_dsp.dll - the file you add in AIMP's DSP manager.
// AIMP skips the plugin part of a DLL that also exports winampDSPGetHeader2, so the Winamp DSP
// entry point lives here and forwards to Plugins\aimp_cast\aimp_cast.dll. Loading that file by
// its full path returns the module AIMP already loaded as a plugin, so there is only one engine.
#include <windows.h>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" __declspec(dllexport) void* winampDSPGetHeader2(HWND hwnd) {
    static void* (*get)(HWND) = nullptr;
    if (!get) {
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW((HMODULE)&__ImageBase, self, MAX_PATH);
        // AIMP lists Winamp DSP plugins as Plugins\<name>\<name>.dll, so this file lives in
        // Plugins\aimp_cast_dsp\ and the plugin in Plugins\aimp_cast\. Same folder also works.
        std::wstring dir = self;
        dir = dir.substr(0, dir.find_last_of(L'\\') + 1);       // ...\Plugins\aimp_cast_dsp\ (trailing slash)
        std::wstring plugins = dir.substr(0, dir.find_last_of(L'\\', dir.size() - 2) + 1);
        HMODULE h = LoadLibraryW((plugins + L"aimp_cast\\aimp_cast.dll").c_str());
        if (!h) h = LoadLibraryW((dir + L"aimp_cast.dll").c_str());  // kept loaded for the process
        if (h) get = (void* (*)(HWND))GetProcAddress(h, "AimpCast_DspHeader");
    }
    return get ? get(hwnd) : nullptr;
}
