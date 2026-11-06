// Online play: the app's side (see netplay.h for the protocol).
//
// The host's console is the session's: its local controller is controller 1,
// the client's is controller 2, and the host's settings that change what the
// console does (widescreen margin, co-op, cheats, NES brick bounce...) are
// used by both while connected. The host's menu does everything as usual;
// whatever it changes (a state loaded, a game started, the level select, a
// setting) is sent as a new sync point when the game resumes. The client
// plays, opens its menu (which pauses both), and leaves; it can't load
// states or start games meanwhile. Its own battery save is never written
// while the console holds the host's, and comes back when the session ends.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "app/app.h"
#include "app/vkeys.h"

// ---------------------------------------------------------------------------
// Each pass

void App::netPass(double dt) {
  if (!netArgsDone) {  // (--host / --join on the command line)
    netArgsDone = true;
    if (netArgHost >= 0) netHost(netArgHost > 0 ? netArgHost : cfg.netPort);
    else if (!netArgJoin.empty()) netJoin(netArgJoin);
  }
  if (net.active()) net.poll();
  std::string msg;
  while (net.takeEvent(&msg)) menu.notify(msg, 4.5f);
  if (net.takeEnded(&msg)) {
    netEnded(msg);
    return;
  }
  if (!net.active()) return;
  if (net.phase() != netLastPhase) {  // (the Online Play page shows other rows)
    netLastPhase = net.phase();
    if (menu.open()) menu.refresh();
  }
  if (net.isClient()) {
    NetSync s;
    if (net.takeSync(&s)) netApplySync(s);
  } else if (inLauncher || !snes) {
    net.setPause(Netplay::kNoGame);
    netDirty = true;  // (a game will be started: a sync point then)
  }
  (void)dt;
}

// The game's frames while connected. False: not in lockstep now (the host's
// level select or game start running out of sight), the usual path runs them.
bool App::netGameFrames(double dt, int gameFrames, bool emulate) {
  if (net.isHost()) {
    if (game->levelLoading()) {
      net.setPause(Netplay::kNoGame);
      netDirty = true;
      return false;
    }
    if (!emulate) {
      net.setPause(Netplay::kMenu);
      netStall = 0;
      return true;
    }
    net.setPause(Netplay::kRunning);
    NetSettings want = netCurrentSettings();
    // (a new widescreen margin alone, the window being resized: once it has
    // settled, not at every step of the drag)
    NetSettings sameExt = want;
    sameExt.ext = netSet.ext;
    if (netHaveSet && want != netSet && sameExt == netSet) {
      netExtWait += dt;
      if (netExtWait < 0.5) want = netSet;
    } else {
      netExtWait = 0;
    }
    const bool need = net.phase() == Netplay::Phase::Connected || netDirty || net.hostWantsSync() ||
                      !netHaveSet || want != netSet;
    if (need) {
      if (!game->coopMidTurn()) {
        netHostSync();
        return true;
      }
      if (net.phase() == Netplay::Phase::Connected) {
        // (the frame ended in a co-op turn's middle, before the first sync
        // point: one more frame on its own first, as a save state waits for)
        snes->input[0] = padState[0];
        snes->input[1] = 0;
        game->beforeFrame();
        snes->runFrame();
        game->afterFrame();
        audioBuf.clear();
        snes->takeAudio(audioBuf);
        return true;
      }
      // (playing: the sync point after this pass's frames end the turn)
    }
  } else {
    if (!emulate) {
      net.setPause(Netplay::kMenu);
      netStall = 0;
      return true;
    }
    net.setPause(Netplay::kRunning);
  }
  if (net.phase() != Netplay::Phase::Playing) {
    netStall = 0;
    return true;
  }
  int runs = gameFrames > 0 ? std::clamp(gameFrames + net.timeAdjust(), 0, 4) : 0;
  int ran = 0;
  for (int i = 0; i < runs; i++) {
    net.pushLocal(padState[0]);
    uint16_t p1 = 0, p2 = 0;
    if (!net.inputs(&p1, &p2)) {
      // (the other side's controller isn't here yet: a moment's wait, then
      // this refresh shows the last picture)
      if (i == 0 && net.peerPause() == Netplay::kRunning) net.wait(6);
      if (!net.inputs(&p1, &p2)) break;
    }
    snes->input[0] = p1;
    snes->input[1] = p2;
    game->beforeFrame();
    snes->runFrame();
    game->afterFrame();
    if (game->coopPick >= 0) {  // (a title screen's co-op choice: the same on both sides)
      if (net.isHost()) {
        cfg.coop = game->coopPick != 0;
        cfg.save(configPath);
        netSet.flags = (uint8_t)((netSet.flags & ~kNetCoop) | (cfg.coop ? kNetCoop : 0));
      }
      game->coopPick = -1;
    }
    audioBuf.clear();
    snes->takeAudio(audioBuf);
    if (!(cfg.muteUnfocused && !focused)) audio.push(audioBuf.data(), audioBuf.size() / 2, Snes::kSampleRate);
    fpsFrames++;
    net.advance(net.needHash() ? Netplay::hash(snes->wram, sizeof(snes->wram)) : 0);
    if (pendingSave >= 0 && !game->coopMidTurn()) saveState(pendingSave);
    ran++;
  }
  // (time since a frame last ran: refreshes with no frame due, on a display
  // faster than the game, don't count as progress)
  netStall = ran > 0 ? 0 : netStall + dt;
  if (ran) achFrame();
  return true;
}

