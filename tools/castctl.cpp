// Test tool:  castctl discover
//             castctl play <ip> <port> <url> [contentType] [seconds]
#include "../src/cast.h"
#include <cstdio>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 2 && !strcmp(argv[1], "discover")) {
        auto devs = DiscoverCastDevices(3000);
        printf("%zu device(s)\n", devs.size());
        for (auto& d : devs) printf("%s:%u\t%s\t%s\n", d.ip.c_str(), d.port, d.name.c_str(), d.model.c_str());
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "play")) {
        CastClient c;
        if (!c.Connect(argv[2], (uint16_t)atoi(argv[3]))) return 1;
        const char* ct = argc >= 6 ? argv[5] : "audio/mpeg";
        int secs = argc >= 7 ? atoi(argv[6]) : 30;
        bool ok = c.LaunchAndLoad(argv[4], ct, "castctl test", true);
        printf("load: %s\n", ok ? "OK" : "FAILED");
        Sleep(secs * 1000);
        c.StopApp();
        Sleep(500);
        c.Close();
        return ok ? 0 : 2;
    }
    printf("usage: castctl discover | play <ip> <port> <url> [contentType] [seconds]\n");
    return 1;
}
