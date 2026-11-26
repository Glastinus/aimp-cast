// Minimal Google Cast (CASTV2) sender: mDNS discovery + TLS + Default Media Receiver.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

struct CastDevice {
    std::string name;   // friendly name (fn=)
    std::string model;  // md=
    std::string id;     // instance name
    std::string ip;
    uint16_t port = 8009;
};

extern void (*g_castLog)(const std::string&);
void CastLog(const char* fmt, ...);

void EnsureWinsock();
std::vector<CastDevice> DiscoverCastDevices(int timeoutMs = 2500);
std::string LocalIpFor(const std::string& remoteIp);

class TlsSocket {
public:
    ~TlsSocket() { Close(); }
    bool Connect(const std::string& ip, uint16_t port);
    bool Send(const void* data, size_t len);
    bool RecvAll(void* data, size_t len);  // blocking, exact
    void Close();
    void Shutdown() { if (s_ != INVALID_SOCKET) shutdown(s_, SD_BOTH); }
private:
    int RecvSome(char* out, size_t max);
    SOCKET s_ = INVALID_SOCKET;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool haveCred_ = false, haveCtx_ = false;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<char> in_;     // encrypted, not yet decrypted
    std::vector<char> plain_;  // decrypted, not yet consumed
    std::mutex sendMu_;
};

class CastClient {
public:
    ~CastClient() { Close(); }
    bool Connect(const std::string& ip, uint16_t port);
    // Launches Default Media Receiver and loads url. Blocks until loaded or failed.
    bool LaunchAndLoad(const std::string& url, const std::string& contentType,
                       const std::string& title, bool live);
    void SetVolume(double level);
    void StopApp();
    void Close();
    bool IsConnected() const { return connected_; }
    bool AppRunning();
    float SpeakerVolume() const { return speakerVolume_; }  // -1 = unknown
private:
    void Send(const std::string& src, const std::string& dst, const std::string& ns,
              const std::string& payload);
    void ReaderLoop();
    void HeartbeatLoop();
    void Handle(const std::string& src, const std::string& ns, const std::string& payload);

    TlsSocket tls_;
    std::atomic<bool> connected_{false};
    std::thread reader_, hb_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::string transportId_, sessionId_;
    int64_t mediaSessionId_ = 0;
    bool loadFailed_ = false;
    int requestId_ = 1;
    std::atomic<float> speakerVolume_{-1.0f};
};