// Host: the console as it is now, sent as a new sync point. The host takes
// the same fresh start from it as the client will (the game layer's tracking
// rebuilt from the state), so the two begin identical.
void App::netHostSync() {
  netSet = netCurrentSettings();
  netHaveSet = true;
  netDirty = false;
  netExtWait = 0;
  netApplySettings(netSet);
  if (rewinding) endRewind();
  rewind.clear();
  std::vector<uint8_t> st = snes->saveState();
  if (snes->loadState(st)) game->onStateLoaded();
  const int delay = cfg.netDelay > 0 ? cfg.netDelay : net.autoDelay();
  net.hostSync(st, netSet, delay);
  logf("netplay: sync point %u, %zu byte state (%zu sent), delay %d, ping %d ms", net.epoch(), st.size(),
       net.syncBytes(), delay, net.rttMs());
}

// Client: the host's sync point into this console.
void App::netApplySync(NetSync& s) {
  if (!snes || !game) {
    net.stop("Player 2 has no ROM loaded");
    return;
  }
  if (!netSramForeign) {  // (this player's own progress saved first: the console takes the host's now)
    saveSram();
    netSramForeign = true;
  }
  pendingSave = -1;
  if (rewinding) endRewind();
  rewind.clear();
  // (settings first: the brick fix is written into the ROM, not the state)
  netSet = s.settings;
  netHaveSet = true;
  netApplySettings(netSet);
  if (!snes->loadState(s.state)) {
    net.stop("Player 2 couldn't load the host's game");
    return;
  }
  game->onStateLoaded();
  if (!cfg.achievements) game->achievements().counts = false;
  playing = (GameId)s.settings.game;
  if (inLauncher) {
    inLauncher = false;
    launcher.stopSound();
  }
  audio.flush();
  net.synced();
  logf("netplay: sync point %u loaded, delay %d", s.epoch, s.delay);
}

void App::netEnded(const std::string& why) {
  menu.notify(why.empty() ? std::string("Online session ended") : why, why.empty() ? 3.0f : 8.0f);
  logf("netplay: ended: %s", why.empty() ? "(here)" : why.c_str());
  netHaveSet = false;
  netDirty = false;
  netStall = 0;
  netLastPhase = Netplay::Phase::Off;
  if (netSramForeign) {
    // Player 2: the console holds the host's game and save files. Back to
    // the launcher, with this player's own save files.
    netSramForeign = false;
    if (snes) loadSram();
    if (!inLauncher && game) {
      game->cancelWarps();
      rewind.clear();
      menu.closeAll();
      launcher.selectGame(playing);
      inLauncher = true;
      audio.flush();
    }
  }
  applyConfig();  // (this player's own settings again)
  if (menu.open()) menu.refresh();
}

