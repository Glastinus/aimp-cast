#include "stream.h"
#include <chrono>
#include <cmath>

using Clock = std::chrono::steady_clock;

bool AudioStreamServer::Start(uint16_t port) {
    EnsureWinsock();
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL excl = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&excl, sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (bind(listen_, (sockaddr*)&a, sizeof a) != 0 || listen(listen_, 8) != 0) {
        CastLog("http: cannot listen on port %u (%d)", port, WSAGetLastError());
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
        return false;
    }
    port_ = port;
    running_ = true;
    acceptThr_ = std::thread(&AudioStreamServer::AcceptLoop, this);
    CastLog("http: listening on port %u", port);
    return true;
}

void AudioStreamServer::Stop() {
    if (!running_.exchange(false)) return;
    closesocket(listen_);
    listen_ = INVALID_SOCKET;
    if (acceptThr_.joinable()) acceptThr_.join();
    for (int i = 0; i < 100 && clients_ > 0; i++) Sleep(20);
}

void AudioStreamServer::Push(const void* samples, int frames, int bps, int nch, int srate) {
    if (frames <= 0 || nch <= 0) return;
    if (!loggedFormat_) {
        loggedFormat_ = true;
        CastLog("dsp: first block frames=%d bps=%d nch=%d srate=%d", frames, bps, nch, srate);
    }
    tmp_.resize((size_t)frames * 2);
    const uint8_t* p = (const uint8_t*)samples;
    int bytes = bps / 8;
    for (int f = 0; f < frames; f++) {
        for (int ch = 0; ch < 2; ch++) {
            int src = ch < nch ? ch : 0;
            const uint8_t* s = p + ((size_t)f * nch + src) * bytes;
            int v;
            switch (bps) {
                case 8: v = ((int)s[0] - 128) << 8; break;
                case 16: v = *(const int16_t*)s; break;
                case 24: v = (int)((int8_t)s[2]) << 8 | s[1]; break;
                case 32: v = *(const int32_t*)s >> 16; break;
                default: v = 0;
            }
            tmp_[(size_t)f * 2 + ch] = (int16_t)v;
        }
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (srate != rate_) {
            if (rate_ != 0) changed = true;
            rate_ = srate;
            gen_++;
        }
        for (int f = 0; f < frames; f++) {
            size_t idx = (size_t)((writeFrame_ + f) % kRingFrames) * 2;
            ring_[idx] = tmp_[(size_t)f * 2];
            ring_[idx + 1] = tmp_[(size_t)f * 2 + 1];
        }
        writeFrame_ += frames;
    }
    if (changed) {
        CastLog("dsp: sample rate changed to %d", srate);
        if (onFormatChanged) onFormatChanged(srate);
    }
}

void AudioStreamServer::AcceptLoop() {
    while (running_) {
        sockaddr_in from{};
        int fl = sizeof from;
        SOCKET c = accept(listen_, (sockaddr*)&from, &fl);
        if (c == INVALID_SOCKET) break;
        char ip[32];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        CastLog("http: client %s", ip);
        clients_++;
        std::thread([this, c] { ClientLoop(c); closesocket(c); clients_--; }).detach();
    }
}

static void PutLE(std::string& h, uint32_t v, int n) {
    for (int i = 0; i < n; i++) h += (char)((v >> (8 * i)) & 0xFF);
}

void AudioStreamServer::ClientLoop(SOCKET c) {
    DWORD to = 3000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof to);
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) return;
        req.append(buf, n);
    }
    std::string line = req.substr(0, req.find("\r\n"));
    CastLog("http: %s", line.c_str());
    bool head = line.rfind("HEAD ", 0) == 0;
    if (line.find(" /aimp") == std::string::npos) {
        const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, r, (int)strlen(r), 0);
        return;
    }
    for (int i = 0; i < 100 && rate_ == 0; i++) Sleep(50);
    int rate = rate_ ? (int)rate_ : 44100;

    std::string h =
        "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nCache-Control: no-cache, no-store\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
    if (send(c, h.data(), (int)h.size(), 0) <= 0 || head) return;

    const uint32_t big = 0x7FFFFFF0;
    std::string w = "RIFF";
    PutLE(w, big + 36, 4);
    w += "WAVEfmt ";
    PutLE(w, 16, 4); PutLE(w, 1, 2); PutLE(w, 2, 2);
    PutLE(w, rate, 4); PutLE(w, rate * 4, 4); PutLE(w, 4, 2); PutLE(w, 16, 2);
    w += "data";
    PutLE(w, big, 4);
    if (send(c, w.data(), (int)w.size(), 0) <= 0) return;

    int myGen;
    uint64_t pos;
    {
        std::lock_guard<std::mutex> lk(mu_);
        myGen = gen_;
        uint64_t back = (uint64_t)rate * (uint64_t)startBackMs / 1000;
        pos = writeFrame_ > back ? writeFrame_ - back : 0;
    }
    std::vector<int16_t> out;
    auto lastSend = Clock::now();
    const size_t chunk = 4096;
    while (running_) {
        size_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (gen_ != myGen) { CastLog("http: format changed, closing client"); break; }
            uint64_t avail = writeFrame_ - pos;
            if (avail > kRingFrames - (uint64_t)rate) {  // client too slow: skip ahead
                pos = writeFrame_ - (uint64_t)rate / 2;
                avail = writeFrame_ - pos;
            }
            n = (size_t)(avail < chunk ? avail : chunk);
            out.resize(n * 2);
            for (size_t f = 0; f < n; f++) {
                size_t idx = (size_t)((pos + f) % kRingFrames) * 2;
                out[f * 2] = ring_[idx];
                out[f * 2 + 1] = ring_[idx + 1];
            }
            pos += n;
        }
        auto now = Clock::now();
        if (n == 0) {
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSend).count();
            if (idle < 1200) { Sleep(10); continue; }
            // AIMP paused / stopped: keep the receiver fed with silence (real-time paced).
            n = (size_t)rate * (size_t)(idle > 2000 ? 2000 : idle) / 1000;
            out.assign(n * 2, 0);
        }
        int len = (int)(out.size() * sizeof(int16_t));
        const char* d = (const char*)out.data();
        while (len > 0) {
            int s = send(c, d, len, 0);
            if (s <= 0) { CastLog("http: client disconnected"); return; }
            d += s;
            len -= s;
        }
        lastSend = Clock::now();
    }
}
