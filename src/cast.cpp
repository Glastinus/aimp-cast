#include "cast.h"
#include <map>
#include <cstdarg>
#include <cstdio>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")

static void DefaultLog(const std::string& s) { fprintf(stderr, "%s\n", s.c_str()); }
void (*g_castLog)(const std::string&) = DefaultLog;

void CastLog(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_castLog(buf);
}

void EnsureWinsock() {
    static std::once_flag f;
    std::call_once(f, [] { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); });
}

// ---------------------------------------------------------------- mDNS

static void WriteName(std::vector<uint8_t>& p, const char* name) {
    const char* s = name;
    while (*s) {
        const char* d = strchr(s, '.');
        size_t n = d ? (size_t)(d - s) : strlen(s);
        p.push_back((uint8_t)n);
        p.insert(p.end(), s, s + n);
        s += n;
        if (*s == '.') s++;
    }
    p.push_back(0);
}

static bool ReadName(const uint8_t* pkt, size_t len, size_t& off, std::string& out) {
    out.clear();
    size_t o = off;
    bool jumped = false;
    int guard = 0;
    for (;;) {
        if (o >= len || ++guard > 128) return false;
        uint8_t c = pkt[o];
        if (c == 0) { o++; break; }
        if ((c & 0xC0) == 0xC0) {
            if (o + 1 >= len) return false;
            size_t ptr = ((size_t)(c & 0x3F) << 8) | pkt[o + 1];
            if (!jumped) off = o + 2;
            jumped = true;
            o = ptr;
            continue;
        }
        o++;
        if (o + c > len) return false;
        if (!out.empty()) out += '.';
        out.append((const char*)pkt + o, c);
        o += c;
    }
    if (!jumped) off = o;
    return true;
}

static uint16_t U16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

struct MdnsState {
    std::map<std::string, std::string> srcIp;  // instance -> packet source ip
    std::map<std::string, std::pair<uint16_t, std::string>> srv;  // instance -> port,target
    std::map<std::string, std::map<std::string, std::string>> txt;
    std::map<std::string, std::string> a;  // host -> ip
    std::vector<std::string> instances;
};

static void ParseMdns(const uint8_t* p, size_t len, const std::string& from, MdnsState& st) {
    if (len < 12) return;
    int qd = U16(p + 4), an = U16(p + 6), ns = U16(p + 8), ar = U16(p + 10);
    size_t off = 12;
    std::string name;
    for (int i = 0; i < qd; i++) {
        if (!ReadName(p, len, off, name)) return;
        off += 4;
    }
    int total = an + ns + ar;
    for (int i = 0; i < total; i++) {
        if (!ReadName(p, len, off, name)) return;
        if (off + 10 > len) return;
        uint16_t type = U16(p + off);
        uint16_t rdlen = U16(p + off + 8);
        off += 10;
        if (off + rdlen > len) return;
        size_t rd = off;
        std::string lname = Lower(name);
        if (type == 12 && lname.find("_googlecast._tcp") != std::string::npos) {
            std::string inst;
            size_t o = rd;
            if (ReadName(p, len, o, inst)) {
                std::string li = Lower(inst);
                bool known = false;
                for (auto& x : st.instances) if (x == li) known = true;
                if (!known) st.instances.push_back(li);
                st.srcIp[li] = from;
            }
        } else if (type == 33 && rdlen >= 7) {
            uint16_t port = U16(p + rd + 4);
            size_t o = rd + 6;
            std::string target;
            if (ReadName(p, len, o, target)) st.srv[lname] = {port, Lower(target)};
            if (!st.srcIp.count(lname)) st.srcIp[lname] = from;
        } else if (type == 16) {
            size_t o = rd;
            while (o < rd + rdlen) {
                uint8_t l = p[o++];
                if (o + l > rd + rdlen) break;
                std::string kv((const char*)p + o, l);
                o += l;
                size_t eq = kv.find('=');
                if (eq != std::string::npos) st.txt[lname][kv.substr(0, eq)] = kv.substr(eq + 1);
            }
        } else if (type == 1 && rdlen == 4) {
            char ip[32];
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", p[rd], p[rd + 1], p[rd + 2], p[rd + 3]);
            st.a[lname] = ip;
        }
        off += rdlen;
    }
}

