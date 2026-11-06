// Online play: two copies of the port in lockstep over UDP.
//
// The console is deterministic: given the same machine state and the same
// controllers each frame, both copies compute the same frames. So only the
// controllers travel (a couple of bytes a frame): the host's local controller
// is the console's controller 1, the client's is controller 2, and each frame
// runs on both machines once both controllers for it are known.
//
// * Input delay: a frame's local input is used `delay` frames after it is
//   read, which gives it time to reach the other machine. Chosen from the
//   ping when the session starts (or set by the host).
// * Sync points: the host sends its save state (with the settings that
//   change what the console does: widescreen margin, co-op, cheats...) and
//   both continue from it, frame 0 of a new "epoch". At the start, after the
//   host's menu changed something (a state loaded, a game started, a cheat),
//   and when the two have drifted apart. The state goes as a delta to the
//   last one both have, run-length coded, in acknowledged chunks.
// * Drift check: every second each side hashes the console's work RAM; the
//   client compares and asks for a sync point if they differ.
// * Time sync: each side tells the other which frame it is on; the one ahead
//   waits a frame now and then, so neither keeps stalling on the other.
//
// This class is the protocol and the sockets (Winsock or BSD sockets); the
// app's side (what a sync point holds, running the frames, the menus) is in
// app_net.cpp.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// What the console needs to agree on besides its state: the host's settings,
// applied on both sides for the session.
struct NetSettings {
  uint8_t ext = 0;      // widescreen margin (pixels per side)
  uint8_t flags = 0;    // kNet* below
  uint8_t game = 0;     // the game being played (App::playing, a GameId)
  uint64_t cheats = 0;  // the cheats in effect
  bool operator==(const NetSettings& o) const {
    return ext == o.ext && flags == o.flags && game == o.game && cheats == o.cheats;
  }
  bool operator!=(const NetSettings& o) const { return !(*this == o); }
};
enum : uint8_t {
  kNetEnemyRange = 1, kNetHudAnchor = 2, kNetDimWrap = 4, kNetCoop = 8, kNetBrickBounce = 16, kNetNoSpriteLimit = 32,
};

// A sync point, as the client receives it.
struct NetSync {
  uint32_t epoch = 0;
  int delay = 2;
  NetSettings settings;
  std::vector<uint8_t> state;  // Snes::saveState()
};

class Netplay {
public:
  static constexpr int kDefaultPort = 7845;
  static constexpr int kMaxDelay = 10;
  static constexpr int kHashEvery = 60;  // frames between work RAM checks

  enum class Role { None, Host, Client };
  enum class Phase {
    Off,
    Waiting,    // host: listening, no player 2 yet; client: knocking
    Connected,  // both there, no sync point yet (host: in the launcher, say)
    Syncing,    // a sync point on its way
    Playing,    // lockstep
  };
  // Why a side isn't running frames (sent along, for the other's message).
  enum Pause : uint8_t { kRunning = 0, kMenu = 1, kNoGame = 2 };

  // Checked when the client knocks: both must run the same thing.
  struct Identity {
    std::string version;   // the app's
    uint32_t stateVersion = 0;
    uint32_t romCrc = 0;   // the ROM file's CRC
    std::string msu;       // the MSU-1 pack in use (empty: none)
  };

  Netplay();
  ~Netplay();
  Netplay(const Netplay&) = delete;
  Netplay& operator=(const Netplay&) = delete;

  void setIdentity(const Identity& id) { id_ = id; }
  bool startHost(int port, std::string* err);
  bool startJoin(const std::string& address, std::string* err);  // "host", "host:port", "[v6]:port"
  void stop(const std::string& reason = "");                      // (the other side is told)

  // Each pass: packets in, chunks and acks out, timeouts.
  void poll();
  // Blocks for up to `ms` until a packet is there (a frame waiting on the other side).
  void wait(int ms);

  Role role() const { return role_; }
  Phase phase() const { return phase_; }
  bool active() const { return role_ != Role::None; }
  bool isHost() const { return role_ == Role::Host; }
  bool isClient() const { return role_ == Role::Client; }
  bool connected() const { return phase_ >= Phase::Connected; }
  const std::string& peerName() const { return peerName_; }
  int port() const { return port_; }
  int rttMs() const { return rtt_ < 0 ? -1 : (int)(rtt_ + 0.5); }
  int delay() const { return delay_; }
  int autoDelay() const;  // the delay the ping suggests
  Pause peerPause() const { return peerPause_; }
  double peerSilence() const;  // seconds since the last packet
  // Messages for the player (connected, refused, left...), oldest first.
  bool takeEvent(std::string* msg);
  // After the session ended: true once, with why (empty if this side stopped it).
  bool takeEnded(std::string* why);

