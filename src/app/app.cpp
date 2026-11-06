// Super Mario All-Stars port: the application, the same on every platform
// (see app.h).
#include "app/app.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include "app/achexport.h"
#include "app/msupack.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include "core/fileio.h"
#include "core/png.h"

const wchar_t* kAppTitle = L"Super Mario All-Stars";
const char* kAppVersion = "1.1.1";
namespace {
const char* kExpectedTitle = "SUPER MARIO ALL_STARS";
constexpr uint32_t kKnownCrc = 0x925637c7;  // Super Mario All-Stars (USA)
}  // namespace

App* g_app = nullptr;
FILE* g_log = nullptr;  // (opened by the platform)

void logf(const char* fmt, ...) {
  if (!g_log) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}

std::string narrow(const std::wstring& w) { return narrowUtf8(w); }

bool readAll(const std::wstring& path, std::vector<uint8_t>& out) { return readFile(path, out, 16u << 20); }

bool writeAll(const std::wstring& path, const void* d, size_t n) { return writeFileSafely(path, d, n); }

static uint32_t crc32(const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

// Validate that the data is the Super Mario All-Stars ROM (headered or not).
bool isSmasRom(const std::vector<uint8_t>& d, bool* exact) {
  size_t off = (d.size() & 0x3ff) == 0x200 ? 0x200 : 0;
  if (d.size() < off + 0x8000) return false;
  const uint8_t* h = d.data() + off + 0x7fc0;
  if (memcmp(h, kExpectedTitle, strlen(kExpectedTitle)) != 0) return false;
  if (exact) *exact = (d.size() - off == 0x200000) && crc32(d.data() + off, d.size() - off) == kKnownCrc;
  return true;
}

bool App::loadRom() {
  std::wstring path;
  // remembered location
  std::string rp;
  if (readText(pathJoin(exeDir, L"rompath.txt"), rp)) {
    while (!rp.empty() && (rp.back() == '\n' || rp.back() == '\r')) rp.pop_back();
    std::wstring w = widenUtf8(rp);
    std::vector<uint8_t> data;
    if (readAll(w, data) && isSmasRom(data, nullptr)) path = w;
  }
  // (Not found: the launcher's cards say what is needed, and it is looked for
  // again while they are shown.)
  if (path.empty() && !findRom(&path)) return false;
  std::vector<uint8_t> data;
  bool exact = false;
  if (!readAll(path, data) || !isSmasRom(data, &exact)) return false;
  // (Another version or dump than the USA one: it runs, the card's ROM line in
  // amber; widescreen, cheats and the level select are tuned for the USA one.)
  const std::string remembered = narrow(path) + "\n";
  writeAll(pathJoin(exeDir, L"rompath.txt"), remembered.data(), remembered.size());
  romPath = path;
  romExact = exact;
  game.reset();  // (before the Snes it points at: its destructor uses it)
  snes = std::make_unique<Snes>();
  // (the console's copy patched for an MSU-1 pack if one is chosen; the
  // launcher's previews run the ROM as it is)
  std::vector<uint8_t> run = data;
  applyMsu(run);
  if (!snes->loadRom(run)) {
    snes.reset();
    return false;
  }
  romData = std::move(data);
  game = std::make_unique<SmasGame>(*snes);
  game->install();
  snes->reset(true);
  loadSram();
  return true;
}

// The chosen MSU-1 pack (see app/msupack.h): its patch applied to the
// console's ROM and the chip attached, or why not (msuStatus).
void App::applyMsu(std::vector<uint8_t>& rom) {
  if (msuDir.empty()) msuDir = pathJoin(exeDir, L"msu");
  makeDir(msuDir);
  msuLoaded = cfg.msuPack;
  msuStatus.clear();
  msuBase.clear();
  if (cfg.msuPack.empty()) return;
  MsuPack pk;
  std::string err;
  std::vector<uint8_t> patch;
  if (findMsuPack(msuDir, cfg.msuPack, pk, &err)) {
    if (!readFile(pk.patch, patch, 16u << 20)) {
      err = "its patch can't be read";
    } else {
      std::vector<uint8_t> patched = rom;
      if ((patched.size() & 0x3ff) == 0x200) patched.erase(patched.begin(), patched.begin() + 0x200);
      if (applyRomPatch(patch, patched, &err)) {
        rom.swap(patched);
        snes->msu = std::make_unique<Msu1>();
        snes->msu->open(pk.base);
        snes->msu->setGain(cfg.msuVolume / 100.0f * cfg.musicVolume / 100.0f);
        msuBase = pk.base;
        msuStatus = "Playing " + narrow(cfg.msuPack) + ": " + std::to_string(pk.tracks) + " tracks, patched with " +
                    narrow(pathName(pk.patch)) + ".";
        logf("MSU-1 pack %s: %d tracks, patch %s", narrow(cfg.msuPack).c_str(), pk.tracks,
             narrow(pk.patch).c_str());
        return;
      }
    }
  }
  msuStatus = "Not used: " + err + ".";
  logf("MSU-1 pack %s not used: %s", narrow(cfg.msuPack).c_str(), err.c_str());
}

// Another pack chosen: the console is rebuilt with it (outside a game: the
// launcher shows). Save files are kept; a game's save states belong with the
// pack they were made with.
void App::reloadForMsu() {
  if (!snes || cfg.msuPack == msuLoaded) return;
  saveSram();
  if (loadRom()) romReady();
}

// Updates: once a day (or on Check Now) the latest release's tag is looked up
// in the background; newer than this version, the launcher shows a badge
// (and Options a "Get Version" button) that opens its page.
void App::checkForUpdate(bool manual) {
  if (test.on || test.runSeconds > 0) return;
#ifdef _WIN32
  if (at.on) {  // (the recording mode: no network, only what an earlier check found)
    applyUpdateTag(cfg.checkUpdates ? cfg.updateLatest : std::string());
    return;
  }
#endif
  if (!manual && !cfg.checkUpdates) return;
  const std::string today = timeText("%Y-%m-%d");
  if (!manual && cfg.updateCheckedOn == today) {
    applyUpdateTag(cfg.updateLatest);  // (checked today already)
    return;
  }
  updateManual = manual;
  updater.start();
}

void App::applyUpdateTag(const std::string& tag) {
  const bool newer = !tag.empty() && UpdateCheck::isNewer(tag, kAppVersion);
  updateTag = newer && (cfg.checkUpdates || updateManual) ? tag : std::string();
  launcher.setUpdate(updateTag);
  if (!updateTag.empty() && !updateTold) {
    updateTold = true;
    menu.notify("Version " + updateVersion() + " is available - select the badge, or Options > Get Version " +
                    updateVersion(),
                6.0f);
  }
}

void App::updateTick() {
  if (!updateStarted) {
    updateStarted = true;
    checkForUpdate(false);
  }
  std::string tag;
  if (!updater.poll(&tag)) return;
  const bool manual = updateManual;
  if (!tag.empty()) {
    cfg.updateCheckedOn = timeText("%Y-%m-%d");
    cfg.updateLatest = tag;
    if (!test.on && test.runSeconds <= 0) cfg.save(configPath);
    if (manual) updateTold = false;
    applyUpdateTag(tag);
    if (manual && updateTag.empty()) menu.notify(std::string("You have the latest version (") + kAppVersion + ")", 4.0f);
  } else if (manual) {
    menu.notify("Couldn't reach GitHub to check for updates", 4.0f);
  }
  updateManual = false;
  if (menu.open()) menu.refresh();
}

// After the ROM is loaded (at start, or found later while the launcher shows).
void App::romReady() {
  music.init(snes.get(), game.get(), &audio);
  music.setMsu(msuBase, narrow(msuLoaded), cfg.msuVolume / 100.0f);  // (the Music Player plays the pack too)
  achNewRun(cfg.achievements);
  applyConfig();
  if (!test.on && test.runSeconds <= 0)
    launcher.setRom(&romData, narrow(romPath.substr(romPath.find_last_of(L"\\/") + 1)), romExact, snes->sram,
                    sizeof(snes->sram));
  if (!romExact) logf("ROM is not the USA dump (CRC differs): running it anyway");
}

void App::pollRom(double dt) {
  if (snes || (romPoll -= dt) > 0) return;
  romPoll = 1.0;
  if (!loadRom()) return;
  logf("rom found: %s", narrow(romPath).c_str());
  romReady();
  menu.notify("Super Mario All-Stars ROM found");
}

// A card chosen (or Reset Game): the console is reset and driven through the
// All-Stars menu to the game's save files, out of sight (see
// SmasGame::requestTitle), as a new run for achievements.
void App::launchGame(GameId g) {
  if (netBlocked("start a game")) return;
  netDirty = true;   // (online: player 2 gets it once it's running)
  pendingSave = -1;  // (a save waiting for a co-op turn's end: not into another game)
  if (!snes) {
#ifdef _WIN32
    menu.notify("ROM not found - see the game cards");
#else
    pickRom();
#endif
    return;
  }
  reloadForMsu();  // (a pack chosen in a game: in use from this start)
  if (!snes) return;
  playing = g;
  for (int i = 0; i < Launcher::kCards; i++)
    if (Launcher::cardGame(i) == g) cfg.lastGame = i;
  launcher.selectGame(g);
  launcher.stopSound();
  inLauncher = false;
  menu.closeAll();
  if (rewinding) endRewind();
  if (g == GameId::Menu) {
    // (the All-Stars card: the cartridge as at power-on, its title and game select)
    saveSram();
    game->cancelWarps();
    snes->reset(false);
  } else {
    game->requestTitle(g, false);
  }
  rewind.clear();
  audio.flush();
  achNewRun(cfg.achievements);
  applyConfig();
  if (!test.on && test.runSeconds <= 0) cfg.save(configPath);
#ifdef _WIN32
  if (at.on) menuHintShown = true;  // (the recording mode: no hint over the footage)
#endif
  if (!menuHintShown && !test.on) {
    menuHintShown = true;
    menu.notify(cfg.selectMode == 0 ? "Press Select or Esc for the menu" : "Hold Select or press Esc for the menu", 4.0f,
                true);
  }
}

void App::returnToLauncher() {
  if (net.isClient()) {  // (player 2: leaving the host's game is leaving the session; see netEnded)
    net.stop("Player 2 left");
    return;
  }
  netDirty = true;
  pendingSave = -1;
  if (rewinding) endRewind();
  saveSram();
  game->cancelWarps();
  rewind.clear();
  audio.flush();
  menu.closeAll();
  launcher.selectGame(playing);
  inLauncher = true;
  reloadForMsu();
}

// A display refresh on the launcher: its cards and buttons (or a menu page
// over it), the previews' frames.
void App::frameLauncher(double dt, int gameFrames) {
  pollRom(dt);
  const bool menuOpen = menu.open();
  MenuInput mi = menuInput();
  if (menuOpen) {
    if (menu.top() && !menu.capturing) menu.update(mi, (float)dt);
    menuAnim = std::min(1.0f, menuAnim + (float)dt * 8.0f);
  } else {
    auto push = [&](std::shared_ptr<MenuPage> p) {
      menuAnim = 0;
      menu.push(p);
    };
    Launcher::Action act = launcher.update(mi, (float)dt);
    if (act == Launcher::kNone) act = launcherTap;  // (a tap, or a click)
    launcherTap = Launcher::kNone;
    switch (act) {
      case Launcher::kPlay: launchGame(Launcher::cardGame(launcher.card())); return;
      case Launcher::kOptions: push(pageOptions()); break;
      case Launcher::kAchievements: push(pageAchievements()); break;
      case Launcher::kMusic:
        if (snes) push(pageMusicPlayer());
        else menu.notify("ROM not found - see the game cards");
        break;
      case Launcher::kCredits: push(pageCredits()); break;
      case Launcher::kUpdate:
        if (!updateTag.empty()) {
          openUrl(UpdateCheck::pageFor(updateTag).c_str());
          menu.notify("Opened the version " + updateVersion() + " download page in your browser");
        }
        break;
      case Launcher::kPatreon:
        openUrl("https://www.patreon.com/TeamRedTail");
        menu.notify("Opened Patreon in your browser");
        break;
      case Launcher::kQuit:
        if (mi.back) {
          menuAnim = 0;
          menu.confirm("Quit?", [this]() { running = false; });
        } else {
          running = false;
        }
        break;
      default: break;
    }
  }
  if (menuOpen && !menu.open()) cfg.save(configPath);
  if (snes) music.update(dt, menu.open());
  // (the chosen card's sound, not under a menu page: the music player's there)
  launcher.runPreviews(gameFrames, !menu.open() && !(cfg.muteUnfocused && !focused), audio, pacing.hz);
}

// Black frame insertion's state, beside its switch.
int App::hfrSubFrames() const {
  if (!cfg.highFrameRate || cfg.blackFrames || pacing.hz < 89) return 1;
  return std::clamp((int)std::lround(pacing.hz / Snes::kFrameRate + 0.25), 2, Ppu::kMaxSubFrames);
}

std::string App::blackFramesNote() const {
  if (!cfg.blackFrames) return "";
  if (!cfg.vsync) return "needs VSync";
  char buf[48];
  if (pacing.perFrame >= 2) snprintf(buf, sizeof buf, "%.0f Hz", pacing.hz);
  else snprintf(buf, sizeof buf, "needs 120 Hz (now %.0f)", pacing.hz);
  return buf;
}

void App::loadSram() {
  std::vector<uint8_t> d;
  if (readAll(pathJoin(saveDir, L"smas.srm"), d) && d.size() >= sizeof(snes->sram)) {
    memcpy(snes->sram, d.data(), sizeof(snes->sram));
  }
  snes->sramDirty = false;
}

void App::saveSram() {
  if (!snes || netSramForeign) return;  // (player 2 online: the console holds the host's save files)
  if (writeAll(pathJoin(saveDir, L"smas.srm"), snes->sram, sizeof(snes->sram))) {
    snes->sramDirty = false;
    sramFailed = false;
  } else if (!sramFailed) {  // (still dirty: tried again later)
    sramFailed = true;
    logf("saving game progress failed");
    menu.notify("Could not save game progress (is the disk full?)");
  }
}

std::wstring App::statePath(int slot) const { return pathJoin(saveDir, L"state" + std::to_wstring(slot) + L".sst"); }

// A rewind snapshot back in the machine. Battery saves are kept as they are
// now: rewinding never takes back progress the game has saved.
void App::restoreSnapshot(const std::vector<uint8_t>& st) {
  pendingSave = -1;
  std::vector<uint8_t> keep(snes->sram, snes->sram + sizeof(snes->sram));
  bool dirty = snes->sramDirty;
  if (!snes->loadState(st)) return;
  std::memcpy(snes->sram, keep.data(), keep.size());
  snes->sramDirty = dirty;
  game->onStateLoaded();
}

// While Rewind is held: `steps` snapshots back (each two frames, so twice the
// game's speed), the oldest staying on screen. Each is drawn by running a
// frame from it, silently; letting go returns to the last one shown (see
// endRewind). False (play on) when there is nothing to go back to.
bool App::stepRewind(int steps) {
  if (!rewinding && rewind.snapshots() < 2) return false;
  if (steps <= 0) return true;
  std::vector<uint8_t> st;
  bool moved = false;
  for (int i = 0; i < steps && rewind.stepBack(&st); i++) moved = true;
  if (!moved) {
    if (rewinding) return true;  // (at the oldest: stay there)
    return false;
  }
  if (!rewinding) audio.flush();
  rewinding = true;
  restoreSnapshot(st);
  game->beforeFrame();
  snes->runFrame();
  game->afterFrame();
  audioBuf.clear();
  snes->takeAudio(audioBuf);  // (not played)
  fpsFrames++;
  return true;
}

void App::endRewind() {
  rewinding = false;
  if (!rewind.newest().empty()) restoreSnapshot(rewind.newest());
  audio.flush();
}

void App::saveState(int slot) {
  // (In a co-op turn's middle the other player's variables are the game's:
  // saved there, the game's player came back as him. At the next frame's end.)
  if (game->coopMidTurn()) {
    pendingSave = slot;
    return;
  }
  pendingSave = -1;
  auto st = snes->saveState();
  if (writeAll(statePath(slot), st.data(), st.size())) menu.notify("State " + std::to_string(slot) + " saved");
  else menu.notify("Could not save state " + std::to_string(slot) + " (is the disk full?)");
}

void App::loadState(int slot) {
  if (netBlocked("load states")) return;
  pendingSave = -1;  // (the timeline it was asked in is gone)
  std::vector<uint8_t> d;
  if (!readAll(statePath(slot), d)) {
    menu.notify("State " + std::to_string(slot) + " is empty");
    return;
  }
  if (snes->loadState(d)) {
    game->onStateLoaded();
    netDirty = true;  // (online: player 2 gets it as the game resumes)
    rewind.clear();  // (the past belongs to another timeline now)
    // The state says whether its run counted for achievements.
    AchievementTracker& t = game->achievements();
    if (!cfg.achievements) t.counts = false;
    menu.notify("State " + std::to_string(slot) + " loaded");
    if (cfg.achievements && !t.counts)
      menu.notify("This state was saved while achievements weren't counting: they count again after "
                  "Reset Game", 5.0f);
    audio.flush();
  } else {
    menu.notify("State " + std::to_string(slot) + " is not compatible");
  }
}

void App::screenshot() {
  makeDir(shotDir);
  const std::wstring name = widenUtf8(timeText("smas_%Y%m%d_%H%M%S.png"));
  int eL = snes->ppu.extL, eR = snes->ppu.extR;
  bool ok = writePng(narrow(pathJoin(shotDir, name)), snes->ppu.frame + (kMaxExt - eL), 256 + eL + eR,
                     snes->ppu.frameHeight(), kLineW);
  menu.notify(ok ? "Screenshot saved" : "Screenshot failed");
}

// ---------------------------------------------------------------------------
// Display


void App::applyLauncherArt(bool reload) {
  launcher.setCoverFill(cfg.launcherCoverFill);
  launcher.setArt(pathJoin(exeDir, L"covers"), cfg.launcherCovers, cfg.launcherBackground, reload);
}

void App::applyConfig() {
  applyLauncherArt();
  audio.setVolume(cfg.volume / 100.0f);
  audio.setLatency(cfg.audioLatency);
  menu.confirmRight = cfg.confirmRight;
  menu.style = cfg.menuStyle;
  launcher.setConfirmRight(cfg.confirmRight);
  if (snes) snes->ppu.noSpriteLimit = cfg.noSpriteLimit;
  if (snes) {
    snes->apu.dsp.musicGain = cfg.musicVolume * 256 / 100;
    snes->apu.dsp.sfxGain = cfg.sfxVolume * 256 / 100;
  }
  launcher.setGains(cfg.musicVolume * 256 / 100, cfg.sfxVolume * 256 / 100);
  // (a music pack's songs are music too: its own volume, times the music's)
  if (snes && snes->msu) snes->msu->setGain(cfg.msuVolume / 100.0f * cfg.musicVolume / 100.0f);
  music.setMsuGain(cfg.msuVolume / 100.0f);
  if (game) {
    game->hudAnchor = cfg.hudAnchor;
    game->dimWrap = cfg.dimWrap;
    game->enemyRange = cfg.widescreenEnemies != 0;
    game->setNesBrickBounce(cfg.nesBrickBounce);
    game->coop = cfg.coop;
    game->setCheats(achLocked() ? 0 : cfg.cheats);  // (achievements on: no cheats)
    game->playerPalette = cfg.playerPalette;
    game->luigiPalette = cfg.luigiPalette;
  }
  // (online: the session's settings over this player's, until it ends; the
  // host's own changes go out at the next sync point)
  if (net.connected() && netHaveSet) netApplySettings(netSet);
}

// Decide the widescreen margin and the destination rectangle.
void App::computeLayout(IRect* dest, int* extOut, int* srcW) {
  int W = video.width(), H = video.height();
  double par = cfg.pixelAspectCrt ? 8.0 / 7.0 : 1.0;
  double target;
  switch (cfg.aspect) {
    case kAspect4x3: target = 0; break;
    case kAspect16x10: target = 16.0 / 10.0; break;
    case kAspect16x9: target = 16.0 / 9.0; break;
    case kAspect21x9: target = 21.0 / 9.0; break;
    default: target = (double)W / std::max(H, 1); break;
  }
  int wantExt = 0;
  if (target > 0) {
    double needW = 224.0 * target / par;
    wantExt = (int)std::floor((needW - 256.0) / 2.0);
    wantExt = std::clamp(wantExt, 0, SmasGame::kMaxSupportedExt);
  }
  // A minimised window has a 1x1 client area: keep the last layout, or the
  // game would switch to 4:3 (and its widescreen patches off) meanwhile.
#ifdef _WIN32
  if (IsIconic(hwnd)) wantExt = lastWantExt;
#endif
  if (W < 16 || H < 16) wantExt = lastWantExt;
  lastWantExt = wantExt;
  netWantExt = wantExt;
  // (online: the session's margin, the host's display's at the last sync
  // point; the console's view decides which enemies are active)
  game->setWidescreen(net.connected() && netHaveSet ? netSet.ext : wantExt);
  int extL = game->activeExtL(), extR = game->activeExtR();
  *extOut = extL;
  int sw = 256 + extL + extR;
  *srcW = sw;
  int sh = snes->ppu.frameHeight();
  double imgW = sw * par, imgH = sh;
  // (The scale is the full wide picture's whatever is shown: a screen the
  // game shows 4:3 in a widescreen session (SMB2's open vertical areas, the
  // menus) keeps the size of the rest, beside it the pillarbox; it was drawn
  // a fifth bigger on a 16:9 display, the HUD jumping, as the view switched.)
  int fullSw = std::max(sw, 256 + 2 * wantExt);
  double dw, dh;
  if (cfg.scaling == 2) {  // stretch: fill the target aspect area
    double fullW = (target > 0 ? 224.0 * target : 256.0 * par);
    double s = std::min(W / fullW, H / imgH);
    dh = imgH * s;
    dw = imgW * s;
    if (extL + extR >= 2 * wantExt || target <= 0) {
      dw = std::min((double)W, fullW * s);
    }
  } else if (cfg.scaling == 1) {  // integer: whole multiples on both axes
    // (CRT pixels: the nearest whole width to 8/7 of the height)
    int s = std::max(1, (int)std::floor(H / imgH)), hs;
    for (;; s--) {
      hs = std::max(1, (int)std::lround(s * par));
      if (fullSw * hs <= W || s == 1) break;
    }
    dw = (double)sw * hs;
    dh = imgH * s;
  } else {
    double s = std::min(W / (fullSw * par), H / imgH);
    dw = imgW * s;
    dh = imgH * s;
    // The margins are whole pixels: fill the last bit of a display the
    // chosen aspect matches (a stretch of under two pixels).
    if (target > 0 && extL + extR >= 2 * wantExt && dw < W && W - dw < 2 * par * s + 1) dw = W;
  }
  int x0 = (int)std::lround((W - dw) / 2), y0 = (int)std::lround((H - dh) / 2);
  *dest = IRect{x0, y0, x0 + (int)std::lround(dw), y0 + (int)std::lround(dh)};
}

// ---------------------------------------------------------------------------
// Input

void App::readPlayer(int p, uint16_t* out, double dt) {
  float dz = cfg.deadzone / 100.0f;
  int padIdx = cfg.padIndex[p];
  bool act[kActCount];
  for (int a = 0; a < kActCount; a++) {
    act[a] = input.active(cfg.keys[p][a], padIdx, dz) || input.active(cfg.pad[p][a], padIdx, dz);
  }
  // Left stick always drives the D-pad
  if (const PadState* ps = input.pad(padIdx)) {
    act[kActLeft] |= ps->axes[0] < -dz;
    act[kActRight] |= ps->axes[0] > dz;
    act[kActUp] |= ps->axes[1] < -dz;
    act[kActDown] |= ps->axes[1] > dz;
  }
  // Select and the menu.
  // * The Menu bindings (Esc; the pad's View/Select by default) open the menu.
  // * selectMode 0: Select opens the menu too. The game's own Select (used by
  //   All-Stars' pause -> save menu) is on the left stick button, or
  //   Menu -> "Press Select In Game".
  // * selectMode 1: tap Select = game Select, hold Select = menu.
  auto isSelectBinding = [&](const Binding& b) { return b == cfg.keys[p][kActSelect] || b == cfg.pad[p][kActSelect]; };
  bool select = input.active(cfg.keys[p][kActSelect], padIdx, dz) || input.active(cfg.pad[p][kActSelect], padIdx, dz);
  bool menuBtn = false;
  for (const Binding* b : {&cfg.keys[p][kActMenu], &cfg.pad[p][kActMenu]})
    if (b->type != Binding::None && !isSelectBinding(*b) && input.active(*b, padIdx, dz)) menuBtn = true;
  bool menuPressed = menuBtn && !prevMenuBtn[p];
  bool selectOut = false;
  if (p != 0) {
    selectOut = select;  // player 2 has no menu button
  } else if (cfg.selectMode == 0) {
    if (select && !prevSelect[p]) menuPressed = true;
    bool lsFree = true;
    for (int a = 0; a < kActCount; a++)
      if (cfg.pad[p][a].type == Binding::PadButton && cfg.pad[p][a].code == 8) lsFree = false;
    const PadState* ps = input.pad(padIdx);
    if (lsFree && ps && ps->buttons[8]) selectOut = true;  // left stick press
  } else {
    if (select) {
      if (menu.open()) selectConsumed[p] = true;  // a press in the menu (Back) isn't a game tap
      selectHeld[p] += (float)dt;
      if (selectHeld[p] >= 0.4f && !selectConsumed[p]) {
        selectConsumed[p] = true;
        menuPressed = true;
      }
    } else {
      if (selectHeld[p] > 0 && !selectConsumed[p]) selectPulse[p] = 0.1f;  // tap -> game Select
      selectHeld[p] = 0;
      selectConsumed[p] = false;
    }
  }
  prevSelect[p] = select;
  prevMenuBtn[p] = menuBtn;
  if (selectPulse[p] > 0) {
    selectOut = true;
    selectPulse[p] -= (float)dt;
  }

  uint16_t v = 0;
  if (act[kActUp]) v |= kPadUp;
  if (act[kActDown]) v |= kPadDown;
  if (act[kActLeft]) v |= kPadLeft;
  if (act[kActRight]) v |= kPadRight;
  if (act[kActA]) v |= kPadA;
  if (act[kActB]) v |= kPadB;
  if (act[kActX]) v |= kPadX;
  if (act[kActY]) v |= kPadY;
  if (act[kActL]) v |= kPadL;
  if (act[kActR]) v |= kPadR;
  if (act[kActStart]) v |= kPadStart;
  if (selectOut) v |= kPadSelect;
  // no opposing directions
  if ((v & kPadUp) && (v & kPadDown)) v &= ~(kPadUp | kPadDown);
  if ((v & kPadLeft) && (v & kPadRight)) v &= ~(kPadLeft | kPadRight);
  *out = v;

  if (p == 0) {
    if (menuPressed && !menu.open() && !inLauncher) {
      logf("menu opened by %s (select key=%d pad=%d, menu button=%d, pad %d)",
           select ? "Select" : "Menu button", (int)input.active(cfg.keys[p][kActSelect], padIdx, dz),
           (int)input.active(cfg.pad[p][kActSelect], padIdx, dz), (int)menuBtn, padIdx);
      openMenu();
    }
    auto edge = [&](int a) { return act[a] && !prevAction[p][a]; };
    if (!menu.open() && !inLauncher) {
      if (edge(kActSaveState)) saveState(stateSlot);
      if (edge(kActLoadState)) loadState(stateSlot);
      if (edge(kActScreenshot)) screenshot();
    }
#ifndef __ANDROID__
    if (!menu.open()) {
      if (edge(kActFullscreen)) {
        cfg.displayMode = cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
        applyDisplayMode();
      }
    }
#endif
    // (Not while the level select drives the game to the chosen level.)
    const bool inGame = !inLauncher && game && !game->levelLoading();
#ifndef _WIN32
    act[kActRewind] |= touch.held(TouchControls::BtnRewind);
    act[kActFastForward] |= touch.held(TouchControls::BtnFF);
#endif
    // (online: neither; the other player's game runs with this one)
    rewindHeld = cfg.rewind && !achLocked() && act[kActRewind] && !menu.open() && inGame && !net.active();
    fastForward = cfg.fastForward && !achLocked() && act[kActFastForward] && !menu.open() && !rewindHeld && inGame &&
                  !net.active();
    // (pressed while achievements keep them off: say so, or they just seemed broken)
    if (achLocked() && !menu.open() && inGame && (edge(kActRewind) || edge(kActFastForward)))
      menu.notify("Fast forward and rewind are off while achievements are on (Options > Achievements)", 3.5f);
  }
  for (int a = 0; a < kActCount; a++) prevAction[p][a] = act[a];
}

MenuInput App::menuInput() {
  MenuInput m;
  float dz = 0.5f;
  bool nav[8] = {};
  nav[0] = input.keyDown(VK_UP);
  nav[1] = input.keyDown(VK_DOWN);
  nav[2] = input.keyDown(VK_LEFT);
  nav[3] = input.keyDown(VK_RIGHT);
  nav[4] = input.keyDown(VK_RETURN) || input.keyDown(VK_SPACE);
  nav[5] = input.keyDown(VK_ESCAPE) || input.keyDown(VK_BACK);
  // When Select opens the menu, Select also backs out of it.
  if (cfg.selectMode == 0)
    nav[5] |= input.active(cfg.keys[0][kActSelect], cfg.padIndex[0], 0.5f) ||
              input.active(cfg.pad[0][kActSelect], cfg.padIndex[0], 0.5f);
  nav[6] = input.keyDown(VK_PRIOR) || input.keyDown('Q');
  nav[7] = input.keyDown(VK_NEXT) || input.keyDown('E');
  for (int i = 0; i < input.padCount(); i++) {
    const PadState* p = input.pad(i);
    if (!p) continue;
    nav[0] |= p->buttons[10] || p->axes[1] < -dz;
    nav[1] |= p->buttons[11] || p->axes[1] > dz;
    nav[2] |= p->buttons[12] || p->axes[0] < -dz;
    nav[3] |= p->buttons[13] || p->axes[0] > dz;
    // (face buttons by position: 0 bottom, 1 right; Menu Buttons picks which confirms)
    const int ok = cfg.confirmRight ? 1 : 0;
    nav[4] |= p->buttons[ok] || p->buttons[7];
    nav[5] |= p->buttons[1 - ok] || p->buttons[6];
    nav[6] |= p->buttons[4];
    nav[7] |= p->buttons[5];
  }
  // directions are level-triggered (menu handles repeat); buttons are edges
  m.up = nav[0];
  m.down = nav[1];
  m.left = nav[2];
  m.right = nav[3];
  m.confirm = nav[4] && !prevMenuNav[4];
  m.back = nav[5] && !prevMenuNav[5];
  m.tabLeft = nav[6] && !prevMenuNav[6];
  m.tabRight = nav[7] && !prevMenuNav[7];
  for (int i = 0; i < 8; i++) prevMenuNav[i] = nav[i];
  return m;
}

void App::openMenu() {
  if (menu.open()) return;
  menu.push(pageMain());
  menuAnim = 0;
  // swallow the button that opened the menu
  menuInput();
  prevMenuNav[4] = prevMenuNav[5] = true;
}

void App::updateInput(double dt) {
#ifdef _WIN32
  if (!at.on) input.poll();  // (the recording mode: the script's keys and pads only)
#else
  input.poll();
#endif
  for (int p = 0; p < 2; p++) readPlayer(p, &padState[p], dt);
#ifndef _WIN32
  touchFrame();
  padState[0] |= touch.takeSnesButtons();
#endif
  if (!snes) return;
  snes->input[0] = menu.open() || inLauncher ? 0 : padState[0];
  snes->input[1] = menu.open() || inLauncher ? 0 : padState[1];
}

// ---------------------------------------------------------------------------
// Rebinding

void App::startBind(int player, bool pad, int action) {
  bindPlayer = player;
  bindPad = pad;
  bindAction = action;
  bindTimer = 6.0;
  menu.capturing = true;
  input.resetCapture();
}

void App::updateBind(double dt) {
  if (bindAction < 0) return;
  bindTimer -= dt;
  char buf[160];
  snprintf(buf, sizeof buf, "Press a %s for \"%s\"  (Esc to cancel, Delete to clear)  %d",
           bindPad ? "controller button" : "key", actionName(bindAction), (int)std::ceil(bindTimer));
  menu.captureText = buf;
  bool cancel = bindTimer <= 0;
  Binding b;
  if (input.keyDown(VK_ESCAPE)) cancel = true;
  else if (input.keyDown(VK_DELETE)) {
    (bindPad ? cfg.pad : cfg.keys)[bindPlayer][bindAction] = Binding();
    cancel = true;
  } else if (input.capture(&b, bindPad, cfg.padIndex[bindPlayer], cfg.deadzone / 100.0f)) {
    (bindPad ? cfg.pad : cfg.keys)[bindPlayer][bindAction] = b;
    cancel = true;
  }
  if (cancel) {
    bindAction = -1;
    menu.capturing = false;
    prevMenuNav[4] = prevMenuNav[5] = true;  // don't let the key also act in the menu
    menu.refresh();
    cfg.save(configPath);
  }
}

// ---------------------------------------------------------------------------
// Menu pages

std::shared_ptr<MenuPage> App::pageMain() {
  auto p = std::make_shared<MenuPage>();
  p->rebuild = [this](MenuPage& pg) {
    GameId g = game->current();
    // (on the All-Stars file panel the launcher went to: the game being started)
    const bool inTitle = g == GameId::SMB1 || g == GameId::SMBLL || g == GameId::SMB2 || g == GameId::SMB3;
    pg.title = inTitle ? SmasGame::gameName(g)
               : playing == GameId::Menu ? "Super Mario All-Stars"
                                         : SmasGame::gameName(playing);
    pg.subtitle = "Paused";
    pg.items.clear();
    pg.items.push_back(MenuItem::action("Resume", [this]() { menu.closeAll(); }, "Return to the game."));
    pg.items.push_back(MenuItem::action("Press Select In Game", [this]() {
      menu.closeAll();
      selectPulse[0] = 0.12f;
    }, "Send the controller's Select button to the game, e.g. while paused to open the All-Stars save menu."));

    pg.items.push_back(MenuItem::header("Save states"));
    MenuItem slot = MenuItem::choice(
        "Slot", {"1", "2", "3", "4", "5", "6", "7", "8", "9"}, [this]() { return stateSlot - 1; },
        [this](int v) { stateSlot = v + 1; },
        "Choose the save-state slot (" + bindingsText(kActSaveState) + " saves, " + bindingsText(kActLoadState) +
            " loads in game).");
    slot.valueText = [this]() {
      // the slot and when it was saved (or that it's empty)
      std::string t = std::to_string(stateSlot) + "  \xE2\x80\xA2  ";
      int day, month, hour, minute;
      if (!fileModified(statePath(stateSlot), &day, &month, &hour, &minute)) return t + "Empty";
      static const char* kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      char buf[48];
      snprintf(buf, sizeof buf, "%d %s, %02d:%02d", day, kMon[(month + 11) % 12], hour, minute);
      return t + buf;
    };
    pg.items.push_back(slot);
    pg.items.push_back(MenuItem::action("Save State", [this]() {
      saveState(stateSlot);
      menu.closeAll();
    }, "Save a snapshot of the game to the selected slot."));
    pg.items.push_back(MenuItem::action("Load State", [this]() {
      if (!fileExists(statePath(stateSlot))) {
        menu.notify("Slot " + std::to_string(stateSlot) + " is empty");
        return;
      }
      loadState(stateSlot);
      menu.closeAll();
    }, "Restore the snapshot in the selected slot."));

    pg.items.push_back(MenuItem::header("Game"));
    // with achievements on, the level select and cheats are greyed out (Options turns achievements off)
    auto locked = [this]() { return std::string(achLocked() ? "Off (achievements on)" : ""); };
    MenuItem ls = MenuItem::submenu("Level Select", [this]() { menu.push(pageLevelSelect()); },
                                    "Jump straight to any world and level of the four games.");
    ls.enabled = [this]() { return !achLocked(); };
    ls.valueText = locked;
    pg.items.push_back(ls);
    MenuItem ch = MenuItem::submenu("Cheats", [this]() { menu.push(pageCheats()); },
                                    "Toggle cheats for each game. They apply immediately.");
    ch.enabled = [this]() { return !achLocked(); };
    ch.valueText = locked;
    pg.items.push_back(ch);
    MenuItem ac = MenuItem::submenu("Achievements", [this]() { menu.push(pageAchievements()); },
                                    "Every game's achievements, and what you have earned.");
    ac.valueText = [this]() {
      int got = 0;
      for (const AchievementDef& d : achievementDefs()) got += achStore.unlocked(d.id) ? 1 : 0;
      return std::to_string(got) + " / " + std::to_string((int)achievementDefs().size());
    };
    pg.items.push_back(ac);
    MenuItem online = MenuItem::submenu("Online Play", [this]() { menu.push(pageOnline()); },
                                        "Play co-op over the internet: host a game, or join one as player 2.");
    online.valueText = [this]() { return net.active() ? netSummary() : std::string(); };
    pg.items.push_back(online);
    pg.items.push_back(MenuItem::submenu("Sound Select", [this]() { menu.push(pageSoundSelect()); },
                                         "Classic sound test: play any music number and sound effect."));
    pg.items.push_back(MenuItem::submenu("Music Player", [this]() { menu.push(pageMusicPlayer()); },
                                         "Browse and listen to the soundtrack of all four games."));
    pg.items.push_back(MenuItem::submenu("Options", [this]() { menu.push(pageOptions()); },
                                         "Display, graphics, colour, audio, controls, speed and achievements."));
    pg.items.push_back(MenuItem::submenu("Credits", [this]() { menu.push(pageCredits()); },
                                         "Who made this port, and where to find them."));

    pg.items.push_back(MenuItem::header("System"));
    pg.items.push_back(MenuItem::action("Take Screenshot", [this]() { screenshot(); },
                                        "Saves the game picture (without the menu) to the screenshots folder (" +
                                            bindingsText(kActScreenshot) + " in game)."));
    pg.items.push_back(MenuItem::action("Reset Game", [this]() {
      menu.confirm("Reset the game?", [this]() {
        menu.closeAll();
        launchGame(playing);
      }, "Back to its save files. Progress is kept only if the game saved it.");
    }, "Start this game again from its save files, as at power-on."));
    pg.items.push_back(MenuItem::action("Return to Launcher", [this]() {
      menu.confirm("Return to the launcher?", [this]() { returnToLauncher(); },
                   "Progress is kept only if the game saved it.");
    }, "Leave this game for the launcher, to pick another."));
#ifdef _WIN32
    pg.items.push_back(MenuItem::action("Quit to Desktop", [this]() {
      menu.confirm("Quit to desktop?", [this]() { running = false; },
#else
    pg.items.push_back(MenuItem::action("Quit", [this]() {
      menu.confirm("Quit?", [this]() { running = false; },
#endif
                   "Progress is kept only if the game saved it.");
    }, "Save your settings and exit."));
  };
  p->onLeave = [this]() { cfg.save(configPath); };
  return p;
}

std::string App::bindingsText(int a) const {
  std::string k = bindingName(cfg.keys[0][a]), j = bindingName(cfg.pad[0][a]);
  bool hk = !k.empty() && k != "-", hj = !j.empty() && j != "-";
  return hk && hj ? k + " / " + j : hk ? k : hj ? j : "unbound";
}

std::shared_ptr<MenuPage> App::pageOptions() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Options";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::submenu("Display", [this]() { menu.push(pageDisplay()); },
                                         "Fullscreen, monitor, widescreen and scaling."));
    pg.items.push_back(MenuItem::submenu("Graphics", [this]() { menu.push(pageGraphics()); },
                                         "Filters, CRT effects and sprite options."));
    pg.items.push_back(MenuItem::submenu("Colour", [this]() { menu.push(pageColor()); },
                                         "Colour profiles, picture adjustments and accessibility filters."));
    pg.items.push_back(MenuItem::submenu("Audio", [this]() { menu.push(pageAudio()); },
                                         "Volume, latency and custom soundtracks (MSU-1 music packs)."));
    pg.items.push_back(MenuItem::submenu("Controls", [this]() { menu.push(pageControls()); },
                                         "Keyboard and joypad bindings for both players."));
    pg.items.push_back(MenuItem::choice(
        "Menu Style", {"Storybook", "Retro", "Themed Dark", "Classic"}, [this]() { return cfg.menuStyle; },
        [this](int v) {
          cfg.menuStyle = v;
          applyConfig();
        },
        "How these menus look: Storybook (bright and chunky), Retro (black, bricks and a pixel font), Themed Dark "
        "(dark, under the game's own heading) or Classic (the original dark panel)."));
    pg.items.push_back(MenuItem::header("Launcher"));
    pg.items.push_back(MenuItem::choice(
        "Launcher Shows", {"Super Mario All-Stars", "The Four Games"}, [this]() { return cfg.launcherAllStars ? 0 : 1; },
        [this](int v) {
          cfg.launcherAllStars = v == 0;
          launcher.setAllStarsOnly(cfg.launcherAllStars);
        },
        "Super Mario All-Stars: one card, the cartridge's own title and game select. The Four Games: a card "
        "for each, straight to its save files."));
    // The player's own covers and background (none come with the game).
#ifdef __ANDROID__
    const std::string where = "Choose each picture below (PNG or JPG).";
#else
    const std::string where = "Put PNG or JPG pictures in the covers folder (Open Covers Folder below): smb1, "
                              "lostlevels, smb2, smb3 and allstars (the single card), and background.";
#endif
    pg.items.push_back(MenuItem::toggle(
        "Game Covers", [this]() { return cfg.launcherCovers ? 1 : 0; },
        [this](int v) {
          cfg.launcherCovers = v != 0;
          applyLauncherArt();
        },
        "The cards show your own cover pictures in place of their live previews; a card without one keeps its "
        "preview. " + where));
    pg.items.push_back(MenuItem::choice(
        "Cover Shape", {"Fill the Frame", "Whole Cover"}, [this]() { return cfg.launcherCoverFill ? 0 : 1; },
        [this](int v) {
          cfg.launcherCoverFill = v == 0;
          applyLauncherArt();
        },
        "Fill the Frame: the cover fills its card's square frame, its edges trimmed (a tall box's mostly from the "
        "bottom). Whole Cover: all of it, with dark bars beside a tall one."));
    pg.items.push_back(MenuItem::toggle(
        "Custom Background", [this]() { return cfg.launcherBackground ? 1 : 0; },
        [this](int v) {
          cfg.launcherBackground = v != 0;
          applyLauncherArt();
        },
        "The launcher's background is your own picture, filling the screen. " + where));
#ifdef __ANDROID__
    static const char* const kArtLabels[] = {"Super Mario Bros.", "The Lost Levels", "Super Mario Bros. 2",
                                             "Super Mario Bros. 3", "Super Mario All-Stars"};
    for (int k = 0; k <= Launcher::kCards; k++)
      pg.items.push_back(MenuItem::action(std::string("Choose Cover: ") + kArtLabels[k], [this, k]() { pickArt(k); },
                                          "A picture from your phone for this card (turn Game Covers on to see it)."));
    pg.items.push_back(MenuItem::action("Choose Background", [this]() { pickArt(-1); },
                                        "A picture from your phone for the launcher (turn Custom Background on)."));
#else
    pg.items.push_back(MenuItem::action("Open Covers Folder", [this]() {
      const std::wstring dir = pathJoin(exeDir, L"covers");
      makeDir(dir);
      openFolder(dir);
      menu.notify("Covers folder opened - pictures are read when you switch Game Covers on", 4.0f);
    }, "Where your covers and background go. After adding pictures, switch Game Covers (or Custom Background) "
       "off and on again to read them."));
#endif
    pg.items.push_back(MenuItem::header("Updates"));
    pg.items.push_back(MenuItem::toggle(
        "Check for Updates", [this]() { return cfg.checkUpdates ? 1 : 0; },
        [this](int v) {
          cfg.checkUpdates = v != 0;
          applyUpdateTag(cfg.updateLatest);
          if (cfg.checkUpdates) checkForUpdate(false);
        },
        std::string("Once a day the launcher looks up the latest release on GitHub (nothing about you or your game is "
                    "sent) and shows a badge when there's a newer version than this one (") +
            kAppVersion + ")."));
    if (!updateTag.empty()) {
      pg.items.push_back(MenuItem::action("Get Version " + updateVersion(), [this]() {
        openUrl(UpdateCheck::pageFor(updateTag).c_str());
        menu.notify("Opened the version " + updateVersion() + " download page in your browser");
      }, "Opens its page on GitHub, where it can be downloaded. Your saves and settings carry over."));
    } else {
      pg.items.push_back(MenuItem::action(updater.running() ? "Checking..." : "Check Now", [this]() {
        if (updater.running()) return;
        checkForUpdate(true);
        menu.notify("Checking for updates...", 2.0f);
        menu.refresh();
      }, std::string("Looks up the latest release on GitHub now. This is version ") + kAppVersion + "."));
    }
    pg.items.push_back(MenuItem::header("Gameplay"));
    pg.items.push_back(MenuItem::toggle(
        "NES Brick Bounce", [this]() { return cfg.nesBrickBounce ? 1 : 0; },
        [this](int v) {
          cfg.nesBrickBounce = v != 0;
          applyConfig();
        },
        "Super Mario Bros. and The Lost Levels: breaking a brick from below sends Mario back down, as on the NES. "
        "Off: he keeps rising, as in the original All-Stars. (BMF54123's brick fix.)"));
    pg.items.push_back(MenuItem::header("Multiplayer"));
    pg.items.push_back(MenuItem::toggle(
        "Two-Player Co-op", [this]() { return cfg.coop ? 1 : 0; },
        [this](int v) {
          cfg.coop = v != 0;
          applyConfig();
        },
        "Mario and Luigi play at once, Luigi on controller 2, in Super Mario Bros., The Lost Levels and Super "
        "Mario Bros. 3 (the screen follows both; a player left idle a few seconds is brought along). "
        "Each of those games' title screens also has CO-OP GAME, which sets this."));
    MenuItem online = MenuItem::submenu("Online Play", [this]() { menu.push(pageOnline()); },
                                        "Play over the internet: host a game, or join one as player 2.");
    online.valueText = [this]() { return net.active() ? netSummary() : std::string(); };
    pg.items.push_back(online);
    pg.items.push_back(MenuItem::header("Speed"));
    MenuItem ff = MenuItem::toggle("Fast Forward", [this]() { return cfg.fastForward && !achLocked() ? 1 : 0; },
                                   [this](int v) { cfg.fastForward = v != 0; },
                                   "Hold " + bindingsText(kActFastForward) + " to run the game at 4x speed (sound off).");
    ff.enabled = [this]() { return !achLocked(); };
    ff.valueText = [this]() { return std::string(achLocked() ? "Off (achievements on)" : ""); };
    pg.items.push_back(ff);
    MenuItem rw = MenuItem::toggle("Rewind", [this]() { return cfg.rewind && !achLocked() ? 1 : 0; },
                                   [this](int v) {
                                     cfg.rewind = v != 0;
                                     if (!v) rewind.clear();
                                   },
                                   "Hold " + bindingsText(kActRewind) +
                                       " to step back through the last minute of play. Saved game progress is "
                                       "never undone.");
    rw.enabled = [this]() { return !achLocked(); };
    rw.valueText = [this]() { return std::string(achLocked() ? "Off (achievements on)" : ""); };
    pg.items.push_back(rw);
    // (right under what it switches off)
    pg.items.push_back(MenuItem::header("Achievements"));
    pg.items.push_back(achievementsSwitch());
    pg.items.push_back(MenuItem::header("Hotkeys"));
    auto hot = [&](const char* what, int a) {
      MenuItem it = MenuItem::info(what);
      it.valueText = [this, a]() { return bindingsText(a); };
      pg.items.push_back(it);
    };
    hot("Menu", kActMenu);
    hot("Save state", kActSaveState);
    hot("Load state", kActLoadState);
    hot("Fast forward (hold)", kActFastForward);
    hot("Rewind (hold)", kActRewind);
    hot("Fullscreen", kActFullscreen);
    hot("Screenshot", kActScreenshot);
  };
  p->onLeave = [this]() { cfg.save(configPath); };
  return p;
}

// ---------------------------------------------------------------------------
// Achievements

void App::achNewRun(bool counts) {
  if (game) game->achievements().newRun(counts);
}

void App::setAchievements(bool on) {
  if (on == cfg.achievements) return;
  cfg.achievements = on;
  // They only count in a game started with them on (from the launcher or
  // Reset Game): switched on mid-game, what cheats, the level select, fast
  // forward or rewind did in this game can't earn anything.
  if (game) game->achievements().counts = false;
  if (on) {
    rewind.clear();
    if (!inLauncher) menu.notify("Achievements count from your next Reset Game", 4.0f);
  }
  applyConfig();  // cheats leave (or come back into) the running game
}

void App::achFrame() {
  AchievementTracker& t = game->achievements();
  if (!t.counts) return;
  std::vector<AchievementEvent> ev = t.take();
  if (ev.empty()) return;
  bool dirty = false;
  for (const AchievementEvent& e : ev) {
    const AchievementDef* d = findAchievement(e.id);
    if (!d) continue;
    if (e.kind == AchievementEvent::Unlock) {
      achAward(e.id);
    } else if (e.kind == AchievementEvent::Progress) {
      dirty = achStore.setProgress(e.id, e.value) || dirty;
      if (d->target > 0 && e.value >= d->target) achAward(e.id);
    } else if (e.kind == AchievementEvent::Add) {
      const int total = achStore.progress(e.id) + e.value;
      dirty = achStore.setProgress(e.id, total) || dirty;
      if (d->target > 0 && total >= d->target) achAward(e.id);
    } else if (e.kind == AchievementEvent::Mark && e.value >= 0 && e.value < 31) {
      int mask = achStore.progress(e.id) | (1 << e.value);
      dirty = achStore.setProgress(e.id, mask) || dirty;
      int found = 0;
      for (int b = 0; b < 31; b++) found += (mask >> b) & 1;
      if (found >= d->target) achAward(e.id);
    }
  }
  if (dirty) achStore.save();
}

void App::achAward(const std::string& id) {
  if (!achStore.unlock(id)) return;
  if (const AchievementDef* d = findAchievement(id)) menu.achievement(d->name, d->desc);
  achCheckCollection();
  achStore.save();
}

// All-Star (finish all four games) and Completionist (everything else).
void App::achCheckCollection() {
  int finished = 0;
  for (const char* id : kFinishedGame) finished += achStore.unlocked(id) ? 1 : 0;
  achStore.setProgress("smas_allstar", finished);
  if (finished == 4 && achStore.unlock("smas_allstar")) {
    if (const AchievementDef* d = findAchievement("smas_allstar")) menu.achievement(d->name, d->desc);
  }
  bool all = true;
  for (const AchievementDef& d : achievementDefs())
    if (std::string(d.id) != "smas_complete") all = all && achStore.unlocked(d.id);
  if (all && achStore.unlock("smas_complete")) {
    if (const AchievementDef* d = findAchievement("smas_complete")) menu.achievement(d->name, d->desc);
  }
}

// The on / off switch. Switching off during a game that counts asks first:
// that game can't earn any more until Reset Game.
MenuItem App::achievementsSwitch() {
  MenuItem it = MenuItem::toggle(
      "Achievements", [this]() { return cfg.achievements ? 1 : 0; },
      [this](int v) {
        if (v || inLauncher || !game || !game->achievements().counts) {
          setAchievements(v != 0);
          cfg.save(configPath);
          menu.refresh();
          return;
        }
        menu.confirm("Turn achievements off?", [this]() {
          setAchievements(false);
          cfg.save(configPath);
        }, "Cheats, Level Select, Fast Forward and Rewind come back. Achievements count again after Reset "
           "Game.");
      },
      "On: earn achievements; cheats, level select, fast forward and rewind are off. Off: those come back.");
  it.valueText = [this]() {
    return std::string(cfg.achievements && !inLauncher && game && !game->achievements().counts ? "from your next Reset Game"
                                                                                                : "");
  };
  return it;
}

std::shared_ptr<MenuPage> App::pageAchievements() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Achievements";
  p->wide = true;
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(achievementsSwitch());
    pg.items.push_back(MenuItem::action("Export to CSV", [this]() {
      // (Windows / Linux: next to the settings; Android: where screenshots go, reachable over USB)
      const std::wstring dir = pathJoin(pathDir(shotDir), L"achievements export");
      std::string err;
      if (!exportAchievements(dir, achStore, &err)) {
        menu.notify("Export failed: " + err, 5.0f);
        return;
      }
      logf("achievements exported to %s", narrow(dir).c_str());
#ifdef __ANDROID__
      menu.notify("Exported to Android/data/com.darhkfox.allstars/files/achievements export", 6.0f);
#else
      openFolder(dir);
      menu.notify("Achievements exported: achievements.csv and icons", 4.0f);
#endif
    }, "Writes achievements.csv (ID, title, description, points, trophy, hidden, progress, unlocked and when, "
       "icons) and an icons folder, earned and locked, for tools such as PlayniteAchievements' spreadsheet import."));
    // the running game's first
    GameId order[4] = {GameId::SMB1, GameId::SMBLL, GameId::SMB2, GameId::SMB3};
    GameId cur = inLauncher || !game ? Launcher::cardGame(launcher.card()) : game->current();
    for (int i = 1; i < 4; i++)
      if (order[i] == cur) std::swap(order[0], order[i]);
    int total = 0, got = 0;
    auto add = [&](const std::string& group, GameId g) {
      bool any = false;
      for (const AchievementDef& d : achievementDefs()) {
        if (d.game != g) continue;
        if (!any) pg.items.push_back(MenuItem::header(group));
        any = true;
        const bool have = achStore.unlocked(d.id);
        total++;
        got += have ? 1 : 0;
        // (a secret one shows what it is too, marked as a secret)
        MenuItem it = MenuItem::action(d.name, []() {}, d.secret ? std::string("Secret: ") + d.desc : std::string(d.desc));
        std::string id = d.id;
        int target = d.target;
        bool set = d.set;
        it.valueText = [this, id, target, set]() {
          std::string on = achStore.unlockedOn(id);
          if (!on.empty()) return std::string("\xE2\x9C\x93  ") + on;
          int have = achStore.progress(id);
          if (set) {  // a set of things: how many were done
            int n = 0;
            for (int b = 0; b < 31; b++) n += (have >> b) & 1;
            have = n;
          }
          if (target > 0) return std::to_string(std::min(have, target)) + " / " + std::to_string(target);
          return std::string();
        };
        pg.items.push_back(it);
      }
    };
    for (GameId g : order) add(SmasGame::gameName(g), g);
    add("Across the collection", GameId::Boot);
    pg.subtitle = std::to_string(got) + " of " + std::to_string(total) + " unlocked";
  };
  return p;
}