static std::vector<in_addr> LocalIPv4s() {
    std::vector<in_addr> out;
    char host[256];
    if (gethostname(host, sizeof host) != 0) return out;
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return out;
    for (auto* r = res; r; r = r->ai_next) out.push_back(((sockaddr_in*)r->ai_addr)->sin_addr);
    freeaddrinfo(res);
    return out;
}

std::vector<CastDevice> DiscoverCastDevices(int timeoutMs) {
    EnsureWinsock();
    std::vector<CastDevice> result;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return result;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(s, (sockaddr*)&local, sizeof local);
    int ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, sizeof ttl);

    std::vector<uint8_t> q = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    WriteName(q, "_googlecast._tcp.local");
    q.push_back(0); q.push_back(12);    // PTR
    q.push_back(0x80); q.push_back(1);  // QU + IN

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &dst.sin_addr);

    auto sendAll = [&] {
        auto ifs = LocalIPv4s();
        for (auto& ia : ifs) {
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (char*)&ia, sizeof ia);
            sendto(s, (const char*)q.data(), (int)q.size(), 0, (sockaddr*)&dst, sizeof dst);
        }
        if (ifs.empty()) sendto(s, (const char*)q.data(), (int)q.size(), 0, (sockaddr*)&dst, sizeof dst);
    };

    MdnsState st;
    auto start = std::chrono::steady_clock::now();
    bool resent = false;
    sendAll();
    for (;;) {
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (el >= timeoutMs) break;
        if (!resent && el > timeoutMs / 3) { sendAll(); resent = true; }
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv{0, 100 * 1000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        uint8_t buf[9000];
        sockaddr_in from{}; int fl = sizeof from;
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        char ip[32];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        ParseMdns(buf, (size_t)n, ip, st);
    }
    closesocket(s);

    for (auto& inst : st.instances) {
        CastDevice d;
        d.id = inst;
        d.ip = st.srcIp[inst];
        if (st.srv.count(inst)) {
            d.port = st.srv[inst].first;
            auto& tgt = st.srv[inst].second;
            if (st.a.count(tgt)) d.ip = st.a[tgt];
        }
        auto& t = st.txt[inst];
        d.name = t.count("fn") ? t["fn"] : inst.substr(0, inst.find("._googlecast"));
        d.model = t.count("md") ? t["md"] : "";
        if (!d.ip.empty()) result.push_back(d);
    }
    return result;
}

std::string LocalIpFor(const std::string& remoteIp) {
    EnsureWinsock();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in r{};
    r.sin_family = AF_INET;
    r.sin_port = htons(9);
    inet_pton(AF_INET, remoteIp.c_str(), &r.sin_addr);
    std::string out = "127.0.0.1";
    if (connect(s, (sockaddr*)&r, sizeof r) == 0) {
        sockaddr_in l{}; int ll = sizeof l;
        getsockname(s, (sockaddr*)&l, &ll);
        char ip[32];
        inet_ntop(AF_INET, &l.sin_addr, ip, sizeof ip);
        out = ip;
    }
    closesocket(s);
    return out;
}

// ---------------------------------------------------------------- TLS (Schannel)

