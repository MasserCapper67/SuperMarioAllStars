// Online play: the protocol and the sockets (see netplay.h).
#include "app/netplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#if !defined(__ANDROID__) || __ANDROID_API__ >= 24
#include <ifaddrs.h>
#define SMAS_HAVE_IFADDRS 1
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
#endif

namespace {

constexpr uint32_t kMagic = 0x314E4D53;  // "SMN1"
constexpr uint16_t kProtocol = 1;
constexpr int kChunkBytes = 1100;
constexpr double kTimeout = 10.0;        // seconds without a packet: gone
constexpr double kJoinTimeout = 15.0;    // seconds knocking without an answer
constexpr double kFrameSeconds = 357366.0 / 21477272.727272;

enum : uint8_t {
  kHello = 1, kWelcome, kRefuse, kBye, kInput, kChunk, kChunkAck, kSyncDone, kDesync, kPing, kPong, kNeedFull,
};

// Little-endian writer and reader.
struct W {
  std::vector<uint8_t>& v;
  void u8(uint8_t x) { v.push_back(x); }
  void u16(uint16_t x) { u8((uint8_t)x), u8((uint8_t)(x >> 8)); }
  void u32(uint32_t x) { u16((uint16_t)x), u16((uint16_t)(x >> 16)); }
  void u64(uint64_t x) { u32((uint32_t)x), u32((uint32_t)(x >> 32)); }
  void str(const std::string& s) {
    size_t n = std::min<size_t>(s.size(), 255);
    u8((uint8_t)n);
    v.insert(v.end(), s.begin(), s.begin() + n);
  }
  void bytes(const uint8_t* p, size_t n) { v.insert(v.end(), p, p + n); }
};
struct R {
  const uint8_t* p;
  const uint8_t* e;
  bool ok = true;
  bool need(size_t n) {
    if ((size_t)(e - p) < n) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? *p++ : 0; }
  uint16_t u16() {
    uint16_t a = u8();
    return (uint16_t)(a | (u8() << 8));
  }
  uint32_t u32() {
    uint32_t a = u16();
    return a | ((uint32_t)u16() << 16);
  }
  uint64_t u64() {
    uint64_t a = u32();
    return a | ((uint64_t)u32() << 32);
  }
  std::string str() {
    size_t n = u8();
    if (!need(n)) return std::string();
    std::string s((const char*)p, n);
    p += n;
    return s;
  }
  const uint8_t* bytes(size_t n) {
    if (!need(n)) return nullptr;
    const uint8_t* q = p;
    p += n;
    return q;
  }
};

void putVar(std::vector<uint8_t>& o, uint32_t v) {
  while (v >= 0x80) {
    o.push_back((uint8_t)(v | 0x80));
    v >>= 7;
  }
  o.push_back((uint8_t)v);
}
bool getVar(R& r, uint32_t* v) {
  uint32_t x = 0;
  for (int s = 0; s < 35; s += 7) {
    uint8_t b = r.u8();
    if (!r.ok) return false;
    x |= (uint32_t)(b & 0x7f) << s;
    if (!(b & 0x80)) {
      *v = x;
      return true;
    }
  }
  return false;
}

// A state as a delta to `base` (XOR, so unchanged bytes are zero), coded as
// (zero run, literal run, literal bytes) triples.
std::vector<uint8_t> encodeDelta(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& base) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() / 4);
  auto at = [&](size_t i) { return (uint8_t)(raw[i] ^ (i < base.size() ? base[i] : 0)); };
  size_t i = 0, n = raw.size();
  while (i < n) {
    size_t z = i;
    while (z < n && at(z) == 0) z++;
    size_t lit = z;
    // (a literal run ends at four zeros in a row, or the end)
    while (lit < n) {
      if (at(lit) == 0 && lit + 3 < n && at(lit + 1) == 0 && at(lit + 2) == 0 && at(lit + 3) == 0) break;
      if (at(lit) == 0 && lit + 3 >= n) {
        size_t k = lit;
        while (k < n && at(k) == 0) k++;
        if (k == n) break;
      }
      lit++;
    }
    putVar(out, (uint32_t)(z - i));
    putVar(out, (uint32_t)(lit - z));
    for (size_t k = z; k < lit; k++) out.push_back(at(k));
    i = lit;
  }
  return out;
}