// Once, on the first start: what achievements are and what they switch off.
void App::achWelcome() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Achievements";
  p->subtitle = std::to_string((int)achievementDefs().size()) +
                " to earn across Super Mario Bros., The Lost Levels, Super Mario Bros. 2 and 3";
  p->wide = true;
  p->items.push_back(MenuItem::info("Rescue princesses, defeat the bosses, find secrets and pull off tricky moves."));
  p->items.push_back(MenuItem::header("To keep them fair, while achievements are on"));
  p->items.push_back(MenuItem::info("Cheats, Level Select, Fast Forward and Rewind are switched off."));
  p->items.push_back(MenuItem::info("Save states still work. Turn achievements off any time in Options."));
  p->items.push_back(MenuItem::separator());
  p->items.push_back(MenuItem::action("Play with achievements", [this]() { menu.closeAll(); },
                                      "Each one pops up as you earn it; the menu's Achievements page lists them all."));
  p->items.push_back(MenuItem::action("Turn achievements off", [this]() {
    setAchievements(false);
    cfg.save(configPath);
    menu.closeAll();
  }, "Cheats, Level Select, Fast Forward and Rewind stay available. Turn achievements on again in Options."));
  menuAnim = 0;
  menu.push(p);
}

std::shared_ptr<MenuPage> App::pageCredits() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Credits";
  p->subtitle = "Super Mario All-Stars PC Port";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::separator("Find me over at"));
    struct Link {
      const char* site;
      const char* shown;
      const char* url;
    };
    static const Link kLinks[] = {
        {"YouTube", "youtube.com/adhgaming", "https://www.youtube.com/adhgaming"},
        {"Reddit", "u/Darhkwing", "https://www.reddit.com/user/Darhkwing"},
        {"GitHub", "github.com/Darhkfox", "https://github.com/Darhkfox"},
        {"X", "x.com/ADH_Gaming", "https://x.com/ADH_Gaming"},
        {"Patreon", "patreon.com/c/TeamRedTail", "https://www.patreon.com/c/TeamRedTail"},
    };
    for (const Link& l : kLinks) {
      MenuItem it = MenuItem::action(l.site, [this, l]() {
        openUrl(l.url);
        menu.notify(std::string("Opened ") + l.site + " in your browser");
      }, "Opens it in your web browser.");
      std::string shown = l.shown;
      it.valueText = [shown]() { return shown; };
      pg.items.push_back(it);
    }
    pg.items.push_back(MenuItem::separator());
    pg.items.push_back(MenuItem::info("Super Mario All-Stars is © Nintendo."));
    pg.items.push_back(MenuItem::info("This port contains no game data: it runs from your own ROM."));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageDisplay() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Display";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