bool TlsSocket::Connect(const std::string& ip, uint16_t port) {
    EnsureWinsock();
    Close();
    s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    if (connect(s_, (sockaddr*)&a, sizeof a) != 0) {
        CastLog("TLS: connect %s:%u failed (%d)", ip.c_str(), port, WSAGetLastError());
        Close();
        return false;
    }
    int one = 1;
    setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, (char*)&one, sizeof one);

    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
                                                   nullptr, &sc, nullptr, nullptr, &cred_, nullptr);
    if (ss != SEC_E_OK) { CastLog("TLS: AcquireCredentialsHandle 0x%08x", ss); Close(); return false; }
    haveCred_ = true;

    DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                  ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD outFlags = 0;
    SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc od{SECBUFFER_VERSION, 1, &ob};
    ss = InitializeSecurityContextW(&cred_, nullptr, (SEC_WCHAR*)L"chromecast", flags, 0, 0, nullptr, 0,
                                    &ctx_, &od, &outFlags, nullptr);
    if (ss != SEC_I_CONTINUE_NEEDED) { CastLog("TLS: ISC first 0x%08x", ss); Close(); return false; }
    haveCtx_ = true;
    if (ob.cbBuffer) { send(s_, (const char*)ob.pvBuffer, ob.cbBuffer, 0); FreeContextBuffer(ob.pvBuffer); }

    in_.clear();
    bool needData = true;
    while (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_E_INCOMPLETE_MESSAGE) {
        if (needData) {
            char tmp[16384];
            int n = recv(s_, tmp, sizeof tmp, 0);
            if (n <= 0) { CastLog("TLS: handshake recv failed"); Close(); return false; }
            in_.insert(in_.end(), tmp, tmp + n);
        }
        SecBuffer ib[2] = {{(unsigned long)in_.size(), SECBUFFER_TOKEN, in_.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc id{SECBUFFER_VERSION, 2, ib};
        ob = {0, SECBUFFER_TOKEN, nullptr};
        od = {SECBUFFER_VERSION, 1, &ob};
        ss = InitializeSecurityContextW(&cred_, &ctx_, nullptr, flags, 0, 0, &id, 0, nullptr, &od, &outFlags, nullptr);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) { needData = true; continue; }
        if (ob.cbBuffer && ob.pvBuffer) {
            send(s_, (const char*)ob.pvBuffer, ob.cbBuffer, 0);
            FreeContextBuffer(ob.pvBuffer);
        }
        if (FAILED(ss)) { CastLog("TLS: handshake 0x%08x", ss); Close(); return false; }
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer > 0) {
            std::vector<char> extra(in_.end() - ib[1].cbBuffer, in_.end());
            in_.swap(extra);
            needData = false;
        } else {
            in_.clear();
            needData = true;
        }
    }
    if (ss != SEC_E_OK) { CastLog("TLS: handshake end 0x%08x", ss); Close(); return false; }
    QueryContextAttributesW(&ctx_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
    return true;
}