bool decodeDelta(R& r, const std::vector<uint8_t>& base, size_t size, std::vector<uint8_t>& out) {
  out.assign(size, 0);
  std::memcpy(out.data(), base.data(), std::min(size, base.size()));
  size_t pos = 0;
  while (r.p < r.e) {
    uint32_t z, lit;
    if (!getVar(r, &z) || !getVar(r, &lit)) return false;
    if (pos + z + lit > size) return false;
    pos += z;
    const uint8_t* d = r.bytes(lit);
    if (!d) return false;
    for (uint32_t k = 0; k < lit; k++) out[pos + k] ^= d[k];
    pos += lit;
  }
  return true;
}

#ifdef _WIN32
struct WsaInit {
  bool ok = false;
  WsaInit() {
    WSADATA d;
    ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }
  ~WsaInit() {
    if (ok) WSACleanup();
  }
};
void closeSock(intptr_t s) { closesocket((SOCKET)s); }
#else
void closeSock(intptr_t s) { close((int)s); }
#endif

void setNonBlocking(intptr_t s) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket((SOCKET)s, FIONBIO, &on);
  // (an ICMP "port unreachable" would otherwise fail the next recvfrom)
  BOOL report = FALSE;
  DWORD got = 0;
  WSAIoctl((SOCKET)s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &got, nullptr, nullptr);
#else
  fcntl((int)s, F_SETFL, fcntl((int)s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

std::string addrText(const sockaddr* sa, socklen_t len) {
  char host[NI_MAXHOST] = "", serv[NI_MAXSERV] = "";
  if (getnameinfo(sa, len, host, sizeof host, serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) != 0) return "?";
  std::string h = host;
  if (h.rfind("::ffff:", 0) == 0) h = h.substr(7);  // (an IPv4 client of a dual-stack socket)
  if (h.find(':') != std::string::npos) h = "[" + h + "]";
  return h + ":" + serv;
}

}  // namespace

// ---------------------------------------------------------------------------

Netplay::Netplay() {
#ifdef _WIN32
  static WsaInit wsa;
#endif
}

Netplay::~Netplay() { stop(); }

double Netplay::now() const {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

uint64_t Netplay::hash(const uint8_t* d, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 1099511628211ull;
  return h;
}

bool Netplay::Addr::same(const Addr& o) const {
  const sockaddr* a = (const sockaddr*)raw;
  const sockaddr* b = (const sockaddr*)o.raw;
  if (a->sa_family != b->sa_family) return false;
  if (a->sa_family == AF_INET) {
    const sockaddr_in *x = (const sockaddr_in*)raw, *y = (const sockaddr_in*)o.raw;
    return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
  }
  if (a->sa_family == AF_INET6) {
    const sockaddr_in6 *x = (const sockaddr_in6*)raw, *y = (const sockaddr_in6*)o.raw;
    return x->sin6_port == y->sin6_port && std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof x->sin6_addr) == 0;
  }
  return len == o.len && std::memcmp(raw, o.raw, len) == 0;
}

std::string Netplay::Addr::text() const { return addrText((const sockaddr*)raw, (socklen_t)len); }

void Netplay::closeSocket() {
  if (sock_ != -1) closeSock(sock_);
  sock_ = -1;
}

bool Netplay::startHost(int port, std::string* err) {
  stop();
  if (port < 1 || port > 65535) {
    if (err) *err = "The port must be between 1 and 65535";
    return false;
  }
  // (both IPv4 and IPv6 players where the system allows it)
  intptr_t s = (intptr_t)socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  bool bound = false;
  if (s != -1) {
    int off = 0;
    setsockopt((SockT)s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof off);
    sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons((uint16_t)port);
    bound = bind((SockT)s, (sockaddr*)&a, sizeof a) == 0;
    if (bound) family_ = AF_INET6;
    else closeSock(s);
  }
  if (!bound) {
    s = (intptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == -1) {
      if (err) *err = "Couldn't open a network socket";
      return false;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind((SockT)s, (sockaddr*)&a, sizeof a) != 0) {
      closeSock(s);
      if (err) *err = "UDP port " + std::to_string(port) + " is in use (is another copy hosting?)";
      return false;
    }
    family_ = AF_INET;
  }
  setNonBlocking(s);
  sock_ = s;
  role_ = Role::Host;
  phase_ = Phase::Waiting;
  port_ = port;
  started_ = lastRecv_ = now();
  std::random_device rd;
  session_ = (rd() << 1) | 1;
  ended_ = false;
  return true;
}

bool Netplay::startJoin(const std::string& address, std::string* err) {
  stop();
  std::string a = address;
  while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
  while (!a.empty() && (a[0] == ' ' || a[0] == '\t')) a.erase(0, 1);
  std::string host = a, portText;
  if (!a.empty() && a[0] == '[') {  // [v6]:port
    size_t close = a.find(']');
    if (close == std::string::npos) {
      if (err) *err = "Missing ] in the address";
      return false;
    }
    host = a.substr(1, close - 1);
    if (close + 1 < a.size() && a[close + 1] == ':') portText = a.substr(close + 2);
  } else if (std::count(a.begin(), a.end(), ':') == 1) {
    host = a.substr(0, a.find(':'));
    portText = a.substr(a.find(':') + 1);
  }
  int port = kDefaultPort;
  if (!portText.empty()) {
    port = atoi(portText.c_str());
    if (port < 1 || port > 65535 || portText.find_first_not_of("0123456789") != std::string::npos) {
      if (err) *err = "\"" + portText + "\" isn't a port number";
      return false;
    }
  }
  if (host.empty()) {
    if (err) *err = "Enter the host's address";
    return false;
  }
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    if (err) *err = "Couldn't find \"" + host + "\" (check the address)";
    return false;
  }
  // (IPv4 first: what port forwarding is usually set up for)
  addrinfo* pick = res;
  for (addrinfo* r = res; r; r = r->ai_next)
    if (r->ai_family == AF_INET) {
      pick = r;
      break;
    }
  intptr_t s = (intptr_t)socket(pick->ai_family, SOCK_DGRAM, IPPROTO_UDP);
  if (s == -1) {
    freeaddrinfo(res);
    if (err) *err = "Couldn't open a network socket";
    return false;
  }
  setNonBlocking(s);
  sock_ = s;
  family_ = pick->ai_family;
  peer_ = Addr();
  std::memcpy(peer_.raw, pick->ai_addr, std::min<size_t>(pick->ai_addrlen, sizeof peer_.raw));
  peer_.len = (int)pick->ai_addrlen;
  freeaddrinfo(res);
  havePeer_ = true;
  peerName_ = peer_.text();
  role_ = Role::Client;
  phase_ = Phase::Waiting;
  port_ = port;
  started_ = lastRecv_ = now();
  lastHello_ = -1;
  session_ = 0;
  ended_ = false;
  return true;
}

