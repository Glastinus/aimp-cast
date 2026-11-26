// Loads aimp_cast.dll like AIMP's DSP manager would and feeds it a quiet test tone in real time.
//   dsptest <path-to-aimp_cast_dsp.dll> <seconds> [rate] [bps]   or   dsptest <dll> api
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstdint>

struct winampDSPModule {
    char* description; HWND hwndParent; HINSTANCE hDllInstance;
    void (*Config)(winampDSPModule*); int (*Init)(winampDSPModule*);
    int (*ModifySamples)(winampDSPModule*, short int*, int, int, int, int);
    void (*Quit)(winampDSPModule*); void* userData;
};
struct winampDSPHeader { int version; char* description; winampDSPModule* (*getModule)(int); int (*sf)(int); };

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) { printf("usage: dsptest <dll> <seconds> [rate] [bps]\n"); return 1; }
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto get = (winampDSPHeader * (*)(HWND)) GetProcAddress(h, "winampDSPGetHeader2");
    auto* hdr = get(nullptr);
    auto* m = hdr->getModule(0);
    printf("header: %s / module: %s\n", hdr->description, m->description);
    if (!wcscmp(argv[2], L"api")) {  // exercise the menu-plugin API without audio
        struct Dev { char id[160]; char name[128]; char model[64]; } devs[8];
        if (!GetProcAddress(h, "AimpCast_Attach")) h = GetModuleHandleW(L"aimp_cast.dll");  // given the DSP forwarder
        auto attach = (void (*)())GetProcAddress(h, "AimpCast_Attach");
        auto detach = (void (*)())GetProcAddress(h, "AimpCast_Detach");
        auto get = (int (*)(Dev*, int))GetProcAddress(h, "AimpCast_GetDevices");
        auto st = (int (*)(char*, int))GetProcAddress(h, "AimpCast_GetState");
        attach();
        printf("state before dsp init: %d\n", st(nullptr, 0));
        m->Init(m);
        printf("state after dsp init: %d\n", st(nullptr, 0));
        Sleep(3000);
        int n = get(devs, 8);
        for (int i = 0; i < n; i++) printf("dev: %s | %s | %s\n", devs[i].name, devs[i].model, devs[i].id);
        m->Quit(m);
        printf("state after dsp quit: %d\n", st(nullptr, 0));
        detach();
        FreeLibrary(h);
        printf("api ok\n");
        return 0;
    }
    m->Init(m);
    int secs = _wtoi(argv[2]);
    int rate = argc > 3 ? _wtoi(argv[3]) : 48000;
    int bps = argc > 4 ? _wtoi(argv[4]) : 16;
    const int block = rate / 50;  // 20 ms
    std::vector<uint8_t> buf((size_t)block * 2 * (bps / 8));
    double ph = 0;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int64_t done = 0; done < (int64_t)rate * secs; done += block) {
        for (int i = 0; i < block; i++) {
            // 440 Hz, -26 dBFS, gentle 2 Hz amplitude wobble
            double v = 0.05 * sin(ph) * (0.75 + 0.25 * sin(ph / 220.0));
            ph += 2 * 3.14159265358979 * 440.0 / rate;
            for (int ch = 0; ch < 2; ch++) {
                size_t o = ((size_t)i * 2 + ch) * (bps / 8);
                if (bps == 16) *(int16_t*)&buf[o] = (int16_t)(v * 32767);
                else if (bps == 32) *(int32_t*)&buf[o] = (int32_t)(v * 2147483647.0);
            }
        }
        m->ModifySamples(m, (short*)buf.data(), block, bps, 2, rate);
        for (;;) {  // pace in real time
            QueryPerformanceCounter(&t);
            double el = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
            if (el * rate >= done) break;
            Sleep(2);
        }
    }
    m->Quit(m);
    FreeLibrary(h);
    printf("done\n");
    return 0;
}