#ifdef _WIN32
    pg.items.push_back(MenuItem::choice(
        "Display Mode", {"Windowed", "Borderless Fullscreen", "Exclusive Fullscreen"},
        [this]() { return (int)cfg.displayMode; },
        [this](int v) {
          cfg.displayMode = (DisplayMode)v;
          applyDisplayMode();
          menu.refresh();
        },
        "Borderless is recommended. Alt+Enter or F11 toggles windowed mode."));
    monitors = enumerateMonitors();
    std::vector<std::string> names;
    for (auto& m : monitors) names.push_back(narrow(m.name));
    if (names.empty()) names.push_back("Primary");
    pg.items.push_back(MenuItem::choice(
        "Monitor", names,
        [this]() {
          for (size_t i = 0; i < monitors.size(); i++)
            if (monitors[i].device == findMonitor(monitors, cfg.monitor).device) return (int)i;
          return 0;
        },
        [this](int v) {
          if (v >= 0 && v < (int)monitors.size()) cfg.monitor = monitors[v].device;
          applyDisplayMode();
        },
        "Choose which display the game appears on."));
    pg.items.push_back(MenuItem::slider("Window Size", 1, 8, 1, [this]() { return cfg.windowScale; },
                                        [this](int v) {
                                          cfg.windowScale = v;
                                          if (cfg.displayMode == DisplayMode::Windowed) applyDisplayMode();
                                        },
                                        "x", "Window height as a multiple of the original 224 lines."));