void Netplay::stop(const std::string& reason) {
  if (role_ == Role::None) return;
  end(reason, true);
  endWhy_.clear();  // (stopped here: nothing to tell)
}

void Netplay::end(const std::string& why, bool tellPeer) {
  if (tellPeer && havePeer_ && sock_ != -1) {
    std::vector<uint8_t> p = header(kBye);
    W{p}.str(why);
    sendRaw(p);
  }
  closeSocket();
  role_ = Role::None;
  phase_ = Phase::Off;
  havePeer_ = false;
  peerName_.clear();
  rtt_ = -1;
  epoch_ = baseEpoch_ = 0;
  base_.clear();
  pending_.clear();
  out_ = Chunked();
  in_ = Chunked();
  resyncAsked_ = syncReady_ = false;
  pause_ = peerPause_ = kRunning;
  myHashes_.clear();
  hostHashes_.clear();
  desyncs_ = 0;
  ended_ = true;
  endWhy_ = why;
}

bool Netplay::takeEvent(std::string* msg) {
  if (events_.empty()) return false;
  *msg = events_.front();
  events_.pop_front();
  return true;
}

bool Netplay::takeEnded(std::string* why) {
  if (!ended_) return false;
  ended_ = false;
  *why = endWhy_;
  endWhy_.clear();
  return true;
}

double Netplay::peerSilence() const { return active() ? now() - lastRecv_ : 0; }

int Netplay::autoDelay() const {
  if (rtt_ < 0) return 2;
  int d = 1 + (int)std::ceil((rtt_ * 0.5 + 3.0) / (kFrameSeconds * 1000.0));
  return std::clamp(d, 1, kMaxDelay);
}

std::vector<uint8_t> Netplay::header(uint8_t type) const {
  std::vector<uint8_t> p;
  p.reserve(64);
  W w{p};
  w.u32(kMagic);
  w.u8(type);
  w.u32(session_);
  return p;
}

