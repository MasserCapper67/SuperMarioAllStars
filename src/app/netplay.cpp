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

constexpr uint32_t kMagic = 0x324E4D53;  // "SMN2"
constexpr uint16_t kProtocol = 2;
constexpr int kChunkBytes = 1100;
constexpr int kMaxSend = 100;            // a slot's inputs in one packet, at most
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
void closeSock(intptr_t s) { closesocket((SockT)s); }
#else
void closeSock(intptr_t s) { close((int)s); }
#endif

void setNonBlocking(intptr_t s) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket((SockT)s, FIONBIO, &on);
  // (an ICMP "port unreachable" would otherwise fail the next recvfrom)
  BOOL report = FALSE;
  DWORD got = 0;
  WSAIoctl((SockT)s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &got, nullptr, nullptr);
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

int bits(uint8_t m) {
  int n = 0;
  for (; m; m &= m - 1) n++;
  return n;
}
int lowest(uint8_t m) {
  for (int s = 0; s < 8; s++)
    if (m >> s & 1) return s;
  return -1;
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

std::string Netplay::slotList(uint8_t slots) {
  std::vector<std::string> v;
  for (int s = 0; s < kMaxPlayers; s++)
    if (slots >> s & 1) v.push_back("P" + std::to_string(s + 1));
  std::string out;
  for (size_t i = 0; i < v.size(); i++) out += (i == 0 ? "" : i + 1 == v.size() ? " and " : ", ") + v[i];
  return out;
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

bool Netplay::startHost(int port, int localPlayers, std::string* err) {
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
    if (!bound) closeSock(s);
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
  }
  setNonBlocking(s);
  sock_ = s;
  role_ = Role::Host;
  phase_ = Phase::Waiting;
  port_ = port;
  localCount_ = std::clamp(localPlayers, 1, kMaxPlayers - 1);
  local_ = (uint8_t)((1 << localCount_) - 1);
  active_ = local_;
  started_ = now();
  ended_ = false;
  return true;
}

bool Netplay::startJoin(const std::string& address, int localPlayers, std::string* err) {
  stop();
  std::string a = address;
  while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) a.pop_back();
  while (!a.empty() && (a[0] == ' ' || a[0] == '\t')) a.erase(0, 1);
  std::string hostName = a, portText;
  if (!a.empty() && a[0] == '[') {  // [v6]:port
    size_t close = a.find(']');
    if (close == std::string::npos) {
      if (err) *err = "Missing ] in the address";
      return false;
    }
    hostName = a.substr(1, close - 1);
    if (close + 1 < a.size() && a[close + 1] == ':') portText = a.substr(close + 2);
  } else if (std::count(a.begin(), a.end(), ':') == 1) {
    hostName = a.substr(0, a.find(':'));
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
  if (hostName.empty()) {
    if (err) *err = "Enter the host's address";
    return false;
  }
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  if (getaddrinfo(hostName.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    if (err) *err = "Couldn't find \"" + hostName + "\" (check the address)";
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
  Peer h;
  std::memcpy(h.addr.raw, pick->ai_addr, std::min<size_t>(pick->ai_addrlen, sizeof h.addr.raw));
  h.addr.len = (int)pick->ai_addrlen;
  freeaddrinfo(res);
  h.name = h.addr.text();
  h.lastRecv = now();
  peers_.assign(1, h);
  role_ = Role::Client;
  phase_ = Phase::Waiting;
  port_ = port;
  localCount_ = std::clamp(localPlayers, 1, kMaxPlayers - 1);
  local_ = 0;  // (given by the host's welcome)
  active_ = 0;
  started_ = now();
  lastHello_ = -1;
  ended_ = false;
  return true;
}

void Netplay::stop(const std::string& reason) {
  if (role_ == Role::None) return;
  end(reason, true);
  endWhy_.clear();  // (stopped here: nothing to tell)
}

void Netplay::end(const std::string& why, bool tell) {
  if (tell && sock_ != -1)
    for (Peer& p : peers_) {
      std::vector<uint8_t> pkt = header(kBye, p.session);
      W{pkt}.str(why);
      send(p, pkt);
    }
  if (sock_ != -1) closeSock(sock_);
  sock_ = -1;
  role_ = Role::None;
  phase_ = Phase::Off;
  peers_.clear();
  local_ = active_ = 0;
  epoch_ = baseEpoch_ = 0;
  base_.clear();
  pending_.clear();
  in_ = Chunked();
  resyncAsked_ = syncReady_ = false;
  pause_ = kRunning;
  pausedSlot_ = -1;
  pausedWhy_ = kRunning;
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

std::vector<Netplay::PeerInfo> Netplay::peers() const {
  std::vector<PeerInfo> v;
  for (const Peer& p : peers_)
    if (role_ == Role::Client || p.session) {
      PeerInfo i;
      i.name = p.name;
      i.rttMs = p.rtt < 0 ? -1 : (int)(p.rtt + 0.5);
      i.slots = role_ == Role::Client ? (uint8_t)(active_ & ~local_) : p.slots;
      i.pause = p.pause;
      v.push_back(i);
    }
  return v;
}

std::string Netplay::peerName() const { return peers_.empty() ? std::string() : peers_[0].name; }

int Netplay::rttMs() const {
  double worst = -1;
  for (const Peer& p : peers_) worst = std::max(worst, p.rtt);
  return worst < 0 ? -1 : (int)(worst + 0.5);
}

double Netplay::silence() const {
  double t = now(), worst = 0;
  for (const Peer& p : peers_) worst = std::max(worst, t - p.lastRecv);
  return worst;
}

int Netplay::autoDelay() const {
  // The slowest path a controller takes: one client to the host and on to
  // another (the two slowest clients' half pings, and the host's pass in
  // between), or host to client.
  std::vector<double> r;
  for (const Peer& p : peers_)
    if (p.rtt >= 0) r.push_back(p.rtt);
  if (r.empty()) return 2;
  std::sort(r.rbegin(), r.rend());
  double path = r.size() >= 2 && role_ == Role::Host ? (r[0] + r[1]) * 0.5 + kFrameSeconds * 1000.0 : r[0] * 0.5;
  int d = 1 + (int)std::ceil((path + 3.0) / (kFrameSeconds * 1000.0));
  return std::clamp(d, 1, kMaxDelay);
}

std::vector<uint8_t> Netplay::header(uint8_t type, uint32_t session) const {
  std::vector<uint8_t> p;
  p.reserve(64);
  W w{p};
  w.u32(kMagic);
  w.u8(type);
  w.u32(session);
  return p;
}

void Netplay::sendTo(const Addr& a, const std::vector<uint8_t>& p) {
  if (sock_ == -1) return;
  if (testDropPercent > 0 && (int)(std::random_device()() % 100) < testDropPercent) return;
  sendto((SockT)sock_, (const char*)p.data(), (int)p.size(), 0, (const sockaddr*)a.raw, (socklen_t)a.len);
}

void Netplay::updatePaused() {
  // (who stopped the game: the host first, then the clients by slot)
  pausedSlot_ = -1;
  pausedWhy_ = kRunning;
  if (role_ != Role::Host) return;
  if (pause_ != kRunning) {
    pausedSlot_ = lowest(local_);
    pausedWhy_ = pause_;
    return;
  }
  for (const Peer& p : peers_)
    if (p.session && p.pause != kRunning && (pausedSlot_ < 0 || lowest(p.slots) < pausedSlot_)) {
      pausedSlot_ = lowest(p.slots);
      pausedWhy_ = p.pause;
    }
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
      std::vector<uint8_t> p = header(kHello, 0);
      W w{p};
      w.u16(kProtocol);
      w.u32(id_.stateVersion);
      w.u32(id_.romCrc);
      w.str(id_.version);
      w.str(id_.msu);
      w.u8((uint8_t)localCount_);
      send(peers_[0], p);
    }
    if (t - started_ > kJoinTimeout)
      end("No answer from " + peers_[0].name + ". Check the address, that the other player is hosting, and that UDP "
              "port " + std::to_string(port_) + " is forwarded to the host (or that you share a VPN).",
          false);
    return;
  }
  for (size_t i = 0; i < peers_.size();) {
    Peer& p = peers_[i];
    if (role_ == Role::Host && !p.session) {
      i++;
      continue;
    }
    if (t - p.lastRecv > kTimeout) {
      if (role_ == Role::Client) {
        end("Connection lost (nothing heard for " + std::to_string((int)kTimeout) + " seconds)", false);
        return;
      }
      dropPeer(i, Netplay::slotList(p.slots) + " lost the connection");
      continue;
    }
    if (t - p.lastPing > 0.5) {
      p.lastPing = t;
      std::vector<uint8_t> pkt = header(kPing, p.session);
      W{pkt}.u64((uint64_t)(t * 1e6));
      send(p, pkt);
    }
    if (role_ == Role::Host && phase_ == Phase::Syncing && !p.confirmed) sendChunks(p);
    if (phase_ == Phase::Playing && t - p.lastInput > 0.008) sendInputs(p);
    i++;
  }
  if (role_ == Role::Client && phase_ == Phase::Syncing && in_.count && t - lastAck_ > 0.03) sendAck();
  updatePaused();
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

void Netplay::handleHello(const uint8_t* d, int n, const Addr& from) {
  R r{d, d + n};
  auto refuse = [&](const std::string& why) {
    std::vector<uint8_t> p = header(kRefuse, 0);
    W{p}.str(why);
    sendTo(from, p);
  };
  for (Peer& p : peers_)
    if (p.addr.same(from)) {  // (our welcome was lost)
      std::vector<uint8_t> pkt = header(kWelcome, p.session);
      W w{pkt};
      w.str(id_.version);
      w.u8(p.slots);
      send(p, pkt);
      return;
    }
  const uint16_t proto = r.u16();
  const uint32_t stateVersion = r.u32();
  const uint32_t romCrc = r.u32();
  const std::string version = r.str(), msu = r.str();
  const int want = std::clamp((int)r.u8(), 1, kMaxPlayers - 1);
  if (!r.ok) return;
  std::string why;
  if (proto != kProtocol || version != id_.version || stateVersion != id_.stateVersion)
    why = "Different versions of the game: the host runs " + id_.version + " (online " + std::to_string(kProtocol) +
          "), you run " + version + " (online " + std::to_string(proto) + "). Everyone needs the same build.";
  else if (romCrc != id_.romCrc)
    why = "Different ROMs: everyone needs the same Super Mario All-Stars ROM file";
  else if (msu != id_.msu)
    why = "Different music packs (host: " + (id_.msu.empty() ? std::string("none") : id_.msu) +
          ", you: " + (msu.empty() ? std::string("none") : msu) + "). Choose the same one, or none, everywhere.";
  // (free slots, lowest first)
  uint8_t taken = local_, slots = 0;
  int computers = 0;
  for (const Peer& p : peers_) taken |= p.slots, computers++;
  for (int s = 0; s < kMaxPlayers && bits(slots) < want; s++)
    if (!(taken >> s & 1)) slots |= (uint8_t)(1 << s);
  if (why.empty() && (bits(slots) < want || computers >= kMaxPeers))
    why = bits(taken) >= kMaxPlayers || computers >= kMaxPeers
              ? "The game is full (" + std::to_string(kMaxPlayers) + " players)"
              : "Only " + std::to_string(bits(slots)) + " more player" + (bits(slots) == 1 ? "" : "s") +
                    " can join: set Players on This Computer to that";
  if (!why.empty()) {
    refuse(why);
    events_.push_back("A player from " + from.text() + " couldn't join: " + why);
    return;
  }
  Peer p;
  p.addr = from;
  p.name = from.text();
  std::random_device rd;
  p.session = (rd() << 1) | 1;
  p.slots = slots;
  p.lastRecv = now();
  peers_.push_back(p);
  if (phase_ == Phase::Waiting) phase_ = Phase::Connected;
  resyncAsked_ = true;  // (a sync point with the new players)
  std::vector<uint8_t> pkt = header(kWelcome, p.session);
  W w{pkt};
  w.str(id_.version);
  w.u8(slots);
  send(peers_.back(), pkt);
  events_.push_back(slotList(slots) + " joined from " + p.name);
}

void Netplay::dropPeer(size_t i, const std::string& why) {
  events_.push_back(why);
  peers_.erase(peers_.begin() + i);
  if (peers_.empty()) {
    // (alone again: the host plays on by itself, and waits for others)
    phase_ = Phase::Waiting;
    active_ = local_;
    resyncAsked_ = false;
    return;
  }
  resyncAsked_ = true;  // (a sync point without its players)
}

void Netplay::handle(const uint8_t* d, int n, const Addr& from) {
  R r{d, d + n};
  if (r.u32() != kMagic) return;
  const uint8_t type = r.u8();
  const uint32_t session = r.u32();
  if (!r.ok) return;
  const double t = now();

  if (type == kHello) {
    if (role_ == Role::Host) handleHello(r.p, (int)(r.e - r.p), from);
    return;
  }
  Peer* pp = nullptr;
  size_t index = 0;
  for (size_t i = 0; i < peers_.size(); i++)
    if (peers_[i].addr.same(from)) pp = &peers_[i], index = i;
  if (!pp) return;
  Peer& p = *pp;
  if (role_ == Role::Client && phase_ == Phase::Waiting) {
    if (type == kWelcome) {
      p.session = session;
      p.lastRecv = t;
      r.str();
      local_ = r.u8();
      if (!r.ok || !local_) return;
      phase_ = Phase::Connected;
      events_.push_back("Connected to " + p.name + " as " + slotList(local_) + " - waiting for the host's game");
    } else if (type == kRefuse) {
      end("The host refused: " + r.str(), false);
    }
    return;
  }
  if (session != p.session) return;
  p.lastRecv = t;

  switch (type) {
    case kBye: {
      std::string why = r.str();
      if (role_ == Role::Client) {
        end(why.empty() ? "The host ended the session" : why, false);
      } else {
        dropPeer(index, slotList(p.slots) + " left");
      }
      return;
    }
    case kPing: {
      std::vector<uint8_t> pkt = header(kPong, p.session);
      W{pkt}.u64(r.u64());
      send(p, pkt);
      return;
    }
    case kPong: {
      uint64_t sent = r.u64();
      if (!r.ok) return;
      double ms = (t - sent / 1e6) * 1000.0;
      if (ms < 0 || ms > 5000) return;
      p.rtt = p.rtt < 0 ? ms : p.rtt * 0.8 + ms * 0.2;
      return;
    }
    case kChunk: {
      if (role_ != Role::Client) return;
      const uint32_t epoch = r.u32();
      const uint8_t id = r.u8();
      const uint32_t total = r.u32(), index = r.u32();
      const uint16_t len = r.u16();
      const uint8_t* data = r.bytes(len);
      if (!r.ok || epoch < in_.epoch || total > (64u << 20)) return;
      if (phase_ == Phase::Playing && epoch == epoch_) {  // (applied: our done went missing)
        std::vector<uint8_t> pkt = header(kSyncDone, p.session);
        W{pkt}.u32(epoch_);
        send(p, pkt);
        return;
      }
      if (epoch > in_.epoch || id != in_.id) {  // (a new sync point, or a new encoding of it)
        in_ = Chunked();
        in_.epoch = epoch;
        in_.id = id;
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
        if (in_.got == in_.count && !syncReady_ && !decodeSync()) {
          std::vector<uint8_t> pkt = header(kNeedFull, p.session);  // (our base isn't the one it was made from)
          W w{pkt};
          w.u32(epoch);
          w.u8(id);
          send(p, pkt);
        }
      }
      return;
    }
    case kChunkAck: {
      if (role_ != Role::Host || phase_ != Phase::Syncing) return;
      const uint32_t epoch = r.u32();
      const uint8_t id = r.u8();
      const uint32_t contiguous = r.u32();
      const uint16_t nb = r.u16();
      const uint8_t* map = r.bytes((nb + 7) / 8);
      if (!r.ok || epoch != p.out.epoch || id != p.out.id) return;
      for (uint32_t i = 0; i < std::min(contiguous, p.out.count); i++) p.out.have[i] = 1;
      for (uint32_t i = 0; i < nb; i++)
        if (contiguous + i < p.out.count && (map[i / 8] >> (i % 8) & 1)) p.out.have[contiguous + i] = 1;
      return;
    }
    case kNeedFull: {
      if (role_ != Role::Host || phase_ != Phase::Syncing) return;
      const uint32_t epoch = r.u32();
      const uint8_t id = r.u8();
      if (!r.ok || epoch != p.out.epoch || id != p.out.id) return;
      p.base.clear();  // (the whole state, from nothing)
      p.baseEpoch = 0;
      encodeFor(p);
      sendChunks(p);
      return;
    }
    case kSyncDone:
    case kInput: handleInput(p, r.p, (int)(r.e - r.p), type == kSyncDone); return;
    case kDesync: {
      if (role_ != Role::Host) return;
      const uint32_t epoch = r.u32(), frame = r.u32();
      if (!r.ok || epoch != epoch_ || phase_ != Phase::Playing || resyncAsked_) return;
      resyncAsked_ = true;
      desyncs_++;
      events_.push_back(slotList(p.slots) + "'s game drifted apart at frame " + std::to_string(frame) +
                        " - syncing again");
      return;
    }
    default: return;
  }
}

void Netplay::handleInput(Peer& p, const uint8_t* d, int n, bool syncDone) {
  R r{d, d + n};
  const uint32_t epoch = r.u32();
  if (!r.ok) return;
  if (role_ == Role::Host && phase_ == Phase::Syncing && epoch == epoch_ && !p.confirmed) {
    // (this client has the sync point; once all have, from frame 0 together)
    p.confirmed = true;
    p.base = pending_;
    p.baseEpoch = epoch_;
    bool all = true;
    for (const Peer& q : peers_) all &= q.confirmed || !q.session;
    if (all) startEpoch(epoch_, delay_, pendingActive_);
  }
  if (syncDone || phase_ != Phase::Playing || epoch != epoch_) return;
  const uint8_t pause = r.u8();
  const uint32_t frame = r.u32();
  const uint8_t pausedSlot = r.u8(), pausedWhy = r.u8();
  if (!r.ok) return;
  p.pause = (Pause)std::min<uint8_t>(pause, kNoGame);
  p.frame = frame;
  if (role_ == Role::Client) {
    pausedSlot_ = pausedSlot == 0xFF ? -1 : pausedSlot;
    pausedWhy_ = (Pause)std::min<uint8_t>(pausedWhy, kNoGame);
    if (pause_ == kRunning && pausedSlot_ < 0) {
      const double lag = p.rtt < 0 ? 0 : p.rtt * 0.0005 / kFrameSeconds;
      const double adv = (double)frame_ - ((double)frame + lag);
      advantage_ = advantage_ * 0.9 + adv * 0.1;
    }
  }
  // The other side's players' controllers (a client's own, or, from the
  // host, everyone else's), each slot's from where it has them.
  bool fresh = false;
  const uint8_t nStreams = r.u8();
  for (uint8_t k = 0; k < nStreams && r.ok; k++) {
    const uint8_t s = r.u8();
    const uint32_t first = r.u32();
    const uint8_t count = r.u8();
    for (uint8_t i = 0; i < count; i++) {
      const uint16_t pad = r.u16();
      if (!r.ok) return;
      if (s >= kMaxPlayers || (local_ >> s & 1)) continue;
      if (role_ == Role::Host && !(p.slots >> s & 1)) continue;  // (only its own players)
      const uint32_t f = first + i;
      if (f == next_[s] && f < frame_ + kRing / 2) {
        pads_[s][f % kRing] = pad;
        next_[s]++;
        fresh = true;
      }
    }
  }
  // How far it has the controllers sent to it.
  const uint8_t nAcks = r.u8();
  for (uint8_t k = 0; k < nAcks && r.ok; k++) {
    const uint8_t s = r.u8();
    const uint32_t a = r.u32();
    if (r.ok && s < kMaxPlayers && a > p.ack[s] && a <= next_[s]) p.ack[s] = a;
  }
  const uint8_t nh = r.u8();
  for (uint8_t i = 0; i < nh && r.ok; i++) {
    Check c;
    c.frame = r.u32();
    c.hash = r.u64();
    if (!r.ok || role_ != Role::Client) break;
    bool known = false;
    for (const Check& h : hostHashes_) known |= h.frame == c.frame;
    if (!known) hostHashes_.push_back(c);
    if (hostHashes_.size() > 16) hostHashes_.erase(hostHashes_.begin());
  }
  if (role_ == Role::Client) checkHashes();
  // (the host passes a client's controllers on at once, not at its next pass)
  if (fresh && role_ == Role::Host)
    for (Peer& q : peers_)
      if (&q != &p && q.session && now() - q.lastInput > 0.002) sendInputs(q);
}

// ---------------------------------------------------------------------------
// Sync points

void Netplay::hostSync(const std::vector<uint8_t>& state, const NetSettings& s, int delay) {
  if (role_ != Role::Host || peers_.empty()) return;
  resyncAsked_ = false;
  pending_ = state;
  pendingSet_ = s;
  pendingActive_ = local_;
  for (const Peer& p : peers_) pendingActive_ |= p.slots;
  delay_ = std::clamp(delay, 1, kMaxDelay);
  epoch_++;
  for (Peer& p : peers_) {
    p.confirmed = false;
    encodeFor(p);
    sendChunks(p);
  }
  phase_ = Phase::Syncing;
}

// The sync point for one client: a delta to the last state it has.
void Netplay::encodeFor(Peer& p) {
  std::vector<uint8_t> payload;
  W w{payload};
  w.u32(epoch_);
  w.u32(p.baseEpoch);
  w.u8((uint8_t)delay_);
  w.u8(pendingActive_);
  w.u8(pendingSet_.ext);
  w.u8(pendingSet_.flags);
  w.u8(pendingSet_.game);
  w.u64(pendingSet_.cheats);
  w.u32((uint32_t)pending_.size());
  w.u64(hash(pending_.data(), pending_.size()));
  std::vector<uint8_t> delta = encodeDelta(pending_, p.base);
  payload.insert(payload.end(), delta.begin(), delta.end());
  const uint8_t id = (uint8_t)(p.out.id + 1);
  p.out = Chunked();
  p.out.epoch = epoch_;
  p.out.id = id;
  p.out.data = std::move(payload);
  p.out.count = (uint32_t)((p.out.data.size() + kChunkBytes - 1) / kChunkBytes);
  p.out.have.assign(p.out.count, 0);
  p.out.sentAt.assign(p.out.count, -1e9);
}

void Netplay::sendChunks(Peer& p) {
  const double t = now();
  const double rto = std::max(0.12, p.rtt < 0 ? 0.3 : p.rtt * 0.002 + 0.03);
  int budget = 40;  // (chunks a pass: ~2.6 MB/s at 60 passes a second at most)
  for (uint32_t i = 0; i < p.out.count && budget > 0; i++) {
    if (p.out.have[i] || t - p.out.sentAt[i] < rto) continue;
    p.out.sentAt[i] = t;
    budget--;
    const size_t off = (size_t)i * kChunkBytes;
    const size_t len = std::min<size_t>(kChunkBytes, p.out.data.size() - off);
    std::vector<uint8_t> pkt = header(kChunk, p.session);
    W w{pkt};
    w.u32(p.out.epoch);
    w.u8(p.out.id);
    w.u32((uint32_t)p.out.data.size());
    w.u32(i);
    w.u16((uint16_t)len);
    w.bytes(p.out.data.data() + off, len);
    send(p, pkt);
  }
}

void Netplay::sendAck() {
  Peer* h = host();
  if (!h) return;
  lastAck_ = now();
  uint32_t contiguous = 0;
  while (contiguous < in_.count && in_.have[contiguous]) contiguous++;
  const uint16_t nb = (uint16_t)std::min<uint32_t>(in_.count - contiguous, 1024);
  std::vector<uint8_t> map((nb + 7) / 8, 0);
  for (uint32_t i = 0; i < nb; i++)
    if (in_.have[contiguous + i]) map[i / 8] |= (uint8_t)(1 << (i % 8));
  std::vector<uint8_t> pkt = header(kChunkAck, h->session);
  W w{pkt};
  w.u32(in_.epoch);
  w.u8(in_.id);
  w.u32(contiguous);
  w.u16(nb);
  w.bytes(map.data(), map.size());
  send(*h, pkt);
}

bool Netplay::decodeSync() {
  R r{in_.data.data(), in_.data.data() + in_.data.size()};
  NetSync s;
  s.epoch = r.u32();
  const uint32_t baseEpoch = r.u32();
  s.delay = r.u8();
  s.active = r.u8();
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
  Peer* h = host();
  if (!h || in_.got != in_.count || !in_.count) return;
  base_ = std::move(pending_);
  pending_.clear();
  baseEpoch_ = in_.epoch;
  // (the payload's delay and players again: takeSync gave the copy away)
  R r{in_.data.data() + 8, in_.data.data() + in_.data.size()};
  const int delay = r.u8();
  const uint8_t active = r.u8();
  startEpoch(in_.epoch, delay, active);
  std::vector<uint8_t> pkt = header(kSyncDone, h->session);
  W{pkt}.u32(epoch_);
  send(*h, pkt);
  sendInputs(*h);
}

float Netplay::syncProgress() const {
  if (role_ == Role::Client) return in_.count ? (float)in_.got / in_.count : 0.0f;
  uint32_t n = 0, total = 0;
  for (const Peer& p : peers_) {
    total += p.out.count;
    for (uint8_t h : p.out.have) n += h;
  }
  return total ? (float)n / total : 0.0f;
}

size_t Netplay::syncBytes() const {
  size_t most = 0;
  for (const Peer& p : peers_) most = std::max(most, p.out.data.size());
  return most;
}

void Netplay::startEpoch(uint32_t epoch, int delay, uint8_t active) {
  epoch_ = epoch;
  delay_ = std::clamp(delay, 1, kMaxDelay);
  active_ = active;
  frame_ = 0;
  // (the first `delay` frames: every controller untouched, everywhere)
  for (int s = 0; s < kMaxPlayers; s++) {
    for (int i = 0; i < delay_; i++) pads_[s][i] = 0;
    next_[s] = (uint32_t)delay_;
  }
  for (Peer& p : peers_) {
    for (int s = 0; s < kMaxPlayers; s++) p.ack[s] = (uint32_t)delay_;
    p.frame = 0;
    p.pause = kRunning;
  }
  advantage_ = 0;
  adjustCooldown_ = 0;
  myHashes_.clear();
  hostHashes_.clear();
  desyncs_ = 0;
  phase_ = Phase::Playing;
}

// ---------------------------------------------------------------------------
// Lockstep

void Netplay::pushLocal(const uint16_t* pads) {
  if (phase_ != Phase::Playing) return;
  bool added = false;
  int k = 0;
  for (int s = 0; s < kMaxPlayers; s++) {
    if (!(local_ >> s & 1)) continue;
    const uint16_t pad = pads[k++];
    while (next_[s] <= frame_ + (uint32_t)delay_) {
      pads_[s][next_[s] % kRing] = pad;
      next_[s]++;
      added = true;
    }
  }
  if (added)
    for (Peer& p : peers_)
      if (p.session) sendInputs(p);
}

bool Netplay::inputs(uint16_t* pads) const {
  if (phase_ != Phase::Playing) return false;
  for (int s = 0; s < kMaxPlayers; s++) {
    if (!(active_ >> s & 1)) {
      pads[s] = 0;
      continue;
    }
    if (next_[s] <= frame_) return false;
    pads[s] = pads_[s][frame_ % kRing];
  }
  return true;
}

void Netplay::advance(uint64_t ramHash) {
  if (phase_ != Phase::Playing) return;
  if (needHash()) {
    myHashes_.push_back({frame_ + 1, ramHash});
    if (myHashes_.size() > 16) myHashes_.erase(myHashes_.begin());
  }
  frame_++;
  if (role_ == Role::Client) checkHashes();
}

// To one other computer: the controllers it lacks (a client: its own players';
// the host: everyone's but that client's), what we have of its, our pause.
void Netplay::sendInputs(Peer& p) {
  if (phase_ != Phase::Playing) return;
  p.lastInput = now();
  std::vector<uint8_t> pkt = header(kInput, p.session);
  W w{pkt};
  w.u32(epoch_);
  w.u8(pause_);
  w.u32(frame_);
  w.u8(pausedSlot_ < 0 ? 0xFF : (uint8_t)pausedSlot_);
  w.u8(pausedWhy_);
  uint8_t sendSlots = 0, ackSlots = 0;
  if (role_ == Role::Host) {
    sendSlots = (uint8_t)(active_ & ~p.slots);
    ackSlots = (uint8_t)(active_ & p.slots);
  } else {
    sendSlots = local_;
    ackSlots = (uint8_t)(active_ & ~local_);
  }
  w.u8((uint8_t)bits(sendSlots));
  for (int s = 0; s < kMaxPlayers; s++) {
    if (!(sendSlots >> s & 1)) continue;
    const uint32_t first = std::max(p.ack[s], next_[s] > (uint32_t)kMaxSend ? next_[s] - kMaxSend : 0);
    const uint32_t count = next_[s] > first ? next_[s] - first : 0;
    w.u8((uint8_t)s);
    w.u32(first);
    w.u8((uint8_t)count);
    for (uint32_t i = 0; i < count; i++) w.u16(pads_[s][(first + i) % kRing]);
  }
  w.u8((uint8_t)bits(ackSlots));
  for (int s = 0; s < kMaxPlayers; s++)
    if (ackSlots >> s & 1) {
      w.u8((uint8_t)s);
      w.u32(next_[s]);
    }
  // (the host's last check points, for the client to compare)
  const size_t nh = role_ == Role::Host ? std::min<size_t>(myHashes_.size(), 3) : 0;
  w.u8((uint8_t)nh);
  for (size_t i = myHashes_.size() - nh; i < myHashes_.size(); i++) {
    w.u32(myHashes_[i].frame);
    w.u64(myHashes_[i].hash);
  }
  send(p, pkt);
}

void Netplay::checkHashes() {
  Peer* h = host();
  if (!h) return;
  if (desyncs_ > 0) {
    // (drifted: ask again now and then until the sync point comes)
    if (phase_ == Phase::Playing && now() - lastDesyncSent_ > 0.5) {
      lastDesyncSent_ = now();
      std::vector<uint8_t> pkt = header(kDesync, h->session);
      W w{pkt};
      w.u32(epoch_);
      w.u32(frame_);
      send(*h, pkt);
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
        events_.push_back("This game drifted apart from the host's - syncing again");
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
  if (role_ != Role::Client || phase_ != Phase::Playing || pause_ != kRunning || pausedSlot_ >= 0) return 0;
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