#elif !defined(__ANDROID__)
    pg.items.push_back(MenuItem::choice(
        "Display Mode", {"Windowed", "Fullscreen"},
        [this]() { return cfg.displayMode == DisplayMode::Windowed ? 0 : 1; },
        [this](int v) {
          cfg.displayMode = v == 0 ? DisplayMode::Windowed : DisplayMode::Borderless;
          applyDisplayMode();
          menu.refresh();
        },
        "F11 toggles windowed mode."));
#endif
    pg.items.push_back(MenuItem::choice(
        "Aspect Ratio", {"4:3 (Original)", "16:10 Widescreen", "16:9 Widescreen", "21:9 Ultrawide", "Auto (Fill Display)"},
        [this]() { return cfg.aspect; },
        [this](int v) {
          cfg.aspect = v;
          if (cfg.displayMode == DisplayMode::Windowed) applyDisplayMode();
        },
        "Widescreen shows more of each level to the left and right. Auto matches your display."));
    pg.items.push_back(MenuItem::choice("Pixel Shape", {"CRT (8:7, as on a TV)", "Square (1:1)"},
                                        [this]() { return cfg.pixelAspectCrt ? 0 : 1; },
                                        [this](int v) { cfg.pixelAspectCrt = v == 0; },
                                        "CRT matches how the game looked on a television."));
    pg.items.push_back(MenuItem::choice("Scaling", {"Fit", "Integer (Pixel Perfect)", "Stretch"},
                                        [this]() { return cfg.scaling; }, [this](int v) { cfg.scaling = v; },
                                        "Integer scaling keeps every pixel the same size (with CRT pixels, as close to "
                                        "8:7 as whole screen pixels allow)."));
    pg.items.push_back(MenuItem::toggle("Re-anchor HUD", [this]() { return cfg.hudAnchor ? 1 : 0; },
                                        [this](int v) {
                                          cfg.hudAnchor = v;
                                          applyConfig();
                                        },
                                        "In widescreen, move score, lives and timers out to the screen edges (SMB3's "
                                        "status bar stays centred)."));
    pg.items.push_back(MenuItem::toggle("Dim Wrap-Around Sides", [this]() { return cfg.dimWrap ? 1 : 0; },
                                        [this](int v) {
                                          cfg.dimWrap = v;
                                          applyConfig();
                                        },
                                        "SMB2's climbing areas, SMB3's vertical areas and its battle game wrap round "
                                        "at the edges of the 4:3 picture: dim the widescreen sides there, so it's "
                                        "clear where the play space ends."));
    pg.items.push_back(MenuItem::choice("Widescreen Enemies", {"Original Timing", "Active In Full View"},
                                        [this]() { return cfg.widescreenEnemies; },
                                        [this](int v) {
                                          cfg.widescreenEnemies = v;
                                          applyConfig();
                                        },
                                        "Active: enemies spawn at the edge of the wide view and behave normally "
                                        "there. Original: identical to the 4:3 game."));
    pg.items.push_back(MenuItem::choice("Side Borders", {"Black", "Blurred Game", "Dark Frame"},
                                        [this]() { return cfg.pillarbox; }, [this](int v) { cfg.pillarbox = v; },
                                        "What fills the screen where the game has nothing to show (menus, 4:3)."));