void Netplay::sendRaw(const std::vector<uint8_t>& p) {
  if (sock_ == -1 || !havePeer_) return;
  if (testDropPercent > 0 && (int)(std::random_device()() % 100) < testDropPercent) return;
  sendto((SockT)sock_, (const char*)p.data(), (int)p.size(), 0, (const sockaddr*)peer_.raw,
         (socklen_t)peer_.len);
}

// ---------------------------------------------------------------------------

void Netplay::poll() {
  if (role_ == Role::None) return;
  uint8_t buf[2048];
  for (int guard = 0; guard < 4096; guard++) {
    Addr from;
    socklen_t len = sizeof from.raw;
    int n = (int)recvfrom((SockT)sock_, (char*)buf, sizeof buf, 0, (sockaddr*)from.raw, &len);
    if (n < 0) break;
    from.len = (int)len;
    handle(buf, n, from);
    if (role_ == Role::None) return;  // (ended by what came in)
  }
  const double t = now();
  if (role_ == Role::Client && phase_ == Phase::Waiting) {
    if (t - lastHello_ > 0.25) {
      lastHello_ = t;
      std::vector<uint8_t> p = header(kHello);
      W w{p};
      w.u16(kProtocol);
      w.u32(id_.stateVersion);
      w.u32(id_.romCrc);
      w.str(id_.version);
      w.str(id_.msu);
      sendRaw(p);
    }
    if (t - started_ > kJoinTimeout) {
      end("No answer from " + peerName_ + ". Check the address, that the other player is hosting, and that UDP port " +
              std::to_string(port_) + " is forwarded to the host (or that you share a VPN).",
          false);
      return;
    }
    return;
  }
  if (!connected()) return;
  if (t - lastRecv_ > kTimeout) {
    end("Connection lost (nothing heard for " + std::to_string((int)kTimeout) + " seconds)", false);
    return;
  }
  if (t - lastPing_ > 0.5) {
    lastPing_ = t;
    std::vector<uint8_t> p = header(kPing);
    W{p}.u64((uint64_t)(t * 1e6));
    sendRaw(p);
  }
  if (role_ == Role::Host && phase_ == Phase::Syncing) sendChunks();
  if (role_ == Role::Client && phase_ == Phase::Syncing && in_.count && t - lastAck_ > 0.03) sendAck();
  if (phase_ == Phase::Playing && t - lastInputSent_ > 0.008) sendInputs();
}

void Netplay::wait(int ms) {
  if (sock_ == -1 || ms <= 0) return;
  fd_set fs;
  FD_ZERO(&fs);
  FD_SET((SockT)sock_, &fs);
  timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = ms * 1000;
  select((int)sock_ + 1, &fs, nullptr, nullptr, &tv);
  poll();
}