NetSettings App::netCurrentSettings() const {
  NetSettings s;
  s.ext = (uint8_t)std::clamp(netWantExt, 0, (int)SmasGame::kMaxSupportedExt);
  s.flags = (cfg.widescreenEnemies ? kNetEnemyRange : 0) | (cfg.hudAnchor ? kNetHudAnchor : 0) |
            (cfg.dimWrap ? kNetDimWrap : 0) | (cfg.coop ? kNetCoop : 0) | (cfg.nesBrickBounce ? kNetBrickBounce : 0) |
            (cfg.noSpriteLimit ? kNetNoSpriteLimit : 0);
  s.game = (uint8_t)playing;
  s.cheats = achLocked() ? 0 : cfg.cheats;
  return s;
}

void App::netApplySettings(const NetSettings& s) {
  if (!game || !snes) return;
  game->setWidescreen(s.ext);
  game->enemyRange = (s.flags & kNetEnemyRange) != 0;
  game->hudAnchor = (s.flags & kNetHudAnchor) != 0;
  game->dimWrap = (s.flags & kNetDimWrap) != 0;
  game->coop = (s.flags & kNetCoop) != 0;
  game->setNesBrickBounce((s.flags & kNetBrickBounce) != 0);
  game->setCheats(s.cheats);
  snes->ppu.noSpriteLimit = (s.flags & kNetNoSpriteLimit) != 0;
}

bool App::netBlocked(const char* what) {
  if (!net.isClient() || !net.connected()) return false;
  menu.notify(std::string("Only the host can ") + what + " in an online game", 3.5f);
  return true;
}

// ---------------------------------------------------------------------------
// Starting and the status line

Netplay::Identity App::netIdentity() const {
  Netplay::Identity id;
  id.version = kAppVersion;
  id.stateVersion = 0;
  if (snes) {  // (the save state's header: its magic and version)
    std::vector<uint8_t> st = snes->saveState();
    if (st.size() >= 8) id.stateVersion = (uint32_t)st[4] | (uint32_t)st[5] << 8 | (uint32_t)st[6] << 16 | (uint32_t)st[7] << 24;
  }
  // (the ROM without a copier header: a headered dump of the same game is the same game)
  const size_t off = (romData.size() & 0x3ff) == 0x200 ? 0x200 : 0;
  id.romCrc = romData.size() > off ? (uint32_t)Netplay::hash(romData.data() + off, romData.size() - off) : 0;
  id.msu = narrow(msuLoaded);
  return id;
}

void App::netHost(int port) {
  if (!snes) {
    menu.notify("Online play needs the ROM (see the game cards)", 4.0f);
    return;
  }
  net.setIdentity(netIdentity());
  std::string err;
  if (!net.startHost(port, &err)) {
    menu.notify("Couldn't host: " + err, 6.0f);
    return;
  }
  cfg.netPort = port;
  cfg.save(configPath);
  netDirty = true;
  netLastPhase = net.phase();
  std::string ips = net.localAddresses();
  menu.notify("Hosting on UDP port " + std::to_string(port) + (ips.empty() ? "" : " (this computer: " + ips + ")") +
                  " - waiting for player 2",
              6.0f);
  logf("netplay: hosting on port %d", port);
  if (menu.open()) menu.refresh();
}

void App::netJoin(const std::string& address) {
  if (!snes) {
    menu.notify("Online play needs the ROM (see the game cards)", 4.0f);
    return;
  }
  net.setIdentity(netIdentity());
  std::string err;
  if (!net.startJoin(address, &err)) {
    menu.notify("Couldn't join: " + err, 6.0f);
    return;
  }
  cfg.netAddress = address;
  cfg.save(configPath);
  netLastPhase = net.phase();
  menu.notify("Connecting to " + net.peerName() + "...", 3.0f);
  logf("netplay: joining %s", net.peerName().c_str());
  if (menu.open()) menu.refresh();
}

