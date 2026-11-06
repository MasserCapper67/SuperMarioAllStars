// Super Mario All-Stars port: desktop Linux (the Steam Deck and other PCs),
// as an AppImage. The SDL window and event loop, frame pacing, the ROM picker;
// the video, sound, input and touch layers are the Android build's
// (platform/android: SDL and OpenGL ES 3), the application itself app/app.cpp.
// Settings, saves and the ROM live in $XDG_DATA_HOME/SuperMarioAllStars
// (~/.local/share/SuperMarioAllStars).
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"

std::string g_fontDir;

namespace {
// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps, else 0 (the Steam Deck LCD: 60 Hz; the OLED: 90 Hz).
int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.025 ? k : 0;
}

// The folder the program is in (inside an AppImage: its usr/bin) and the one
// the AppImage file itself is in (where a "roms" folder may be put beside it).
std::string programDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string p(buf);
  size_t s = p.find_last_of('/');
  return s == std::string::npos ? "." : p.substr(0, s);
}

std::string appImageDir() {
  const char* a = getenv("APPIMAGE");
  if (!a || !*a) return programDir();
  std::string p(a);
  size_t s = p.find_last_of('/');
  return s == std::string::npos ? "." : p.substr(0, s);
}

std::string dataDir() {
  std::string base;
  if (const char* x = getenv("XDG_DATA_HOME"); x && *x) base = x;
  else if (const char* h = getenv("HOME"); h && *h) base = std::string(h) + "/.local/share";
  else base = ".";
  return base + "/SuperMarioAllStars";
}

void makeDirs(const std::string& path) {  // (mkdir -p)
  for (size_t i = 1; i <= path.size(); i++)
    if (i == path.size() || path[i] == '/') makeDir(widenUtf8(path.substr(0, i)));
}

bool hasRomExt(const std::wstring& name) {
  std::wstring lower = name;
  for (wchar_t& c : lower) c = (wchar_t)towlower(c);
  for (const wchar_t* ext : {L".sfc", L".smc", L".swc", L".fig"})
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ext) == 0) return true;
  return false;
}

std::vector<std::wstring> romDirs(const std::wstring& data) {
  std::vector<std::wstring> dirs = {pathJoin(data, L"roms"), data};
  const std::wstring beside = widenUtf8(appImageDir());
  dirs.push_back(beside);
  dirs.push_back(pathJoin(beside, L"roms"));
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) dirs.push_back(widenUtf8(cwd));
  return dirs;
}

// The ROM picker: the desktop's file dialog (zenity, else kdialog), run on a
// thread so the window keeps drawing; its answer is collected in pollPick.
std::mutex g_pickLock;
std::string g_picked;
std::atomic<int> g_pickState{0};  // 0 idle, 1 open, 2 answered

std::string runDialog(const char* cmd) {
  std::string out;
  FILE* p = popen(cmd, "r");
  if (!p) return out;
  char buf[4096];
  while (fgets(buf, sizeof buf, p)) out += buf;
  if (pclose(p) != 0) out.clear();
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

// A file given (picked, dropped or on the command line): copied into the
// data folder's "roms" if it is the ROM. True if it was.
bool takeRom(const std::wstring& data, const std::string& path) {
  std::vector<uint8_t> d;
  if (path.empty() || !readAll(widenUtf8(path), d) || !isSmasRom(d, nullptr)) return false;
  std::wstring dir = pathJoin(data, L"roms");
  makeDirs(narrowUtf8(dir));
  return writeAll(pathJoin(dir, L"Super Mario All-Stars (USA).sfc"), d.data(), d.size());
}
}  // namespace

void openUrl(const char* url) { SDL_OpenURL(url); }

bool httpGet(const char* url, std::string& out) {
  out.clear();
  // (the AppImage's own libraries kept out of the system curl's way)
  const std::string cmd = std::string("env -u LD_LIBRARY_PATH curl -fsSL --max-time 10 ") +
                          "-H 'Accept: application/vnd.github+json' -A SuperMarioAllStars-port '" + url +
                          "' 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return false;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0 && out.size() < (1u << 20)) out.append(buf, n);
  return pclose(p) == 0 && !out.empty();
}
void openFolder(const std::wstring& path) { SDL_OpenURL(("file://" + narrowUtf8(path)).c_str()); }

