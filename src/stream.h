// Live PCM ring buffer served as an endless 16-bit stereo WAV stream over HTTP.
#pragma once
#include <atomic>
#include "cast.h"
#include <functional>

class AudioStreamServer {
public:
    bool Start(uint16_t port);
    void Stop();
    // Called on AIMP's audio thread. Interleaved samples.
    void Push(const void* samples, int frames, int bps, int nch, int srate);
    int ClientCount() const { return clients_; }
    uint16_t Port() const { return port_; }
    int SampleRate() const { return rate_; }
    std::function<void(int newRate)> onFormatChanged;
    // History sent in one burst to a new client, so the receiver's buffer fills at once
    // instead of underrunning and re-buffering for several seconds after the first PLAYING.
    std::atomic<int> startBackMs{4000};

private:
    void AcceptLoop();
    void ClientLoop(SOCKET c);

    static const size_t kRingFrames = 48000 * 20;
    std::vector<int16_t> ring_ = std::vector<int16_t>(kRingFrames * 2);
    uint64_t writeFrame_ = 0;
    std::atomic<int> rate_{0};
    std::atomic<int> gen_{0};
    std::mutex mu_;

    SOCKET listen_ = INVALID_SOCKET;
    std::thread acceptThr_;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
    uint16_t port_ = 0;
    std::vector<int16_t> tmp_;
    bool loggedFormat_ = false;
};
