// Online play: up to five players on up to five computers, in lockstep over UDP.
//
// The console is deterministic: given the same machine state and the same
// controllers each frame, every copy computes the same frames. So only the
// controllers travel (a couple of bytes a frame each). Each player has a slot,
// P1 to P5: the host's own players first, then each computer's as it joins
// (a computer can bring several, with its players 1..N controls). P1 and P2
// are the console's two controllers; P3 to P5 are read by the co-op engines.
//
// * The host is the hub: each computer sends its players' controllers to the
//   host, which passes them on to the others at once. A frame runs on each
//   computer once every player's controller for it is known.
// * Input delay: a frame's local input is used `delay` frames after it is
//   read, which gives it time to reach the others. Chosen from the pings
//   when a sync point is made (or set by the host).
// * Sync points: the host sends its save state (with the settings that
//   change what the console does: widescreen margin, co-op, cheats...) and
//   everyone continues from it, frame 0 of a new "epoch": at the start, when
//   a computer joins or leaves, after the host's menu changed something (a
//   state loaded, a game started, a cheat), and when a computer drifted.
//   Each computer gets it as a delta to the last state it has, run-length
//   coded, in acknowledged chunks.
// * Drift check: every second each side hashes the console's work RAM; each
//   client compares with the host's and asks for a sync point if they differ.
// * Time sync: each client tells the host which frame it is on and compares
//   with the host's; a client ahead waits a frame now and then, one behind
//   runs an extra one, so nobody keeps stalling on the others.
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
// applied on every computer for the session.
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

// A sync point, as a client receives it.
struct NetSync {
  uint32_t epoch = 0;
  int delay = 2;
  uint8_t active = 0;  // the players in the session (bit per slot)
  NetSettings settings;
  std::vector<uint8_t> state;  // Snes::saveState()
};

class Netplay {
public:
  static constexpr int kDefaultPort = 7845;
  static constexpr int kMaxDelay = 10;
  static constexpr int kHashEvery = 60;  // frames between work RAM checks
  static constexpr int kMaxPlayers = 5;  // slots P1..P5
  static constexpr int kMaxPeers = 4;    // computers joining a host

  enum class Role { None, Host, Client };
  enum class Phase {
    Off,
    Waiting,    // host: listening, nobody joined (yet, or any more); client: knocking
    Connected,  // others there, no sync point yet (host: in the launcher, say)
    Syncing,    // a sync point on its way
    Playing,    // lockstep
  };
  // Why a computer isn't running frames (sent along, for the others' message).
  enum Pause : uint8_t { kRunning = 0, kMenu = 1, kNoGame = 2 };

  // Checked when a client knocks: everyone must run the same thing.
  struct Identity {
    std::string version;   // the app's
    uint32_t stateVersion = 0;
    uint32_t romCrc = 0;   // the ROM file's
    std::string msu;       // the MSU-1 pack in use (empty: none)
  };
  // Another computer, for the menus: a host's clients, or a client's host.
  struct PeerInfo {
    std::string name;  // its address
    int rttMs = -1;
    uint8_t slots = 0;  // its players (bit per slot)
    Pause pause = kRunning;
  };

  Netplay();
  ~Netplay();
  Netplay(const Netplay&) = delete;
  Netplay& operator=(const Netplay&) = delete;

  void setIdentity(const Identity& id) { id_ = id; }
  // `localPlayers`: the players on this computer (1..4); a host's are P1 onwards.
  bool startHost(int port, int localPlayers, std::string* err);
  bool startJoin(const std::string& address, int localPlayers, std::string* err);  // "host", "host:port", "[v6]:port"
  void stop(const std::string& reason = "");  // (the others are told)

  // Each pass: packets in, chunks, inputs and acks out, timeouts.
  void poll();
  // Blocks for up to `ms` until a packet is there (a frame waiting on someone).
  void wait(int ms);

  Role role() const { return role_; }
  Phase phase() const { return phase_; }
  bool active() const { return role_ != Role::None; }
  bool isHost() const { return role_ == Role::Host; }
  bool isClient() const { return role_ == Role::Client; }
  bool connected() const { return phase_ >= Phase::Connected; }
  std::vector<PeerInfo> peers() const;
  std::string peerName() const;  // (the host's, or the first client's)
  int port() const { return port_; }
  int rttMs() const;              // the slowest connection's
  int delay() const { return delay_; }
  int autoDelay() const;          // the delay the pings suggest
  uint8_t localSlots() const { return local_; }  // this computer's players (bit per slot)
  uint8_t activeSlots() const { return active_; }
  // Who has stopped the game for everyone: the first slot of a computer
  // with its menu open or (the host) no game running; -1 nobody.
  int pausedSlot() const { return pausedSlot_; }
  Pause pausedWhy() const { return pausedWhy_; }
  double silence() const;  // seconds since the slowest connection's last packet
  // Messages for the players (joined, refused, left...), oldest first.
  bool takeEvent(std::string* msg);
  // After the session ended: true once, with why (empty if this side stopped it).
  bool takeEnded(std::string* why);