void App::applyDisplayMode() {
  if (!window) return;
  const bool full = cfg.displayMode != DisplayMode::Windowed;
  const bool now = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
  if (full == now) return;
  SDL_SetWindowFullscreen(window, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (!full) {
    int h = 224 * std::clamp(cfg.windowScale, 2, 4), w = h * 16 / 9;
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  }
}

bool App::findRom(std::wstring* out) {
  for (const std::wstring& dir : romDirs(exeDir)) {
    std::vector<std::wstring> names;
    if (!listFiles(dir, names)) continue;
    for (const std::wstring& n : names) {
      if (!hasRomExt(n)) continue;
      std::wstring p = pathJoin(dir, n);
      std::vector<uint8_t> data;
      if (readAll(p, data) && isSmasRom(data, nullptr)) {
        *out = p;
        return true;
      }
    }
  }
  return false;
}

void App::pickRom() {
  if (pickingRom || g_pickState != 0) return;
  pickingRom = true;
  g_pickState = 1;
  if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) SDL_MinimizeWindow(window);
  std::thread([]() {
    std::string r = runDialog(
        "zenity --file-selection --title='Choose the Super Mario All-Stars (USA) ROM' "
        "--file-filter='SNES ROM | *.sfc *.smc *.SFC *.SMC' 2>/dev/null");
    if (r.empty())
      r = runDialog(
          "kdialog --title 'Choose the Super Mario All-Stars (USA) ROM' --getopenfilename \"$HOME\" "
          "'*.sfc *.smc *.SFC *.SMC' 2>/dev/null");
    std::lock_guard<std::mutex> lock(g_pickLock);
    g_picked = r;
    g_pickState = 2;
  }).detach();
}

void App::pollPick() {
  if (!pickingRom || g_pickState != 2) return;
  std::string picked;
  {
    std::lock_guard<std::mutex> lock(g_pickLock);
    picked.swap(g_picked);
    g_pickState = 0;
  }
  pickingRom = false;
  if (window) SDL_RestoreWindow(window);
  if (picked.empty()) {
    menu.notify("No file chosen - or put the ROM in " + narrowUtf8(pathJoin(exeDir, L"roms")), 5.0f);
    return;
  }
  if (!takeRom(exeDir, picked)) {
    menu.notify("That isn't the Super Mario All-Stars (USA) ROM", 4.0f);
    return;
  }
  romPoll = 0;
  pollRom(0);
}

// (The covers and background go in the covers folder: Options > Launcher >
// Open Covers Folder; no picker here.)
void App::pickArt(int) {}
void App::pollArtPick() {}

// Taps and drags (the Steam Deck's screen; a mouse acts as one finger): on the
// launcher's cards and buttons, the menus' rows; the on-screen pad's Menu button.
void App::touchFrame() {
  touch.layout((float)video.width(), (float)video.height());
  touch.inGame = !inLauncher && snes && !menu.open() && !(game && game->levelLoading());
  touch.visible = touchActive ? cfg.touchMode != 2 : cfg.touchMode == 1;
  touch.showFF = cfg.fastForward && !achLocked();
  touch.showRewind = cfg.rewind && !achLocked();
  if (touch.takePressed(TouchControls::BtnMenu) && touch.inGame) openMenu();
  float dy = touch.takeDrag(), x, y;
  bool tapped = touch.takeTap(&x, &y);
  if (menu.open()) {
    if (dy != 0) menu.drag(dy);
    if (tapped) menu.tap(x, y);
  } else if (inLauncher && tapped) {
    launcherTap = launcher.tap(x, y);
  }
}