bool TlsSocket::Send(const void* data, size_t len) {
    std::lock_guard<std::mutex> lk(sendMu_);
    if (s_ == INVALID_SOCKET) return false;
    const char* p = (const char*)data;
    std::vector<char> buf;
    while (len > 0) {
        size_t chunk = len < sizes_.cbMaximumMessage ? len : sizes_.cbMaximumMessage;
        buf.resize(sizes_.cbHeader + chunk + sizes_.cbTrailer);
        memcpy(buf.data() + sizes_.cbHeader, p, chunk);
        SecBuffer b[4] = {
            {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
            {(unsigned long)chunk, SECBUFFER_DATA, buf.data() + sizes_.cbHeader},
            {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes_.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&ctx_, 0, &d, 0) != SEC_E_OK) return false;
        size_t total = b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer;
        size_t sent = 0;
        while (sent < total) {
            int n = send(s_, buf.data() + sent, (int)(total - sent), 0);
            if (n <= 0) return false;
            sent += n;
        }
        p += chunk;
        len -= chunk;
    }
    return true;
}

int TlsSocket::RecvSome(char* out, size_t max) {
    while (plain_.empty()) {
        if (s_ == INVALID_SOCKET) return -1;
        if (!in_.empty()) {
            SecBuffer b[4] = {{(unsigned long)in_.size(), SECBUFFER_DATA, in_.data()},
                              {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS ss = DecryptMessage(&ctx_, &d, 0, nullptr);
            if (ss == SEC_E_OK) {
                std::vector<char> extra;
                for (auto& x : b) {
                    if (x.BufferType == SECBUFFER_DATA) plain_.insert(plain_.end(), (char*)x.pvBuffer, (char*)x.pvBuffer + x.cbBuffer);
                    if (x.BufferType == SECBUFFER_EXTRA) extra.assign((char*)x.pvBuffer, (char*)x.pvBuffer + x.cbBuffer);
                }
                in_.swap(extra);
                continue;
            }
            if (ss == SEC_I_CONTEXT_EXPIRED) return -1;
            if (ss != SEC_E_INCOMPLETE_MESSAGE) { CastLog("TLS: decrypt 0x%08x", ss); return -1; }
        }
        char tmp[16384];
        int n = recv(s_, tmp, sizeof tmp, 0);
        if (n <= 0) return -1;
        in_.insert(in_.end(), tmp, tmp + n);
    }
    size_t n = plain_.size() < max ? plain_.size() : max;
    memcpy(out, plain_.data(), n);
    plain_.erase(plain_.begin(), plain_.begin() + n);
    return (int)n;
}

bool TlsSocket::RecvAll(void* data, size_t len) {
    char* p = (char*)data;
    while (len) {
        int n = RecvSome(p, len);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

void TlsSocket::Close() {
    if (s_ != INVALID_SOCKET) { shutdown(s_, SD_BOTH); closesocket(s_); s_ = INVALID_SOCKET; }
    if (haveCtx_) { DeleteSecurityContext(&ctx_); haveCtx_ = false; }
    if (haveCred_) { FreeCredentialsHandle(&cred_); haveCred_ = false; }
    in_.clear();
    plain_.clear();
}

// ---------------------------------------------------------------- protobuf CastMessage

static void PutVarint(std::string& o, uint64_t v) {
    while (v >= 0x80) { o += (char)((v & 0x7F) | 0x80); v >>= 7; }
    o += (char)v;
}
static void PutString(std::string& o, int field, const std::string& s) {
    PutVarint(o, (uint64_t)(field << 3 | 2));
    PutVarint(o, s.size());
    o += s;
}
static bool GetVarint(const std::string& b, size_t& off, uint64_t& v) {
    v = 0;
    for (int sh = 0; off < b.size() && sh < 64; sh += 7) {
        uint8_t c = (uint8_t)b[off++];
        v |= (uint64_t)(c & 0x7F) << sh;
        if (!(c & 0x80)) return true;
    }
    return false;
}

static const char* NS_CONN = "urn:x-cast:com.google.cast.tp.connection";
static const char* NS_HB = "urn:x-cast:com.google.cast.tp.heartbeat";
static const char* NS_RECV = "urn:x-cast:com.google.cast.receiver";
static const char* NS_MEDIA = "urn:x-cast:com.google.cast.media";
static const char* APP_ID = "CC1AD845";  // Default Media Receiver

static std::string JsonStr(const std::string& json, const std::string& key, size_t from = 0) {
    std::string k = "\"" + key + "\"";
    size_t p = json.find(k, from);
    if (p == std::string::npos) return "";
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < json.size() && (json[p] == ' ')) p++;
    if (p < json.size() && json[p] == '"') {
        size_t e = json.find('"', p + 1);
        return json.substr(p + 1, e - p - 1);
    }
    size_t e = json.find_first_of(",}]", p);
    return json.substr(p, e - p);
}

static std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

void CastClient::Send(const std::string& src, const std::string& dst, const std::string& ns,
                      const std::string& payload) {
    std::string m;
    PutVarint(m, 1 << 3); PutVarint(m, 0);  // protocol_version = CASTV2_1_0
    PutString(m, 2, src);
    PutString(m, 3, dst);
    PutString(m, 4, ns);
    PutVarint(m, 5 << 3); PutVarint(m, 0);  // payload_type = STRING
    PutString(m, 6, payload);
    uint32_t len = (uint32_t)m.size();
    char hdr[4] = {(char)(len >> 24), (char)(len >> 16), (char)(len >> 8), (char)len};
    std::string frame(hdr, 4);
    frame += m;
    if (ns != NS_HB) CastLog("cast > %s %s", dst.c_str(), payload.substr(0, 400).c_str());
    if (!tls_.Send(frame.data(), frame.size())) connected_ = false;
}

bool CastClient::Connect(const std::string& ip, uint16_t port) {
    Close();
    if (!tls_.Connect(ip, port)) return false;
    connected_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        transportId_.clear(); sessionId_.clear(); mediaSessionId_ = 0; loadFailed_ = false; speakerVolume_ = -1.0f;
    }
    Send("sender-0", "receiver-0", NS_CONN, R"({"type":"CONNECT","origin":{}})");
    reader_ = std::thread(&CastClient::ReaderLoop, this);
    hb_ = std::thread(&CastClient::HeartbeatLoop, this);
    CastLog("cast: connected to %s:%u", ip.c_str(), port);
    return true;
}

void CastClient::ReaderLoop() {
    while (connected_) {
        uint8_t h[4];
        if (!tls_.RecvAll(h, 4)) break;
        uint32_t len = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        if (len > 1 << 20) break;
        std::string b(len, '\0');
        if (!tls_.RecvAll(&b[0], len)) break;
        std::string src, dst, ns, payload;
        size_t off = 0;
        while (off < b.size()) {
            uint64_t key, v;
            if (!GetVarint(b, off, key)) break;
            int field = (int)(key >> 3), wt = (int)(key & 7);
            if (wt == 0) { if (!GetVarint(b, off, v)) break; }
            else if (wt == 2) {
                if (!GetVarint(b, off, v) || off + v > b.size()) break;
                std::string s = b.substr(off, (size_t)v);
                off += (size_t)v;
                if (field == 2) src = s; else if (field == 3) dst = s;
                else if (field == 4) ns = s; else if (field == 6) payload = s;
            } else break;
        }
        Handle(src, ns, payload);
    }
    if (connected_) CastLog("cast: connection lost");
    connected_ = false;
    cv_.notify_all();
}

void CastClient::Handle(const std::string& src, const std::string& ns, const std::string& payload) {
    if (ns == NS_HB) {
        if (payload.find("\"PING\"") != std::string::npos) Send("sender-0", src, NS_HB, R"({"type":"PONG"})");
        return;
    }
    CastLog("cast < %s %s", src.c_str(), payload.substr(0, 500).c_str());
    std::lock_guard<std::mutex> lk(mu_);
    if (ns == NS_RECV && payload.find("RECEIVER_STATUS") != std::string::npos) {
        size_t vp = payload.find("\"volume\":{");
        if (vp != std::string::npos) {
            size_t lp = payload.find("\"level\":", vp);
            size_t end = payload.find('}', vp);
            if (lp != std::string::npos && lp < end) speakerVolume_ = (float)atof(payload.c_str() + lp + 8);
        }
        size_t p = payload.find(std::string("\"appId\":\"") + APP_ID + "\"");
        if (p != std::string::npos) {
            size_t objStart = payload.rfind('{', p);
            transportId_ = JsonStr(payload, "transportId", objStart);
            sessionId_ = JsonStr(payload, "sessionId", objStart);
        } else {
            transportId_.clear();
            sessionId_.clear();
        }
    } else if (ns == NS_MEDIA) {
        if (payload.find("MEDIA_STATUS") != std::string::npos) {
            std::string ms = JsonStr(payload, "mediaSessionId");
            if (!ms.empty()) mediaSessionId_ = _atoi64(ms.c_str());
            if (payload.find("\"idleReason\":\"ERROR\"") != std::string::npos) loadFailed_ = true;
        } else if (payload.find("LOAD_FAILED") != std::string::npos || payload.find("LOAD_CANCELLED") != std::string::npos ||
                   payload.find("INVALID_REQUEST") != std::string::npos) {
            loadFailed_ = true;
        }
    } else if (ns == NS_CONN && payload.find("CLOSE") != std::string::npos && src == transportId_) {
        transportId_.clear();
        sessionId_.clear();
    }
    cv_.notify_all();
}

void CastClient::HeartbeatLoop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (connected_) {
        cv_.wait_for(lk, std::chrono::seconds(5));
        if (!connected_) break;
        lk.unlock();
        Send("sender-0", "receiver-0", NS_HB, R"({"type":"PING"})");
        lk.lock();
    }
}

bool CastClient::AppRunning() {
    std::lock_guard<std::mutex> lk(mu_);
    return connected_ && !transportId_.empty();
}

bool CastClient::LaunchAndLoad(const std::string& url, const std::string& contentType,
                               const std::string& title, bool live) {
    if (!connected_) return false;
    int rid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rid = requestId_++;
        mediaSessionId_ = 0;
        loadFailed_ = false;
    }
    Send("sender-0", "receiver-0", NS_RECV,
         "{\"type\":\"LAUNCH\",\"appId\":\"" + std::string(APP_ID) + "\",\"requestId\":" + std::to_string(rid) + "}");
    std::string tid;
    {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(20), [&] { return !transportId_.empty() || !connected_; });
        tid = transportId_;
    }
    if (tid.empty()) { CastLog("cast: app did not start"); return false; }
    Send("sender-0", tid, NS_CONN, R"({"type":"CONNECT","origin":{}})");
    {
        std::lock_guard<std::mutex> lk(mu_);
        rid = requestId_++;
    }
    std::string load = "{\"type\":\"LOAD\",\"requestId\":" + std::to_string(rid) +
                       ",\"autoplay\":true,\"currentTime\":0,\"media\":{\"contentId\":\"" + JsonEscape(url) +
                       "\",\"contentType\":\"" + contentType + "\",\"streamType\":\"" + (live ? "LIVE" : "BUFFERED") +
                       "\",\"metadata\":{\"metadataType\":3,\"title\":\"" + JsonEscape(title) + "\"}}}";
    Send("sender-0", tid, NS_MEDIA, load);
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::seconds(20), [&] { return mediaSessionId_ != 0 || loadFailed_ || !connected_; });
    return mediaSessionId_ != 0 && !loadFailed_;
}