  // ---- sync points
  // Host: send this state (already in the host's own console) as frame 0 of a new epoch.
  void hostSync(const std::vector<uint8_t>& state, const NetSettings& s, int delay);
  bool hostWantsSync() const { return resyncAsked_; }  // (someone joined, left, or drifted)
  float syncProgress() const;                           // 0-1 while Syncing
  size_t syncBytes() const;                             // (host: the last sync point's size on the wire, most of any)
  // Client: a sync point arrived; the app loads it, then calls synced().
  bool takeSync(NetSync* out);
  void synced();
  uint32_t epoch() const { return epoch_; }

  // ---- lockstep (Playing)
  uint32_t frame() const { return frame_; }  // the next frame to run (from the epoch's start)
  // This computer's players' controllers (one each, in slot order) for the
  // frame `delay` ahead, if not given yet.
  void pushLocal(const uint16_t* pads);
  // Every slot's controller for frame(), if all are known (0 for slots not playing).
  bool inputs(uint16_t* pads) const;
  // The frame ran; `ramHash` is the work RAM's hash when frame()+1 is a check
  // point (see needHash), else ignored.
  void advance(uint64_t ramHash);
  bool needHash() const { return (frame_ + 1) % kHashEvery == 0; }
  void setPause(Pause p) { pause_ = p; }
  // -1: skip a frame now (ahead of the host); +1: run one extra; 0.
  int timeAdjust();
  int desyncCount() const { return desyncs_; }
  std::string localAddresses() const;  // this machine's IPv4 addresses, for the host's page

  static uint64_t hash(const uint8_t* d, size_t n);
  static std::string slotList(uint8_t slots);  // "P3", "P3 and P4"
  int testDropPercent = 0;  // (testing: this share of the packets sent is dropped)

private:
  static constexpr int kRing = 1024;
  struct Addr {
    uint8_t raw[128] = {};
    int len = 0;
    bool same(const Addr& o) const;
    std::string text() const;
  };
  struct Chunked {  // a sync point's payload in flight
    uint32_t epoch = 0;
    uint8_t id = 0;                // (host: which encoding of it; a full one after a base went missing)
    std::vector<uint8_t> data;
    std::vector<uint8_t> have;     // (client) chunk received / (host) acknowledged
    std::vector<double> sentAt;    // (host) last sent
    uint32_t count = 0, got = 0;
  };
  struct Peer {
    Addr addr;
    std::string name;
    uint32_t session = 0;
    uint8_t slots = 0;             // (host: this client's players)
    double lastRecv = 0, lastPing = 0, lastInput = 0;
    double rtt = -1;
    std::vector<uint8_t> base;     // (host) the last state this client has
    uint32_t baseEpoch = 0;
    Chunked out;                   // (host) the sync point on its way to it
    bool confirmed = false;        // (host) it has the current epoch's sync point
    uint32_t ack[kMaxPlayers] = {};  // it has slot s's inputs for frames < ack[s]
    uint32_t frame = 0;
    Pause pause = kRunning;
  };

  Identity id_;
  Role role_ = Role::None;
  Phase phase_ = Phase::Off;
  intptr_t sock_ = -1;
  int port_ = 0;
  int localCount_ = 1;
  uint8_t local_ = 0, active_ = 0;
  std::vector<Peer> peers_;  // host: its clients; client: [0] the host
  double started_ = 0, lastHello_ = -1;
  std::deque<std::string> events_;
  bool ended_ = false;
  std::string endWhy_;

  // sync points
  uint32_t epoch_ = 0;            // current (or arriving) epoch
  std::vector<uint8_t> pending_;  // host: the state on its way; client: the one arrived
  NetSettings pendingSet_;
  uint8_t pendingActive_ = 0;
  Chunked in_;                    // (client)
  std::vector<uint8_t> base_;     // (client) the last state it has
  uint32_t baseEpoch_ = 0;
  bool resyncAsked_ = false;
  bool syncReady_ = false;        // client: in_ complete, decoded into sync_
  NetSync sync_;
  double lastAck_ = 0;

  // lockstep
  int delay_ = 2;
  uint32_t frame_ = 0;
  uint16_t pads_[kMaxPlayers][kRing] = {};
  uint32_t next_[kMaxPlayers] = {};  // slot s's inputs known for frames < next_[s]
  Pause pause_ = kRunning;
  int pausedSlot_ = -1;
  Pause pausedWhy_ = kRunning;
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
  void end(const std::string& why, bool tell);
  void sendTo(const Addr& a, const std::vector<uint8_t>& p);
  void send(Peer& p, const std::vector<uint8_t>& pkt) { sendTo(p.addr, pkt); }
  std::vector<uint8_t> header(uint8_t type, uint32_t session) const;
  void handle(const uint8_t* d, int n, const Addr& from);
  void handleHello(const uint8_t* d, int n, const Addr& from);
  void handleInput(Peer& p, const uint8_t* d, int n, bool syncDone);
  void dropPeer(size_t i, const std::string& why);
  void sendInputs(Peer& p);
  void sendChunks(Peer& p);
  void encodeFor(Peer& p);
  void sendAck();
  void startEpoch(uint32_t epoch, int delay, uint8_t active);
  void checkHashes();
  bool decodeSync();
  void updatePaused();
  Peer* host() { return role_ == Role::Client && !peers_.empty() ? &peers_[0] : nullptr; }
};