void Netplay::handle(const uint8_t* d, int n, const Addr& from) {
  R r{d, d + n};
  if (r.u32() != kMagic) return;
  const uint8_t type = r.u8();
  const uint32_t session = r.u32();
  if (!r.ok) return;
  const double t = now();

  if (type == kHello) {
    if (role_ != Role::Host) return;
    auto refuse = [&](const std::string& why) {
      std::vector<uint8_t> p = header(kRefuse);
      W{p}.str(why);
      sendto((SockT)sock_, (const char*)p.data(), (int)p.size(), 0, (const sockaddr*)from.raw,
             (socklen_t)from.len);
    };
    if (havePeer_) {
      if (from.same(peer_)) {  // (our welcome was lost)
        std::vector<uint8_t> p = header(kWelcome);
        W{p}.str(id_.version);
        sendRaw(p);
      } else {
        refuse("The host already has a second player");
      }
      return;
    }
    const uint16_t proto = r.u16();
    const uint32_t stateVersion = r.u32();
    const uint32_t romCrc = r.u32();
    const std::string version = r.str(), msu = r.str();
    if (!r.ok) return;
    std::string why;
    if (proto != kProtocol || version != id_.version || stateVersion != id_.stateVersion)
      why = "Different versions of the game: the host runs " + id_.version + " (online " + std::to_string(kProtocol) +
            "), you run " + version + " (online " + std::to_string(proto) + "). Both need the same build.";
    else if (romCrc != id_.romCrc)
      why = "Different ROMs: both players need the same Super Mario All-Stars ROM file";
    else if (msu != id_.msu)
      why = "Different music packs (host: " + (id_.msu.empty() ? std::string("none") : id_.msu) +
            ", you: " + (msu.empty() ? std::string("none") : msu) + "). Choose the same one, or none, on both.";
    if (!why.empty()) {
      refuse(why);
      events_.push_back("A player from " + from.text() + " couldn't join: " + why);
      return;
    }
    peer_ = from;
    havePeer_ = true;
    peerName_ = from.text();
    phase_ = Phase::Connected;
    lastRecv_ = t;
    std::vector<uint8_t> p = header(kWelcome);
    W{p}.str(id_.version);
    sendRaw(p);
    events_.push_back("Player 2 joined from " + peerName_);
    return;
  }

  if (!havePeer_ || !from.same(peer_)) return;
  if (role_ == Role::Client && phase_ == Phase::Waiting) {
    if (type == kWelcome) {
      session_ = session;
      phase_ = Phase::Connected;
      lastRecv_ = t;
      events_.push_back("Connected to " + peerName_ + " - waiting for the host's game");
    } else if (type == kRefuse) {
      end("The host refused: " + r.str(), false);
    }
    return;
  }
  if (session != session_) return;
  lastRecv_ = t;

  switch (type) {
    case kBye: {
      std::string why = r.str();
      end(why.empty() ? (role_ == Role::Host ? "Player 2 left" : "The host ended the session") : why, false);
      return;
    }
    case kPing: {
      std::vector<uint8_t> p = header(kPong);
      W{p}.u64(r.u64());
      sendRaw(p);
      return;
    }
    case kPong: {
      uint64_t sent = r.u64();
      if (!r.ok) return;
      double ms = (t - sent / 1e6) * 1000.0;
      if (ms < 0 || ms > 5000) return;
      rtt_ = rtt_ < 0 ? ms : rtt_ * 0.8 + ms * 0.2;
      return;
    }
    case kChunk: {
      if (role_ != Role::Client) return;
      const uint32_t epoch = r.u32(), total = r.u32(), index = r.u32();
      const uint16_t len = r.u16();
      const uint8_t* data = r.bytes(len);
      if (!r.ok || epoch < in_.epoch || total > (64u << 20)) return;
      if (epoch > in_.epoch) {  // (a new sync point: whatever was going on stops)
        in_ = Chunked();
        in_.epoch = epoch;
        in_.data.assign(total, 0);
        in_.count = (total + kChunkBytes - 1) / kChunkBytes;
        in_.have.assign(in_.count, 0);
        syncReady_ = false;
        phase_ = Phase::Syncing;
      }
      if (total != in_.data.size() || index >= in_.count) return;
      const size_t off = (size_t)index * kChunkBytes;
      if (len != std::min<size_t>(kChunkBytes, total - off)) return;
      if (!in_.have[index]) {
        in_.have[index] = 1;
        in_.got++;
        std::memcpy(in_.data.data() + off, data, len);
        if (in_.got == in_.count && phase_ == Phase::Syncing && !decodeSync()) {
          std::vector<uint8_t> p = header(kNeedFull);  // (our base isn't the one it was made from)
          W{p}.u32(epoch);
          sendRaw(p);
        }
      }
      if (in_.got == in_.count && phase_ == Phase::Playing && epoch_ == in_.epoch) {  // (our done went missing)
        std::vector<uint8_t> p = header(kSyncDone);
        W{p}.u32(epoch_);
        sendRaw(p);
      }
      return;
    }
    case kChunkAck: {
      if (role_ != Role::Host || phase_ != Phase::Syncing) return;
      const uint32_t epoch = r.u32(), contiguous = r.u32();
      const uint16_t bits = r.u16();
      const uint8_t* map = r.bytes((bits + 7) / 8);
      if (!r.ok || epoch != out_.epoch) return;
      for (uint32_t i = 0; i < std::min(contiguous, out_.count); i++) out_.have[i] = 1;
      for (uint32_t i = 0; i < bits; i++)
        if (contiguous + i < out_.count && (map[i / 8] >> (i % 8) & 1)) out_.have[contiguous + i] = 1;
      return;
    }
    case kNeedFull: {
      if (role_ != Role::Host || phase_ != Phase::Syncing) return;
      if (r.u32() != out_.epoch || !r.ok) return;
      base_.clear();  // (the whole state, from nothing)
      baseEpoch_ = 0;
      std::vector<uint8_t> st = pending_;
      hostSync(st, pendingSet_, delay_);
      return;
    }
    case kSyncDone:
    case kInput: {
      const uint32_t epoch = r.u32();
      if (!r.ok) return;
      if (role_ == Role::Host && phase_ == Phase::Syncing && epoch == out_.epoch) {
        // (the client has the sync point: from frame 0 together)
        base_ = pending_;
        baseEpoch_ = out_.epoch;
        startEpoch(out_.epoch, delay_);
      }
      if (type == kSyncDone || phase_ != Phase::Playing || epoch != epoch_) return;
      const uint8_t pause = r.u8();
      const uint32_t frame = r.u32(), ack = r.u32(), first = r.u32();
      const uint8_t count = r.u8();
      if (!r.ok) return;
      peerPause_ = (Pause)std::min<uint8_t>(pause, kNoGame);
      peerFrame_ = frame;
      peerFrameAt_ = t;
      if (pause_ == kRunning && peerPause_ == kRunning) {
        const double lag = rtt_ < 0 ? 0 : rtt_ * 0.0005 / kFrameSeconds;
        const double adv = (double)frame_ - ((double)frame + lag);
        advantage_ = advantage_ * 0.9 + adv * 0.1;
      }
      if (ack > peerAck_ && ack <= localNext_) peerAck_ = ack;
      for (uint32_t i = 0; i < count; i++) {
        const uint16_t pad = r.u16();
        if (!r.ok) return;
        const uint32_t f = first + i;
        if (f == remoteNext_ && f < frame_ + kRing / 2) {
          remote_[f % kRing] = pad;
          remoteNext_++;
        }
      }
      const uint8_t nh = r.u8();
      for (uint8_t i = 0; i < nh && r.ok; i++) {
        Check c;
        c.frame = r.u32();
        c.hash = r.u64();
        if (!r.ok || role_ != Role::Client) break;
        bool known = false;
        for (const Check& k : hostHashes_) known |= k.frame == c.frame;
        if (!known) hostHashes_.push_back(c);
        if (hostHashes_.size() > 16) hostHashes_.erase(hostHashes_.begin());
      }
      checkHashes();
      return;
    }
    case kDesync: {
      if (role_ != Role::Host) return;
      const uint32_t epoch = r.u32(), frame = r.u32();
      if (!r.ok || epoch != epoch_ || phase_ != Phase::Playing || resyncAsked_) return;
      resyncAsked_ = true;
      desyncs_++;
      events_.push_back("The two games drifted apart at frame " + std::to_string(frame) + " - syncing again");
      return;
    }
    default: return;
  }
}