std::string App::netStatus() const {
  if (!net.active()) return std::string();
  const bool host = net.isHost();
  const char* other = host ? "player 2" : "the host";
  switch (net.phase()) {
    case Netplay::Phase::Waiting:
      return host ? "Online: waiting for player 2 (port " + std::to_string(net.port()) + ")"
                  : "Online: connecting to " + net.peerName() + "...";
    case Netplay::Phase::Connected:
      return host ? (inLauncher ? "Online: player 2 is here - start a game" : "Online: starting...")
                  : "Online: waiting for the host to start a game";
    case Netplay::Phase::Syncing: {
      const int pc = (int)(net.syncProgress() * 100 + 0.5f);
      return std::string("Online: syncing with ") + other + " " + std::to_string(pc) + "%";
    }
    case Netplay::Phase::Playing:
      if (inLauncher || menu.open()) return std::string();
      if (netStall < 0.25) return std::string();
      if (net.peerPause() == Netplay::kMenu) return std::string("Online: ") + (host ? "player 2" : "the host") + " paused";
      if (net.peerPause() == Netplay::kNoGame) return "Online: the host is choosing a game";
      return std::string("Online: waiting for ") + other + "...";
    default: return std::string();
  }
}

std::string App::netSummary() const {
  if (!net.active()) return "Not connected";
  const bool host = net.isHost();
  switch (net.phase()) {
    case Netplay::Phase::Waiting:
      return host ? "Hosting on port " + std::to_string(net.port()) + ", waiting" : "Connecting to " + net.peerName();
    default: break;
  }
  std::string s = (host ? "Player 2: " : "Host: ") + net.peerName();
  if (net.rttMs() >= 0) s += "  \xE2\x80\xA2  " + std::to_string(net.rttMs()) + " ms";
  if (net.phase() == Netplay::Phase::Playing) s += "  \xE2\x80\xA2  delay " + std::to_string(net.delay());
  return s;
}

// ---------------------------------------------------------------------------
// Menu pages

std::shared_ptr<MenuPage> App::pageOnline() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Online Play";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.subtitle = net.active() ? (net.isHost() ? "Hosting" : "Player 2") : "Two players over the internet";
    MenuItem st = MenuItem::info("Status", "Online co-op: the host is controller 1 (Mario), the other player "
                                           "controller 2 (Luigi). Both need this same version and ROM.");
    st.valueText = [this]() { return netSummary(); };
    pg.items.push_back(st);
    if (!net.active()) {
      pg.items.push_back(MenuItem::header("Start"));
      pg.items.push_back(MenuItem::action("Host a Game", [this]() { netHost(cfg.netPort); },
                                          "Wait for player 2 to join this computer. Forward UDP port " +
                                              std::to_string(cfg.netPort) +
                                              " on your router to this computer (or share a VPN such as Tailscale "
                                              "or ZeroTier), and give player 2 your address."));
      MenuItem join = MenuItem::action("Join a Game", [this]() {
        menu.push(pageTextEntry("Join a Game", "The host's address: an IP or name, with :port if not " +
                                                   std::to_string(Netplay::kDefaultPort) + ".",
                                cfg.netAddress, false, [this](const std::string& t) {
                                  menu.pop();
                                  netJoin(t);
                                }));
      }, "Connect to a player who is hosting. The host's game starts here for you, as player 2.");
      join.valueText = [this]() { return cfg.netAddress; };
      pg.items.push_back(join);
      MenuItem port = MenuItem::action("Host Port", [this]() {
        menu.push(pageTextEntry("Host Port", "The UDP port to host on (1-65535). Player 2 adds it to your address "
                                             "unless it is " + std::to_string(Netplay::kDefaultPort) + ".",
                                std::to_string(cfg.netPort), true, [this](const std::string& t) {
                                  const int v = atoi(t.c_str());
                                  if (v < 1 || v > 65535) {
                                    menu.notify("The port must be between 1 and 65535");
                                    return;
                                  }
                                  cfg.netPort = v;
                                  cfg.save(configPath);
                                  menu.pop();
                                  menu.refresh();
                                }));
      }, "The UDP port to host on (default " + std::to_string(Netplay::kDefaultPort) + ").");
      port.valueText = [this]() { return std::to_string(cfg.netPort); };
      pg.items.push_back(port);
    } else {
      if (net.isHost()) {
        const std::string ips = net.localAddresses();
        MenuItem a = MenuItem::info("This Computer", "Player 2 joins with one of these on your network or VPN; "
                                                     "over the internet, with your public IP (and the port "
                                                     "forwarded to this computer).");
        a.valueText = [ips, this]() { return (ips.empty() ? std::string("?") : ips) + "  port " + std::to_string(net.port()); };
        pg.items.push_back(a);
      }
      pg.items.push_back(MenuItem::action(net.isHost() ? "Stop Hosting" : "Leave", [this]() {
        net.stop(net.isHost() ? "The host ended the session" : "Player 2 left");
      }, net.isHost() ? "End the session: player 2 is told, and you play on alone."
                      : "Leave the session: back to the launcher, with your own save files."));
    }
    pg.items.push_back(MenuItem::header("Connection"));
    std::vector<std::string> opts = {"Auto"};
    for (int d = 1; d <= Netplay::kMaxDelay; d++) opts.push_back(std::to_string(d) + (d == 1 ? " frame" : " frames"));
    MenuItem dl = MenuItem::choice("Input Delay", opts, [this]() { return std::clamp(cfg.netDelay, 0, Netplay::kMaxDelay); },
                                   [this](int v) {
                                     cfg.netDelay = v;
                                     if (net.isHost() && net.connected()) netDirty = true;  // (in use from the next sync point)
                                   },
                                   "Frames between pressing a button and the game seeing it, so it reaches the other "
                                   "player in time. Auto picks it from the ping. Higher: smoother on a slow "
                                   "connection. The host's setting is used.");
    dl.valueText = [this]() {
      const int d = std::clamp(cfg.netDelay, 0, Netplay::kMaxDelay);
      std::string t = d == 0 ? "Auto" : std::to_string(d) + (d == 1 ? " frame" : " frames");
      if (d == 0 && net.connected() && net.rttMs() >= 0) t += " (" + std::to_string(net.autoDelay()) + ")";
      return t;
    };
    pg.items.push_back(dl);
  };
  p->onLeave = [this]() { cfg.save(configPath); };
  return p;
}