  // ---- sync points
  // Host: send this state (already in the host's own console) as frame 0 of a new epoch.
  void hostSync(const std::vector<uint8_t>& state, const NetSettings& s, int delay);
  bool hostWantsSync() const { return resyncAsked_; }  // (the client saw a drift, or the base was missing)
  float syncProgress() const;                           // 0-1 while Syncing
  size_t syncBytes() const { return out_.data.size(); }  // (host: the last sync point's size on the wire)
  // Client: a sync point arrived; the app loads it, then calls synced().
  bool takeSync(NetSync* out);
  void synced();
  uint32_t epoch() const { return epoch_; }

  // ---- lockstep (Playing)
  uint32_t frame() const { return frame_; }  // the next frame to run (from the epoch's start)
  // Give the local controller for the frame `delay` ahead, if not given yet.
  void pushLocal(uint16_t pad);
  // Both controllers of frame(), host's first, if both are known.
  bool inputs(uint16_t* p1, uint16_t* p2) const;
  // The frame ran; `ramHash` is the work RAM's hash when frame()+1 is a check
  // point (see needHash), else ignored.
  void advance(uint64_t ramHash);
  bool needHash() const { return (frame_ + 1) % kHashEvery == 0; }
  void setPause(Pause p) { pause_ = p; }
  // -1: skip a frame now (ahead of the other side); +1: run one extra; 0.
  int timeAdjust();
  bool desynced() const { return desyncs_ > 0; }
  int desyncCount() const { return desyncs_; }
  std::string localAddresses() const;  // this machine's IPv4 addresses, for the host's page

  static uint64_t hash(const uint8_t* d, size_t n);
  int testDropPercent = 0;  // (testing: this share of the packets sent is dropped)

private:
  struct Addr {
    uint8_t raw[128] = {};
    int len = 0;
    bool same(const Addr& o) const;
    std::string text() const;
  };
  struct Chunked {  // a sync point's payload in flight
    uint32_t epoch = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> have;     // (client) chunk received / (host) acknowledged
    std::vector<double> sentAt;    // (host) last sent
    uint32_t count = 0, got = 0;
  };

  Identity id_;
  Role role_ = Role::None;
  Phase phase_ = Phase::Off;
  intptr_t sock_ = -1;
  int family_ = 0;
  int port_ = 0;
  Addr peer_;
  bool havePeer_ = false;
  std::string peerName_;
  uint32_t session_ = 0;
  double started_ = 0, lastRecv_ = 0, lastHello_ = -1, lastPing_ = 0, lastInputSent_ = 0;
  double rtt_ = -1;
  std::deque<std::string> events_;
  bool ended_ = false;
  std::string endWhy_;

  // sync points
  uint32_t epoch_ = 0;            // current (or arriving) epoch
  std::vector<uint8_t> base_;     // the last state both have
  uint32_t baseEpoch_ = 0;
  std::vector<uint8_t> pending_;  // host: the state on its way
  NetSettings pendingSet_;
  Chunked out_, in_;
  bool resyncAsked_ = false;
  bool syncReady_ = false;        // client: in_ complete, decoded into sync_
  NetSync sync_;
  double lastAck_ = 0;

  // lockstep
  static constexpr int kRing = 1024;
  int delay_ = 2;
  uint32_t frame_ = 0;
  uint32_t localNext_ = 0;   // local inputs known for frames < this
  uint32_t remoteNext_ = 0;  // remote inputs known for frames < this
  uint32_t peerAck_ = 0;     // the other side has our inputs for frames < this
  uint32_t peerFrame_ = 0;
  double peerFrameAt_ = 0;
  uint16_t local_[kRing] = {}, remote_[kRing] = {};
  Pause pause_ = kRunning, peerPause_ = kRunning;
  double advantage_ = 0;
  int adjustCooldown_ = 0;
  struct Check {
    uint32_t frame;
    uint64_t hash;
  };
  std::vector<Check> myHashes_, hostHashes_;  // (recent check points)
  int desyncs_ = 0;
  double lastDesyncSent_ = -1;

  double now() const;
  void closeSocket();
  void end(const std::string& why, bool tellPeer);
  void sendRaw(const std::vector<uint8_t>& p);
  std::vector<uint8_t> header(uint8_t type) const;
  void handle(const uint8_t* d, int n, const Addr& from);
  void sendInputs();
  void sendChunks();
  void sendAck();
  void startEpoch(uint32_t epoch, int delay);
  void checkHashes();
  bool decodeSync();
};