// ---------------------------------------------------------------------------
// Sync points

void Netplay::hostSync(const std::vector<uint8_t>& state, const NetSettings& s, int delay) {
  if (role_ != Role::Host || !connected()) return;
  resyncAsked_ = false;
  pending_ = state;
  pendingSet_ = s;
  delay_ = std::clamp(delay, 1, kMaxDelay);
  const uint32_t epoch = std::max(epoch_, out_.epoch) + 1;
  epoch_ = epoch;
  std::vector<uint8_t> payload;
  W w{payload};
  w.u32(epoch);
  w.u32(baseEpoch_);
  w.u8((uint8_t)delay_);
  w.u8(s.ext);
  w.u8(s.flags);
  w.u8(s.game);
  w.u64(s.cheats);
  w.u32((uint32_t)state.size());
  w.u64(hash(state.data(), state.size()));
  std::vector<uint8_t> delta = encodeDelta(state, base_);
  payload.insert(payload.end(), delta.begin(), delta.end());
  out_ = Chunked();
  out_.epoch = epoch;
  out_.data = std::move(payload);
  out_.count = (uint32_t)((out_.data.size() + kChunkBytes - 1) / kChunkBytes);
  out_.have.assign(out_.count, 0);
  out_.sentAt.assign(out_.count, -1e9);
  phase_ = Phase::Syncing;
  sendChunks();
}

void Netplay::sendChunks() {
  const double t = now();
  const double rto = std::max(0.12, rtt_ < 0 ? 0.3 : rtt_ * 0.002 + 0.03);
  int budget = 40;  // (chunks a pass: ~2.6 MB/s at 60 passes a second at most)
  for (uint32_t i = 0; i < out_.count && budget > 0; i++) {
    if (out_.have[i] || t - out_.sentAt[i] < rto) continue;
    out_.sentAt[i] = t;
    budget--;
    const size_t off = (size_t)i * kChunkBytes;
    const size_t len = std::min<size_t>(kChunkBytes, out_.data.size() - off);
    std::vector<uint8_t> p = header(kChunk);
    W w{p};
    w.u32(out_.epoch);
    w.u32((uint32_t)out_.data.size());
    w.u32(i);
    w.u16((uint16_t)len);
    w.bytes(out_.data.data() + off, len);
    sendRaw(p);
  }
}