#ifdef _WIN32
    pg.items.push_back(MenuItem::toggle("VSync", [this]() { return cfg.vsync ? 1 : 0; },
                                        [this](int v) { cfg.vsync = v; },
                                        "On: synchronise with the display. Off: show frames at the game's own "
                                        "60 fps (best with G-SYNC / FreeSync)."));
    MenuItem bfi = MenuItem::toggle("Black Frame Insertion", [this]() { return cfg.blackFrames ? 1 : 0; },
                                    [this](int v) { cfg.blackFrames = v != 0; },
                                    "Sharper motion: a black frame between the game's frames, like a CRT. Needs a "
                                    "120 Hz screen (or 180 / 240 Hz) and VSync; the picture gets darker.");
    bfi.valueText = [this]() { return blackFramesNote(); };
    pg.items.push_back(bfi);
    MenuItem hfr = MenuItem::toggle("High Frame Rate", [this]() { return cfg.highFrameRate ? 1 : 0; },
                                    [this](int v) { cfg.highFrameRate = v != 0; },
                                    "Smoother motion on 100 Hz and faster screens: the refreshes between the game's "
                                    "60 frames show in-between pictures (scrolling and sprites part of the way). The "
                                    "game still runs at 60; the picture is one frame later. With VSync off "
                                    "(G-SYNC / FreeSync) they're shown at the screen's rate. Not with Black Frame "
                                    "Insertion.");
    hfr.valueText = [this]() {
      if (!cfg.highFrameRate) return std::string();
      if (cfg.blackFrames) return std::string("off with Black Frame Insertion");
      if (pacing.hz < 89) return "needs 100 Hz (now " + std::to_string((int)std::lround(pacing.hz)) + ")";
      if (cfg.vsync && !pacing.perFrame) return std::to_string((int)std::lround(pacing.hz)) + " Hz, timed to each refresh";
      return std::to_string((int)std::lround(pacing.hz)) + " Hz, " + std::to_string(hfrSubFrames() - 1) +
             " in-between";
    };
    pg.items.push_back(hfr);