// A line of text to type (an address, a port): typed on a keyboard (or
// pasted), or spelt out with a pad: up and down change the character, left
// and right move, LB deletes.
std::shared_ptr<MenuPage> App::pageTextEntry(const std::string& title, const std::string& hint,
                                             const std::string& initial, bool digitsOnly,
                                             std::function<void(const std::string&)> done) {
  struct Edit {
    std::string text;
    int cursor = 0;
    bool pasteHeld = false, clearHeld = false;
    float blink = 0;
  };
  auto ed = std::make_shared<Edit>();
  ed->text = initial;
  ed->cursor = (int)initial.size();
  const std::string chars = digitsOnly ? "0123456789" : "0123456789.:abcdefghijklmnopqrstuvwxyz-_[]";
  auto allowed = [chars, digitsOnly](char c) {
    if (digitsOnly) return c >= '0' && c <= '9';
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == ':' ||
           c == '-' || c == '_' || c == '[' || c == ']' || c == '%';
  };
  auto p = std::make_shared<MenuPage>();
  p->title = title;
  p->subtitle = "Type, or paste with Ctrl+V";
  p->inputCustom = [this, ed, chars, allowed, done](Menu& m, const MenuInput& in) {
    const size_t maxLen = 80;
    for (char c : typedNow)
      if (allowed(c) && ed->text.size() < maxLen) ed->text.insert(ed->text.begin() + ed->cursor++, c);
    const bool ctrl = input.keyDown(VK_CONTROL) || input.keyDown(VK_LCONTROL) || input.keyDown(VK_RCONTROL);
    const bool paste = ctrl && input.keyDown('V');
    if (paste && !ed->pasteHeld) {
      for (char c : platformClipboardText())
        if (allowed(c) && ed->text.size() < maxLen) ed->text.insert(ed->text.begin() + ed->cursor++, c);
    }
    ed->pasteHeld = paste;
    const bool clear = input.keyDown(VK_DELETE);  // (Delete: the whole line, for another address)
    if (clear && !ed->clearHeld) {
      ed->text.clear();
      ed->cursor = 0;
    }
    ed->clearHeld = clear;
    if (in.back) {
      if (input.keyDown(VK_BACK)) {  // (Backspace: a character; Esc or the pad's back button: leave)
        if (ed->cursor > 0) ed->text.erase(ed->text.begin() + --ed->cursor);
      } else {
        m.pop();
      }
      return true;
    }
    if (in.confirm) {
      std::string t = ed->text;
      done(t);
      return true;
    }
    if (in.left) ed->cursor = std::max(0, ed->cursor - 1);
    if (in.right) {
      if (ed->cursor < (int)ed->text.size()) ed->cursor++;
      else if (!ed->text.empty() && ed->text.size() < maxLen) {  // (a pad spelling it out: one more)
        ed->text.push_back(chars[0]);
        ed->cursor++;
      }
    }
    // (Q and E are tab keys too: typed letters there, not LB / RB)
    const bool keyQE = input.keyDown('Q') || input.keyDown('E');
    if (in.tabLeft && !keyQE && ed->cursor > 0) ed->text.erase(ed->text.begin() + --ed->cursor);
    if ((in.up || in.down) && !typedNow.size()) {
      if (ed->cursor >= (int)ed->text.size()) {
        if (ed->text.size() < maxLen) ed->text.push_back(chars[0]);
        ed->cursor = (int)ed->text.size() - 1;
      }
      char& c = ed->text[ed->cursor];
      size_t at = chars.find((char)std::tolower((unsigned char)c));
      if (at == std::string::npos) at = 0;
      at = (at + (in.up ? 1 : chars.size() - 1)) % chars.size();
      c = chars[at];
    }
    return true;
  };
  p->drawCustom = [ed, hint](Menu& m, Canvas* cv, RectF r) {
    const float s = m.uiScale();
    ed->blink += 1.0f / 60.0f;
    const float pad = 18 * s;
    float y = r.top + pad;
    // (the panel's own text colours: Storybook's is light, the other styles' dark)
    const bool light = m.style == Menu::kStorybook;
    const ColorF ink = light ? colorF(0.22f, 0.17f, 0.12f) : colorF(0.88f, 0.9f, 0.95f);
    const ColorF dim = light ? colorF(0.45f, 0.38f, 0.30f) : colorF(0.6f, 0.64f, 0.75f);
    m.text(Menu::widen(hint), rectF(r.left + pad, y, r.right - pad, y + 60 * s), 20 * s, false, ink, kAlignLeading,
           kParaNear);
    y += 72 * s;
    const RectF box = rectF(r.left + pad, y, r.right - pad, y + 56 * s);
    cv->fillRound(box, 10 * s, colorF(0.03f, 0.035f, 0.06f, 0.92f));
    m.outlineRound(box, 10 * s, 2.5f * s, m.accent());
    const float size = 28 * s;
    const std::wstring before = Menu::widen(ed->text.substr(0, ed->cursor));
    const float tx = box.left + 16 * s;
    m.text(Menu::widen(ed->text), rectF(tx, box.top, box.right - 12 * s, box.bottom), size, true, colorF(1, 1, 1));
    if (std::fmod(ed->blink, 1.0f) < 0.6f) {
      const float cx = tx + m.measure(before, size, true) + 1.5f * s;
      cv->fillRect(rectF(cx, box.top + 11 * s, cx + 3 * s, box.bottom - 11 * s), m.accent());
    }
    y = box.bottom + 20 * s;
    m.text(L"Keyboard: Enter OK, Esc back, Backspace delete, Del clear, Ctrl+V paste", rectF(r.left + pad, y, r.right - pad, y + 26 * s),
           17 * s, false, dim, kAlignLeading, kParaNear);
    y += 28 * s;
    m.text(L"Pad: \u2191\u2193 change the character, \u2190\u2192 move, LB delete, A OK",
           rectF(r.left + pad, y, r.right - pad, y + 26 * s), 17 * s, false, dim, kAlignLeading, kParaNear);
  };
  return p;
}