void Netplay::sendAck() {
  lastAck_ = now();
  uint32_t contiguous = 0;
  while (contiguous < in_.count && in_.have[contiguous]) contiguous++;
  const uint16_t bits = (uint16_t)std::min<uint32_t>(in_.count - contiguous, 1024);
  std::vector<uint8_t> map((bits + 7) / 8, 0);
  for (uint32_t i = 0; i < bits; i++)
    if (in_.have[contiguous + i]) map[i / 8] |= (uint8_t)(1 << (i % 8));
  std::vector<uint8_t> p = header(kChunkAck);
  W w{p};
  w.u32(in_.epoch);
  w.u32(contiguous);
  w.u16(bits);
  w.bytes(map.data(), map.size());
  sendRaw(p);
}

bool Netplay::decodeSync() {
  R r{in_.data.data(), in_.data.data() + in_.data.size()};
  NetSync s;
  s.epoch = r.u32();
  const uint32_t baseEpoch = r.u32();
  s.delay = r.u8();
  s.settings.ext = r.u8();
  s.settings.flags = r.u8();
  s.settings.game = r.u8();
  s.settings.cheats = r.u64();
  const uint32_t size = r.u32();
  const uint64_t h = r.u64();
  if (!r.ok || s.epoch != in_.epoch || size > (16u << 20)) return false;
  if (baseEpoch != baseEpoch_) return false;
  if (!decodeDelta(r, baseEpoch ? base_ : std::vector<uint8_t>(), size, s.state)) return false;
  if (hash(s.state.data(), s.state.size()) != h) return false;
  s.delay = std::clamp(s.delay, 1, kMaxDelay);
  delay_ = s.delay;  // (the host's: both must agree which frame each input is for)
  pending_ = s.state;
  sync_ = std::move(s);
  syncReady_ = true;
  sendAck();
  return true;
}

bool Netplay::takeSync(NetSync* out) {
  if (!syncReady_) return false;
  syncReady_ = false;
  *out = std::move(sync_);
  sync_ = NetSync();
  return true;
}

void Netplay::synced() {
  if (role_ != Role::Client) return;
  base_ = std::move(pending_);
  pending_.clear();
  baseEpoch_ = in_.epoch;
  startEpoch(in_.epoch, delay_);
  std::vector<uint8_t> p = header(kSyncDone);
  W{p}.u32(epoch_);
  sendRaw(p);
  sendInputs();
}

float Netplay::syncProgress() const {
  const Chunked& c = role_ == Role::Host ? out_ : in_;
  if (!c.count) return 0;
  if (role_ == Role::Client) return (float)c.got / c.count;
  uint32_t n = 0;
  for (uint8_t h : c.have) n += h;
  return (float)n / c.count;
}

void Netplay::startEpoch(uint32_t epoch, int delay) {
  epoch_ = epoch;
  delay_ = std::clamp(delay, 1, kMaxDelay);
  frame_ = 0;
  // (the first `delay` frames: both controllers untouched, on both sides)
  for (int i = 0; i < delay_; i++) local_[i] = remote_[i] = 0;
  localNext_ = remoteNext_ = peerAck_ = (uint32_t)delay_;
  peerFrame_ = 0;
  peerFrameAt_ = now();
  advantage_ = 0;
  adjustCooldown_ = 0;
  myHashes_.clear();
  hostHashes_.clear();
  desyncs_ = 0;
  phase_ = Phase::Playing;
}

// ---------------------------------------------------------------------------
// Lockstep

void Netplay::pushLocal(uint16_t pad) {
  if (phase_ != Phase::Playing) return;
  bool added = false;
  while (localNext_ <= frame_ + (uint32_t)delay_) {
    local_[localNext_ % kRing] = pad;
    localNext_++;
    added = true;
  }
  if (added) sendInputs();
}

bool Netplay::inputs(uint16_t* p1, uint16_t* p2) const {
  if (phase_ != Phase::Playing || remoteNext_ <= frame_ || localNext_ <= frame_) return false;
  const uint16_t mine = local_[frame_ % kRing], theirs = remote_[frame_ % kRing];
  *p1 = role_ == Role::Host ? mine : theirs;
  *p2 = role_ == Role::Host ? theirs : mine;
  return true;
}