#endif
    pg.items.push_back(MenuItem::toggle("Show FPS", [this]() { return cfg.showFps ? 1 : 0; },
                                        [this](int v) { cfg.showFps = v; }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageGraphics() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Graphics";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::choice(
        "Filter", {"Nearest (Sharp Pixels)", "Sharp Bilinear", "Smooth (Bilinear)", "CRT Scanlines", "CRT Aperture Grille",
                   "LCD Grid"},
        [this]() { return cfg.filter; }, [this](int v) { cfg.filter = v; },
        "Sharp Bilinear keeps pixels crisp without uneven sizes."));
#ifdef _WIN32
    MenuItem sl = MenuItem::submenu("Slang Shader", [this]() { menu.push(pageSlang()); },
                                    "RetroArch slang shader presets (.slangp) from the 'shaders' folder next to the "
                                    "game, in place of the filter above (and the colour settings).");
    sl.valueText = [this]() {
      if (cfg.slangPreset.empty()) return std::string("Off");
      std::wstring n = std::filesystem::path(cfg.slangPreset).stem().wstring();
      return narrow(n);
    };
    pg.items.push_back(sl);
#endif
    pg.items.push_back(MenuItem::slider("Scanline / Effect Strength", 0, 100, 5, [this]() { return cfg.scanlines; },
                                        [this](int v) { cfg.scanlines = v; }, "%",
                                        "Strength of the CRT and LCD effects."));
    pg.items.push_back(MenuItem::slider("Screen Curvature", 0, 100, 5, [this]() { return cfg.crtCurve; },
                                        [this](int v) { cfg.crtCurve = v; }, "%", "Simulated curved CRT glass."));
    pg.items.push_back(MenuItem::toggle("Remove Sprite Limit", [this]() { return cfg.noSpriteLimit ? 1 : 0; },
                                        [this](int v) {
                                          cfg.noSpriteLimit = v;
                                          applyConfig();
                                        },
                                        "Reduce sprite flicker by lifting the 32 sprites per line hardware limit."));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageColor() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Colour";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::choice(
        "Colour Profile",
        {"Original", "Vivid", "CRT Television", "Muted", "Greyscale", "Sepia", "Pocket (Green)", "Virtual (Red)",
         "Night (Warm)", "Console Accurate"},
        [this]() { return cfg.colorProfile; }, [this](int v) { cfg.colorProfile = v; },
        "Overall colour style of the picture."));
    pg.items.push_back(MenuItem::choice(
        "Mario's Colours", {"Original", "Classic NES", "Fire", "Ice", "Shadow", "Gold", "Forest", "Ocean"},
        [this]() { return cfg.playerPalette; },
        [this](int v) {
          cfg.playerPalette = v;
          applyConfig();
        },
        "Alternate outfit colours for Mario in Super Mario Bros. and The Lost Levels."));
    pg.items.push_back(MenuItem::choice(
        "Luigi's Colours", {"Original", "Classic NES", "Fire", "Ice", "Shadow", "Gold", "Forest", "Ocean"},
        [this]() { return cfg.luigiPalette; },
        [this](int v) {
          cfg.luigiPalette = v;
          applyConfig();
        },
        "Alternate outfit colours for Luigi in Super Mario Bros. and The Lost Levels (co-op, or playing as Luigi)."));
    pg.items.push_back(MenuItem::slider("Brightness", 50, 150, 5, [this]() { return cfg.brightness; },
                                        [this](int v) { cfg.brightness = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Contrast", 50, 150, 5, [this]() { return cfg.contrast; },
                                        [this](int v) { cfg.contrast = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Saturation", 0, 200, 5, [this]() { return cfg.saturation; },
                                        [this](int v) { cfg.saturation = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Gamma", 50, 150, 5, [this]() { return cfg.gamma; },
                                        [this](int v) { cfg.gamma = v; }, "%"));
    pg.items.push_back(MenuItem::slider("Colour Temperature", -100, 100, 10, [this]() { return cfg.colorTemp; },
                                        [this](int v) { cfg.colorTemp = v; }, "",
                                        "Negative values are cooler (bluer), positive values warmer."));
    pg.items.push_back(MenuItem::choice("Colour Vision Assist",
                                        {"Off", "Protanopia", "Deuteranopia", "Tritanopia"},
                                        [this]() { return cfg.colorblind; }, [this](int v) { cfg.colorblind = v; },
                                        "Shifts colours so they are easier to tell apart with colour blindness."));
    pg.items.push_back(MenuItem::action("Reset Picture Settings", [this]() {
      cfg.brightness = cfg.contrast = cfg.gamma = 100;
      cfg.saturation = 100;
      cfg.colorProfile = 0;
      cfg.colorblind = 0;
      cfg.colorTemp = 0;
      menu.refresh();
    }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageAudio() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Audio";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::slider("Volume", 0, 100, 5, [this]() { return cfg.volume; },
                                        [this](int v) {
                                          cfg.volume = v;
                                          applyConfig();
                                        },
                                        "%"));
    pg.items.push_back(MenuItem::slider("Music Volume", 0, 100, 5, [this]() { return cfg.musicVolume; },
                                        [this](int v) {
                                          cfg.musicVolume = v;
                                          applyConfig();
                                        },
                                        "%", "The games' music (and a music pack's). 0% turns the music off and keeps "
                                             "the sound effects."));
    pg.items.push_back(MenuItem::slider("Sound Effects Volume", 0, 100, 5, [this]() { return cfg.sfxVolume; },
                                        [this](int v) {
                                          cfg.sfxVolume = v;
                                          applyConfig();
                                        },
                                        "%", "Jumps, coins, stomps and the rest. 0% turns them off and keeps the music."));
    pg.items.push_back(MenuItem::slider("Latency", 20, 160, 10, [this]() { return cfg.audioLatency; },
                                        [this](int v) {
                                          cfg.audioLatency = v;
                                          applyConfig();
                                          audio.flush();  // take effect now, not after a slow drift
                                        },
                                        " ms", "Lower values respond faster; raise it if you hear crackling."));
    pg.items.push_back(MenuItem::toggle("Mute When Unfocused", [this]() { return cfg.muteUnfocused ? 1 : 0; },
                                        [this](int v) { cfg.muteUnfocused = v; }));
    // MSU-1 music packs (app/msupack.h): Off, then the msu folder's packs
    pg.items.push_back(MenuItem::header("Custom Soundtrack (MSU-1)"));
    if (msuDir.empty()) msuDir = pathJoin(exeDir, L"msu");
    auto packs = std::make_shared<std::vector<std::wstring>>(listMsuPacks(msuDir));
    if (!cfg.msuPack.empty() && std::find(packs->begin(), packs->end(), cfg.msuPack) == packs->end())
      packs->push_back(cfg.msuPack);  // (chosen, since gone: shown so it can be turned off)
    std::vector<std::string> names = {"Off"};
    for (const auto& n : *packs) names.push_back(narrow(n));
    std::string help;
    if (!cfg.msuPack.empty() && cfg.msuPack == msuLoaded && !msuStatus.empty()) help = msuStatus + " ";
    else if (cfg.msuPack != msuLoaded)
      help = inLauncher ? "Starts when you next start a game. "
                        : "Choose Restart With This Pack below to hear it now (or it starts with your next game). ";
    help += packs->empty() ? "Put a pack in its own folder in the msu folder: its MSU-1 patch (.bps or .ips) and "
                             "its tracks (.pcm), then come back here."
                           : "A pack is a folder in the msu folder: its MSU-1 patch (.bps or .ips) and tracks (.pcm).";
    pg.items.push_back(MenuItem::choice(
        "Music Pack", names,
        [this, packs]() {
          for (size_t i = 0; i < packs->size(); i++)
            if ((*packs)[i] == cfg.msuPack) return (int)i + 1;
          return 0;
        },
        [this, packs](int v) {
          cfg.msuPack = v <= 0 ? std::wstring() : (*packs)[(size_t)std::min<int>(v, (int)packs->size()) - 1];
          if (inLauncher) {
            reloadForMsu();
            if (!cfg.msuPack.empty() && !(snes && snes->msu)) menu.notify(msuStatus, 5.0f);
          }
          if (!test.on && test.runSeconds <= 0) cfg.save(configPath);
          menu.refresh();  // (its help says what became of it)
        },
        help));
    if (!inLauncher && snes && cfg.msuPack != msuLoaded) {
      // (in a game: the console is rebuilt with the pack, so the game starts again)
      pg.items.push_back(MenuItem::action("Restart With This Pack", [this]() {
        menu.confirm("Restart the game with this music?", [this]() { launchGame(playing); },
                     "The game starts again from its title. Progress since your last save is lost.");
      }, "Starts the game again from its title with the chosen music pack (or none)."));
    }
    pg.items.push_back(MenuItem::slider("Music Pack Volume", 0, 100, 5, [this]() { return cfg.msuVolume; },
                                        [this](int v) {
                                          cfg.msuVolume = v;
                                          applyConfig();
                                        },
                                        "%"));
#ifndef __ANDROID__
    pg.items.push_back(MenuItem::action("Open MSU-1 Folder", [this]() {
      makeDir(msuDir);
      openFolder(msuDir);
    }, "Where music packs go: one folder per pack."));
#endif
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageControls() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Controls";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    pg.items.push_back(MenuItem::submenu("Player 1 Keyboard", [this]() { menu.push(pageBindings(0, false)); }));
    pg.items.push_back(MenuItem::submenu("Player 1 Joypad", [this]() { menu.push(pageBindings(0, true)); }));
    pg.items.push_back(MenuItem::submenu("Player 2 Keyboard", [this]() { menu.push(pageBindings(1, false)); }));
    pg.items.push_back(MenuItem::submenu("Player 2 Joypad", [this]() { menu.push(pageBindings(1, true)); }));
    std::vector<std::string> pads;
    for (int i = 0; i < 8; i++) {
      const PadState* ps = input.pad(i);
      std::string n = "Pad " + std::to_string(i + 1);
      if (!ps) n += " (not connected)";
      else n += ": " + narrow(ps->name) + (ps->connected ? "" : " (unplugged)");
      pads.push_back(n);
    }
    pg.items.push_back(MenuItem::choice("Player 1 Uses", pads, [this]() { return cfg.padIndex[0]; },
                                        [this](int v) { cfg.padIndex[0] = v; }));
    pg.items.push_back(MenuItem::choice("Player 2 Uses", pads, [this]() { return cfg.padIndex[1]; },
                                        [this](int v) { cfg.padIndex[1] = v; }));
    pg.items.push_back(MenuItem::choice("Select Button", {"Opens Menu", "Tap: Game Select  -  Hold: Menu"},
                                        [this]() { return cfg.selectMode; },
                                        [this](int v) { cfg.selectMode = v; },
                                        "Opens Menu: Select brings up this menu; the game's own Select is on the "
                                        "left stick button or Menu > Press Select In Game."));
    pg.items.push_back(MenuItem::choice(
        "Menu Buttons", {"Bottom Confirms", "Right Confirms"}, [this]() { return cfg.confirmRight ? 1 : 0; },
        [this](int v) {
          cfg.confirmRight = v != 0;
          applyConfig();
        },
        "Which face button chooses in the menus and the launcher: the bottom one (A on an Xbox pad), or the right "
        "one (A on a Nintendo pad); the other goes back. The games' own buttons are set by their TYPE A / TYPE B "
        "and Button Bindings."));
    pg.items.push_back(MenuItem::slider("Stick Deadzone", 10, 80, 5, [this]() { return cfg.deadzone; },
                                        [this](int v) { cfg.deadzone = v; }, "%"));
#ifndef _WIN32
    pg.items.push_back(MenuItem::header("Touch screen"));
    pg.items.push_back(MenuItem::choice("On-Screen Controls", {"While Touching", "Always", "Never"},
                                        [this]() { return cfg.touchMode; }, [this](int v) { cfg.touchMode = v; },
                                        "While Touching: shown once you touch the screen, hidden when a controller "
                                        "or keyboard is used."));
    pg.items.push_back(MenuItem::slider("Controls Opacity", 10, 100, 10, [this]() { return cfg.touchOpacity; },
                                        [this](int v) { cfg.touchOpacity = v; }, "%"));
#endif
    pg.items.push_back(MenuItem::action("Restore Default Controls", [this]() {
      Config d;
      d.setDefaults();
      memcpy(cfg.keys, d.keys, sizeof(cfg.keys));
      memcpy(cfg.pad, d.pad, sizeof(cfg.pad));
      menu.notify("Controls reset");
    }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageBindings(int player, bool pad) {
  auto p = std::make_shared<MenuPage>();
  p->title = "Player " + std::to_string(player + 1) + (pad ? " Joypad" : " Keyboard");
  p->subtitle = pad ? "Positional layout: the bottom face button is SNES B" : "";
  p->rebuild = [this, player, pad](MenuPage& pg) {
    pg.items.clear();
    int last = player == 0 ? kActCount : kActSelect + 1;
    for (int a = 0; a < last; a++) {
      MenuItem it;
      it.kind = MenuItem::Bind;
      it.label = actionName(a);
      it.valueText = [this, player, pad, a]() {
        return bindingName((pad ? cfg.pad : cfg.keys)[player][a]);
      };
      it.onActivate = [this, player, pad, a]() { startBind(player, pad, a); };
      it.help = "Press Enter / A to change.";
      pg.items.push_back(it);
    }
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageCheats() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Cheats";
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    const auto& list = SmasGame::cheatList();
    if (list.empty()) {
      pg.items.push_back(MenuItem::info("No cheats available."));
      return;
    }
    GameId last = GameId::Boot;
    for (const CheatDef& c : list) {
      if (c.game != last) {
        pg.items.push_back(MenuItem::separator(SmasGame::gameName(c.game)));
        last = c.game;
      }
      int id = c.id;
      pg.items.push_back(MenuItem::toggle(c.name, [this, id]() { return (cfg.cheats >> id) & 1 ? 1 : 0; },
                                          [this, id](int v) {
                                            if (v) cfg.cheats |= 1ull << id;
                                            else cfg.cheats &= ~(1ull << id);
                                            applyConfig();
                                          },
                                          c.desc));
    }
    pg.items.push_back(MenuItem::separator());
    pg.items.push_back(MenuItem::action("Disable All Cheats", [this]() {
      cfg.cheats = 0;
      applyConfig();
      menu.refresh();
    }));
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageLevelSelect() {
  // Every level listed under its world (as the Mickey & Donald Collection's),
  // the game chosen at the top; each asks first.
  auto p = std::make_shared<MenuPage>();
  p->title = "Level Select";
  auto selGame = std::make_shared<int>(0);
  // SMB2's character, in the select screen's order (its $008F codes below);
  // the one being played, if SMB2 is.
  static const char* const kChars[] = {"Mario", "Luigi", "Toad", "Peach"};
  static const int kCharCode[] = {0, 3, 2, 1};
  auto selChar = std::make_shared<int>(0);
  auto selCoop = std::make_shared<int>(0);  // (1 PLAYER unless chosen: as each title screen starts)
  if (game->current() == GameId::SMB2)
    for (int i = 0; i < 4; i++)
      if (kCharCode[i] == (snes->wram[0x008f] & 3)) *selChar = i;
  {
    // Start on the game being played, if any.
    const auto& defs = SmasGame::levelSelectList();
    GameId cur = game->current();
    if (cur != GameId::SMB1 && cur != GameId::SMBLL && cur != GameId::SMB2 && cur != GameId::SMB3) cur = playing;
    for (size_t i = 0; i < defs.size(); i++)
      if (defs[i].game == cur) *selGame = (int)i;
  }
  p->rebuild = [this, selGame, selChar, selCoop](MenuPage& pg) {
    pg.items.clear();
    const auto& defs = SmasGame::levelSelectList();
    if (defs.empty()) {
      pg.items.push_back(MenuItem::info("Level select is not available."));
      return;
    }
    std::vector<std::string> games;
    for (auto& d : defs) games.push_back(SmasGame::gameName(d.game));
    *selGame = std::clamp(*selGame, 0, (int)defs.size() - 1);
    const LevelSelectDef& d = defs[*selGame];
    pg.subtitle = SmasGame::gameName(d.game);
    pg.items.push_back(MenuItem::choice("Game", games, [selGame]() { return *selGame; },
                                        [this, selGame](int v) {
                                          *selGame = v;
                                          menu.refresh();
                                        },
                                        "Which game's levels are listed."));
    if (d.game == GameId::SMB2)
      pg.items.push_back(MenuItem::choice("Character", {kChars[0], kChars[1], kChars[2], kChars[3]},
                                          [selChar]() { return *selChar; }, [selChar](int v) { *selChar = v; },
                                          "Who plays the level (chosen for you on the character select)."));
    const bool coopGame = d.game == GameId::SMB1 || d.game == GameId::SMBLL || d.game == GameId::SMB3;
    if (coopGame)
      pg.items.push_back(MenuItem::choice("Players", {"1 Player", "Co-op"}, [selCoop]() { return *selCoop; },
                                          [selCoop](int v) { *selCoop = v; },
                                          "One player, or Mario and Luigi at once (Luigi on controller 2), as the "
                                          "title screen's CO-OP GAME."));
    const int gi = *selGame;
    const bool smb2 = d.game == GameId::SMB2;
    for (int w = 0; w < (int)d.worlds.size(); w++) {
      pg.items.push_back(MenuItem::header(d.worlds[w]));
      for (int l = 0; l < (int)d.levels[w].size(); l++) {
        const std::string name = d.levels[w][l];
        const std::string where = std::string(SmasGame::gameName(d.game)) + ", " + d.worlds[w];
        pg.items.push_back(MenuItem::action(name, [this, gi, w, l, name, where, smb2, selChar, selCoop, coopGame]() {
          const int ch = smb2 ? kCharCode[*selChar] : -1;
          const int co = coopGame ? *selCoop : -1;
          menu.confirm("Start " + name + (smb2 ? std::string(" as ") + kChars[*selChar] : std::string()) +
                           (co == 1 ? std::string(" in co-op") : std::string()) + "?",
                       [this, gi, w, l, ch, co]() {
            if (netBlocked("use the level select")) return;
            const auto& defs2 = SmasGame::levelSelectList();
            std::string err;
            if (co >= 0 && cfg.coop != (co == 1)) {  // (the choice sets the setting, as a title screen's does)
              cfg.coop = co == 1;
              applyConfig();
              cfg.save(configPath);
            }
            if (game->requestLevel(defs2[gi].game, w, l, &err, ch)) {
              playing = defs2[gi].game;  // (Reset Game, Return to Launcher: this one now)
              rewind.clear();
              achNewRun(false);  // (a level select run never counts)
              menu.closeAll();
              audio.flush();
            } else {
              menu.notify(err);
            }
          }, where + ". Progress in the current game is not saved.");
        }, where + "."));
      }
    }
  };
  return p;
}

std::shared_ptr<MenuPage> App::pageSoundSelect() { return music.soundSelectPage(this->menu); }
std::shared_ptr<MenuPage> App::pageMusicPlayer() { return music.playerPage(this->menu); }

// One pass of the main loop: input and menus every display refresh, then
// `gameFrames` emulated frames (0 when this refresh repeats the last picture).
void App::frame(double dt, int gameFrames) {
  dt = std::min(dt, 0.1);
  menu.tickToasts((float)dt);
  typedNow.swap(textTyped);
  textTyped.clear();
  updateInput(dt);
  updateTick();
  netPass(dt);  // (online play: packets, and the host's game arriving)
  if (bindAction >= 0) updateBind(dt);
  if (inLauncher) {
    frameLauncher(dt, gameFrames);
    return;
  }

  bool menuOpen = menu.open();
  if (menuOpen) {
    MenuInput mi = menuInput();
    // Menu button closes the menu from the top level
    if (menu.top() && !menu.capturing) menu.update(mi, (float)dt);
    menuAnim = std::min(1.0f, menuAnim + (float)dt * 8.0f);
  } else {
    menuInput();  // keep edge state fresh
  }
  if (menuOpen && !menu.open()) {
    cfg.save(configPath);
    audio.flush();
  }

  music.update(dt, menu.open());
  bool emulate = !menu.open();
  if (rewinding && (!rewindHeld || !emulate)) endRewind();
  if ((!cfg.rewind || achLocked()) && rewind.snapshots()) rewind.clear();
  if (net.connected() && snes && netGameFrames(dt, gameFrames, emulate)) {
    // (online: both machines' frames in lockstep, see app_net.cpp)
  } else if (emulate && rewindHeld && stepRewind(gameFrames)) {
    // (showing the past)
  } else if (emulate) {
    // Level select: the game is run to the chosen level out of sight (black,
    // sound off), as fast as it goes for most of each refresh.
    bool loading = game->levelLoading();
    int runs = loading ? 1000 : gameFrames * (fastForward ? 4 : 1);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; i++) {
      if (loading && (!game->levelLoading() || std::chrono::steady_clock::now() - start > std::chrono::milliseconds(12)))
        break;
#ifdef _WIN32
      if (at.on && at.padPos < at.pads.size() / 2) {  // (the recording mode's controllers)
        snes->input[0] = at.pads[2 * at.padPos];
        snes->input[1] = at.pads[2 * at.padPos + 1];
        at.padPos++;
      }
#endif
      // (Of the frames a pass runs, only the last is drawn: a device that
      // falls behind (a Galaxy A12: 30 fps, the sound stuttering) runs two a
      // pass, and drawing is most of a frame's work. The game keeps its speed
      // and its sound; the picture changes less often. Fast forward too.)
      snes->ppu.skipRender = !loading && i < runs - 1;
      game->beforeFrame();
      snes->runFrame();
      game->afterFrame();
      if (game->coopPick >= 0) {  // (a title screen's co-op choice: the setting follows)
        cfg.coop = game->coopPick != 0;
        game->coopPick = -1;
        cfg.save(configPath);
      }
      audioBuf.clear();
      snes->takeAudio(audioBuf);
      bool mute = (cfg.muteUnfocused && !focused) || fastForward || loading;
      if (!mute) audio.push(audioBuf.data(), audioBuf.size() / 2, Snes::kSampleRate);
      fpsFrames++;
      if (pendingSave >= 0 && !game->coopMidTurn()) saveState(pendingSave);
      if (cfg.rewind && !achLocked() && !loading && !game->coopMidTurn() && (++rewindCount & 1) == 0)
        rewind.push(snes->saveState());
    }
    snes->ppu.skipRender = false;
    achFrame();
  }
  sramTimer += dt;
  if (sramTimer > 5.0 && snes->sramDirty) {
    sramTimer = 0;
    saveSram();
  }
  fpsTimer += dt;
  if (fpsTimer >= 0.5) {
    fps = fpsFrames / fpsTimer;
    fpsFrames = 0;
    fpsTimer = 0;
  }
}

void App::render() {
  Canvas* uiCtx = video.canvas();
  const RectF all = rectF(0, 0, (float)video.width(), (float)video.height());
  if (inLauncher || !snes) {
    video.clear();
    video.beginUi();
    menu.setAccent(launcher.accent());
    {
      const GameId g = Launcher::cardGame(launcher.card());
      menu.setGameArt(g == GameId::SMB1 ? 1 : g == GameId::SMBLL ? 2 : g == GameId::SMB2 ? 3 : g == GameId::SMB3 ? 4 : 0);
    }
    launcher.setPixelAspect(cfg.pixelAspectCrt ? 8.0 / 7.0 : 1.0);
    launcher.draw(menu, video.width(), video.height());
    if (menu.open()) menu.draw(video.width(), video.height(), menuAnim);
    char fpsText[32] = "";
    if (cfg.showFps) snprintf(fpsText, sizeof fpsText, "%.1f FPS", fps);
    menu.drawOsd(video.width(), video.height(), fpsText, netStatus());
    // (black frame insertion: drawn, then covered, so the timers run on)
    if (blackFrame && uiCtx) uiCtx->fillRect(all, colorF(0, 0, 0, 1));
    video.endUi();
    return;
  }
  IRect dest;
  int ext, srcW;
  computeLayout(&dest, &ext, &srcW);
  // High frame rate: on the refreshes after a game frame, the in-between
  // picture nearest the refresh's moment, the last frame k / K of the way to
  // this one (shown a frame late: what lies between two frames is known once
  // the second is drawn); the frame itself at its own moment.
  const uint32_t* pic = snes->ppu.frame;
  const int subs = snes->ppu.subFrames;
  if (subs > 1 && !menu.open() && hfrT >= 0 && snes->ppu.subTimed) {
    // (drawn at the refreshes' own moments: the one nearest this, or the frame)
    double best = std::fabs(1.0 - hfrT);
    for (int k = 1; k < subs; k++)
      if (std::fabs(snes->ppu.subAt[k - 1] - hfrT) < best)
        best = std::fabs(snes->ppu.subAt[k - 1] - hfrT), pic = snes->ppu.subFrame[k - 1];
    if (hfrPrevOk && hfrPrev.size() == (size_t)(kLineW * kFrameH) && hfrT < best) pic = hfrPrev.data();
  } else if (subs > 1 && !menu.open() && hfrT >= 0) {
    int k = std::clamp((int)std::lround(hfrT * subs), 1, subs);
    if (k < subs) pic = snes->ppu.subFrame[k - 1];
  }
  // (the last frame's picture, shown for a moment near it, at its own left
  // margin: shown at this frame's, a room sliding to the middle jumped back
  // and forth)
  int from = kMaxExt - ext;
  if (pic == hfrPrev.data()) from = std::clamp(kMaxExt - hfrPrevExt, 0, kLineW - srcW);
  video.upload(pic + from, srcW, snes->ppu.frameHeight(), kLineW);
  VideoParams vp;
  vp.srcW = srcW;
  vp.srcH = snes->ppu.frameHeight();
  vp.dest = dest;
  vp.filter = cfg.filter;
  vp.scanlines = cfg.scanlines / 100.0f;
  vp.curve = cfg.crtCurve / 100.0f;
  vp.colorProfile = cfg.colorProfile;
  vp.brightness = cfg.brightness / 100.0f;
  vp.contrast = cfg.contrast / 100.0f;
  vp.saturation = cfg.saturation / 100.0f;
  vp.gamma = cfg.gamma / 100.0f;
  vp.colorblind = cfg.colorblind;
  vp.temperature = cfg.colorTemp / 100.0f;
  vp.pillarbox = cfg.pillarbox;
  vp.frame = snes->frameCount();
  bool loading = game->levelLoading();
  if (loading) vp.fade = 0;
  video.draw(vp);
  video.beginUi();
  // The menu's accent: the running game's colour (and its heading's art).
  switch (game->current()) {
    case GameId::SMB1: menu.setGameArt(1); break;
    case GameId::SMBLL: menu.setGameArt(2); break;
    case GameId::SMB2: menu.setGameArt(3); break;
    case GameId::SMB3: menu.setGameArt(4); break;
    default: menu.setGameArt(0); break;
  }
  switch (game->current()) {
    case GameId::SMB1: menu.setAccent(colorF(0.93f, 0.33f, 0.27f)); break;    // Mario red
    case GameId::SMBLL: menu.setAccent(colorF(0.36f, 0.76f, 0.40f)); break;   // Luigi green
    case GameId::SMB2: menu.setAccent(colorF(0.66f, 0.52f, 0.95f)); break;    // dream purple
    case GameId::SMB3: menu.setAccent(colorF(0.96f, 0.62f, 0.20f)); break;    // SMB3 orange
    default: menu.setAccent(Launcher::gameAccent(playing)); break;                 // (its file panel)
  }
  if (cfg.pillarbox == 2) {
    // dark frame: subtle border around the picture
    RectF r = rectF((float)dest.left - 2, (float)dest.top - 2, (float)dest.right + 2, (float)dest.bottom + 2);
    if (Canvas* c = video.canvas()) c->strokeRect(r, 3.0f, colorF(0.25f, 0.25f, 0.3f, 1.0f));
  }
#ifndef _WIN32
  touch.draw(video.canvas(), cfg.touchOpacity / 100.0f);
#endif
  if (menu.open()) menu.draw(video.width(), video.height(), menuAnim);
  char fpsText[48] = "";
  if (cfg.showFps && net.connected() && net.rttMs() >= 0)
    snprintf(fpsText, sizeof fpsText, "%.1f FPS  %d ms", fps, net.rttMs());
  else if (cfg.showFps)
    snprintf(fpsText, sizeof fpsText, "%.1f FPS", fps);
  const std::string netLine = netStatus();
  menu.drawOsd(video.width(), video.height(), fpsText,
               rewinding ? "<< Rewind" : fastForward ? ">> Fast Forward" : loading ? "Loading..." : netLine);
  if (blackFrame && uiCtx) uiCtx->fillRect(all, colorF(0, 0, 0, 1));
  video.endUi();
}