bool App::init() {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  // (the mouse taps the launcher and menus)
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
  SDL_SetHint(SDL_HINT_APP_NAME, "Super Mario All-Stars");
  // Sound through PulseAudio first (on PipeWire systems, the Steam Deck's,
  // its PulseAudio side): a tester's crackling was far less that way than
  // through SDL's PipeWire output. SDL_AUDIODRIVER still overrides it.
  if (!getenv("SDL_AUDIODRIVER")) SDL_SetHint(SDL_HINT_AUDIODRIVER, "pulseaudio,pipewire,alsa");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  const std::string data = dataDir();
  makeDirs(data);
  exeDir = widenUtf8(data);
  g_fontDir = programDir();
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, data dir %s, program dir %s", data.c_str(), g_fontDir.c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  shotDir = pathJoin(exeDir, L"screenshots");
  makeDir(saveDir);
  makeDir(pathJoin(exeDir, L"roms"));
  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  const bool full = cfg.displayMode != DisplayMode::Windowed;
  int wh = 224 * std::clamp(cfg.windowScale, 2, 4);
  window = SDL_CreateWindow("Super Mario All-Stars", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, wh * 16 / 9, wh,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  if (!window) {
    logf("window failed: %s", SDL_GetError());
    return false;
  }
  if (!video.init(window)) {
    logf("video init failed: %s", SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario All-Stars",
                             "OpenGL ES 3.0 could not be started (check your graphics drivers).", window);
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  if (!audio.init(cfg.audioLatency)) menu.notify("Audio device unavailable");
  else
    logf("audio: %s, %d Hz, %d-frame chunks, latency %d ms", SDL_GetCurrentAudioDriver(), audio.rate(),
         audio.deviceFrames(), cfg.audioLatency);
  input.init();
  menu.setCanvas(video.canvas());
  menu.touchUi = true;
  touchActive = false;

  const bool haveRom = loadRom();
  logf(haveRom ? "rom loaded: %s" : "rom not found%s", narrow(romPath).c_str());
  achStore.load(pathJoin(exeDir, L"achievements.ini"));
  launcher.setVersion(kAppVersion);
  launcher.setAllStarsOnly(cfg.launcherAllStars);
  launcher.selectGame(Launcher::cardGame(cfg.lastGame));
  playing = Launcher::cardGame(cfg.lastGame);
  if (haveRom) romReady();
  applyConfig();
  inLauncher = true;
  if (!achStore.noticeShown) {
    achStore.noticeShown = true;
    achStore.save();
    if (cfg.achievements) achWelcome();
  }
  return true;
}

void App::shutdown() {
  saveSram();
  cfg.save(configPath);
  music.shutdown();
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  audio.shutdown();
  input.shutdown();
  video.shutdown();
  if (window) SDL_DestroyWindow(window);
  window = nullptr;
  if (g_log) fclose(g_log);
  g_log = nullptr;
  SDL_Quit();
}

// One pass per display refresh (SDL_GL_SwapWindow waits for it, VSync). The
// refresh rate is the median of recent intervals; on a whole multiple of the
// game's 60.1 fps each game frame gets a steady number of refreshes, else the
// game time owed decides.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double period = 1.0 / Snes::kFrameRate;
  float intervals[48] = {};
  int nIntervals = 0;
  Uint64 statsAt = last;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      const float W = (float)video.width(), H = (float)video.height();
      switch (e.type) {
        case SDL_QUIT: running = false; break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            video.resize(w, h);
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            focused = false;
            input.clearKeys();
          } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            focused = true;
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          // (Alt+Enter: windowed or fullscreen, as on Windows)
          if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) {
            cfg.displayMode = cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
            applyDisplayMode();
            break;
          }
          int vk = vkFromScancode(e.key.keysym.scancode);
          if (vk) input.onKey(vk, e.type == SDL_KEYDOWN);
          if (e.type == SDL_KEYDOWN) touchActive = false;
          break;
        }
        case SDL_TEXTINPUT: textTyped += e.text.text; break;  // (the address editor: see pageTextEntry)
        case SDL_DROPFILE:
          if (e.drop.file) {
            if (takeRom(exeDir, e.drop.file)) {
              romPoll = 0;
              pollRom(0);
            } else {
              menu.notify("That isn't the Super Mario All-Stars (USA) ROM", 4.0f);
            }
            SDL_free(e.drop.file);
          }
          break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: input.rescanDevices(); break;
        case SDL_CONTROLLERBUTTONDOWN: touchActive = false; break;
        case SDL_CONTROLLERAXISMOTION:
          if (std::abs(e.caxis.value) > 20000) touchActive = false;
          break;
        case SDL_FINGERDOWN:
          // (a real screen shows the on-screen pad; the mouse, acting as a finger, doesn't)
          if (e.tfinger.touchId != SDL_MOUSE_TOUCHID) touchActive = true;
          touch.onDown((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        case SDL_FINGERMOTION: touch.onMove((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_FINGERUP: touch.onUp((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        default: break;
      }
    }
    if (!running) break;
    pollPick();
    Uint64 now = SDL_GetPerformanceCounter();
    double dt = (double)(now - last) / freq;
    last = now;
    intervals[nIntervals++ % 48] = (float)dt;
    double hz = 60.0;
    if (nIntervals >= 48) {
      float d[48];
      std::copy(intervals, intervals + 48, d);
      std::nth_element(d, d + 24, d + 48);
      if (d[24] > 0.002f) hz = 1.0 / d[24];
    } else {
      SDL_DisplayMode m;
      int di = SDL_GetWindowDisplayIndex(window);
      if (SDL_GetCurrentDisplayMode(di < 0 ? 0 : di, &m) == 0 && m.refresh_rate > 0) hz = m.refresh_rate;
    }
    if (pacing.mode != kPaceVsync || lockedRefreshes(hz) != pacing.perFrame) {
      pacing = Pacing();
      pacing.mode = kPaceVsync;
      pacing.perFrame = lockedRefreshes(hz);
      logf("pacing: %.2f Hz display, %d refreshes per frame (0 = uneven)", hz, pacing.perFrame);
    }
    pacing.hz = hz;
    int frames = 0;
    int vb = std::max(1, (int)std::lround(dt * hz));  // refreshes since the last pass
    if (vb > 6) {  // stalled (a dialog, the window dragged): resume without fast-forwarding
      vb = 0;
      pacing.vblanks = 0;
      pacing.accum = 0;
      frames = 1;
    }
    if (pacing.perFrame) {
      pacing.vblanks += vb;
      while (pacing.vblanks >= pacing.perFrame) {
        pacing.vblanks -= pacing.perFrame;
        frames++;
      }
    } else {
      pacing.accum += vb / hz;
      while (pacing.accum >= period) {
        pacing.accum -= period;
        frames++;
      }
    }
    frames = std::min(frames, 3);
    frame(dt, frames);
    render();
    video.present();
    // (sound running dry, for reports: logged every 10 s when it happened)
    if (now - statsAt > freq * 10) {
      statsAt = now;
      int dry = 0, low = 0;
      audio.takeStats(&dry, &low);
      if (dry > 0)
        logf("audio: ran dry %d times in 10 s (lowest queue %d ms; latency %d ms)", dry, low * 1000 / audio.rate(),
             cfg.audioLatency);
    }
  }
}

std::string platformClipboardText() {
  char* t = SDL_GetClipboardText();
  std::string s = t ? t : "";
  SDL_free(t);
  return s;
}

int main(int argc, char** argv) {
  SDL_SetMainReady();
  std::unique_ptr<App> app = std::make_unique<App>();
  g_app = app.get();
  // --host [port] / --join address: straight into online play. Anything else:
  // a ROM given on the command line, or dropped on the AppImage (kept for next time).
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--host") {
      app->netArgHost = 0;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') app->netArgHost = atoi(argv[++i]);
    } else if (a == "--join" && i + 1 < argc) {
      app->netArgJoin = argv[++i];
    } else {
      takeRom(widenUtf8(dataDir()), argv[i]);
    }
  }
  int rc = 0;
  if (!app->init()) rc = 1;
  else app->run();
  app->shutdown();
  g_app = nullptr;
  return rc;
}