void Netplay::advance(uint64_t ramHash) {
  if (phase_ != Phase::Playing) return;
  if (needHash()) {
    myHashes_.push_back({frame_ + 1, ramHash});
    if (myHashes_.size() > 16) myHashes_.erase(myHashes_.begin());
  }
  frame_++;
  checkHashes();
}

void Netplay::sendInputs() {
  if (phase_ != Phase::Playing) return;
  lastInputSent_ = now();
  const uint32_t first = std::max(peerAck_, localNext_ > 120 ? localNext_ - 120 : 0);
  const uint32_t count = localNext_ > first ? localNext_ - first : 0;
  std::vector<uint8_t> p = header(kInput);
  W w{p};
  w.u32(epoch_);
  w.u8(pause_);
  w.u32(frame_);
  w.u32(remoteNext_);
  w.u32(first);
  w.u8((uint8_t)count);
  for (uint32_t i = 0; i < count; i++) w.u16(local_[(first + i) % kRing]);
  // (the host's last check points, for the client to compare)
  const size_t nh = role_ == Role::Host ? std::min<size_t>(myHashes_.size(), 3) : 0;
  w.u8((uint8_t)nh);
  for (size_t i = myHashes_.size() - nh; i < myHashes_.size(); i++) {
    w.u32(myHashes_[i].frame);
    w.u64(myHashes_[i].hash);
  }
  sendRaw(p);
}

void Netplay::checkHashes() {
  if (role_ != Role::Client || desyncs_ > 0) {
    // (drifted: ask again now and then until the sync point comes)
    if (role_ == Role::Client && desyncs_ > 0 && phase_ == Phase::Playing && now() - lastDesyncSent_ > 0.5) {
      lastDesyncSent_ = now();
      std::vector<uint8_t> p = header(kDesync);
      W w{p};
      w.u32(epoch_);
      w.u32(frame_);
      sendRaw(p);
    }
    return;
  }
  for (size_t i = 0; i < hostHashes_.size();) {
    bool matched = false;
    for (size_t k = 0; k < myHashes_.size(); k++) {
      if (myHashes_[k].frame != hostHashes_[i].frame) continue;
      matched = true;
      if (myHashes_[k].hash != hostHashes_[i].hash) {
        desyncs_++;
        events_.push_back("The two games drifted apart - syncing again");
        lastDesyncSent_ = -1;
        hostHashes_.clear();
        checkHashes();
        return;
      }
      break;
    }
    if (matched) hostHashes_.erase(hostHashes_.begin() + i);
    else i++;
  }
}

int Netplay::timeAdjust() {
  if (phase_ != Phase::Playing || pause_ != kRunning || peerPause_ != kRunning) return 0;
  if (adjustCooldown_ > 0) {
    adjustCooldown_--;
    return 0;
  }
  if (advantage_ > 1.5) {
    adjustCooldown_ = 20;
    advantage_ -= 1;
    return -1;
  }
  if (advantage_ < -1.5) {
    adjustCooldown_ = 20;
    advantage_ += 1;
    return 1;
  }
  return 0;
}

std::string Netplay::localAddresses() const {
  std::vector<std::string> out;
#ifdef _WIN32
  char name[256] = "";
  if (gethostname(name, sizeof name) == 0) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, nullptr, &hints, &res) == 0) {
      for (addrinfo* r = res; r; r = r->ai_next) {
        char buf[64] = "";
        inet_ntop(AF_INET, &((sockaddr_in*)r->ai_addr)->sin_addr, buf, sizeof buf);
        if (strncmp(buf, "127.", 4) != 0 && std::find(out.begin(), out.end(), buf) == out.end()) out.push_back(buf);
      }
      freeaddrinfo(res);
    }
  }
#elif defined(SMAS_HAVE_IFADDRS)
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) == 0) {
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
      char buf[64] = "";
      inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, buf, sizeof buf);
      if (strncmp(buf, "127.", 4) != 0 && std::find(out.begin(), out.end(), buf) == out.end()) out.push_back(buf);
    }
    freeifaddrs(ifs);
  }
#endif
  std::string s;
  for (const std::string& a : out) s += (s.empty() ? "" : ", ") + a;
  return s;
}