void CastClient::SetVolume(double level) {
    if (!connected_) return;
    int rid;
    { std::lock_guard<std::mutex> lk(mu_); rid = requestId_++; }
    char buf[200];
    snprintf(buf, sizeof buf, "{\"type\":\"SET_VOLUME\",\"requestId\":%d,\"volume\":{\"level\":%.3f}}", rid, level);
    Send("sender-0", "receiver-0", NS_RECV, buf);
}

void CastClient::StopApp() {
    if (!connected_) return;
    std::string sid;
    int rid;
    { std::lock_guard<std::mutex> lk(mu_); sid = sessionId_; rid = requestId_++; }
    if (sid.empty()) return;
    Send("sender-0", "receiver-0", NS_RECV,
         "{\"type\":\"STOP\",\"requestId\":" + std::to_string(rid) + ",\"sessionId\":\"" + sid + "\"}");
}

void CastClient::Close() {
    bool was = connected_.exchange(false);
    if (was) {
        // polite close of virtual connection
        std::string tid;
        { std::lock_guard<std::mutex> lk(mu_); tid = transportId_; }
        connected_ = true;
        if (!tid.empty()) Send("sender-0", tid, NS_CONN, R"({"type":"CLOSE"})");
        Send("sender-0", "receiver-0", NS_CONN, R"({"type":"CLOSE"})");
        connected_ = false;
    }
    cv_.notify_all();
    tls_.Shutdown();
    if (reader_.joinable()) reader_.join();
    if (hb_.joinable()) hb_.join();
    tls_.Close();
}
