// Super Mario All-Stars port: the Windows side (window, main loop and frame
// pacing, display modes, slang shaders, test modes, entry point).
#include "app/app.h"
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include "app/sys.h"
#include "core/png.h"

namespace {
std::wstring dirOf(const std::wstring& p) { return pathDir(p); }
LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace

bool App::findRom(std::wstring* out) {
  std::vector<std::wstring> dirs;
  dirs.push_back(exeDir);
  dirs.push_back(exeDir + L"\\roms");
  std::wstring d = exeDir;
  for (int i = 0; i < 3; i++) {
    d = dirOf(d);
    dirs.push_back(d);
  }
  wchar_t cwd[MAX_PATH];
  GetCurrentDirectoryW(MAX_PATH, cwd);
  dirs.push_back(cwd);
  for (auto& dir : dirs) {
    for (const wchar_t* pat : {L"\\*.sfc", L"\\*.smc", L"\\*.swc", L"\\*.fig"}) {
      WIN32_FIND_DATAW fd;
      HANDLE fh = FindFirstFileW((dir + pat).c_str(), &fd);
      if (fh == INVALID_HANDLE_VALUE) continue;
      do {
        std::wstring p = dir + L"\\" + fd.cFileName;
        std::vector<uint8_t> data;
        if (readAll(p, data) && isSmasRom(data, nullptr)) {
          FindClose(fh);
          *out = p;
          return true;
        }
      } while (FindNextFileW(fh, &fd));
      FindClose(fh);
    }
  }
  return false;
}

void App::applyDisplayMode() {
  monitors = enumerateMonitors();
  // Leave exclusive fullscreen first: it is re-entered below on the chosen
  // monitor (the monitor may have changed, or Windows may have dropped it).
  if (video.exclusive()) video.setExclusive(false, nullptr);
  MonitorInfo mon = findMonitor(monitors, cfg.monitor);
  if (cfg.displayMode == DisplayMode::Windowed) {
    SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    int mw = mon.work.right - mon.work.left, mh = mon.work.bottom - mon.work.top;
    double aspect = 16.0 / 9.0;
    switch (cfg.aspect) {
      case kAspect4x3: aspect = 256.0 * (cfg.pixelAspectCrt ? 8.0 / 7.0 : 1.0) / 224.0; break;
      case kAspect16x10: aspect = 16.0 / 10.0; break;
      case kAspect16x9: aspect = 16.0 / 9.0; break;
      case kAspect21x9: aspect = 21.0 / 9.0; break;
      default: break;
    }
    int ch = 224 * cfg.windowScale;
    int cw = (int)std::lround(ch * aspect);
    while ((cw > mw - 40 || ch > mh - 60) && ch > 224) {
      ch -= 224 / 2;
      cw = (int)std::lround(ch * aspect);
    }
    RECT r{0, 0, cw, ch};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = mon.work.left + (mw - ww) / 2, y = mon.work.top + (mh - wh) / 2;
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, mon.rect.left, mon.rect.top, mon.rect.right - mon.rect.left,
                 mon.rect.bottom - mon.rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (cfg.displayMode == DisplayMode::Exclusive) {
      video.resize();
      bool ok = video.setExclusive(true, mon.handle);
      logf("exclusive fullscreen: %s (hr 0x%08lx, %s)", ok ? "on" : "failed", (unsigned long)video.lastError(),
           video.diag().c_str());
      if (!ok) {
        menu.notify("Exclusive fullscreen unavailable - using borderless");
      }
    }
  }
  video.resize();
}

void App::applySlang(bool force) {
  std::wstring full = cfg.slangPreset;
  if (!full.empty() && !(full.size() > 1 && (full[1] == L':' || full[0] == L'\\'))) full = exeDir + L"\\" + full;
  if (!force && full == slangLoaded) return;
  std::string err;
  if (video.setSlang(full, &err)) {
    slangLoaded = full;
    if (!full.empty()) logf("slang preset loaded: %s", narrow(full).c_str());
  } else {
    logf("slang preset failed: %s: %s", narrow(full).c_str(), err.c_str());
    // (librashader's own errors are long: a short reason here, the whole of it in the log)
    std::string why = err.find("NotFound") != std::string::npos ? "file not found"
                      : err.find("librashader.dll") != std::string::npos ? err
                                                                          : "it couldn't be compiled (see smas_port.log)";
    menu.notify("Shader " + narrow(std::filesystem::path(full).filename().wstring()) + " not loaded: " + why, 4.0f);
    cfg.slangPreset.clear();
    video.setSlang(L"", nullptr);
    slangLoaded.clear();
  }
}

// The slang presets found under the 'shaders' folder next to the exe (e.g. the
// libretro slang-shaders collection), grouped by folder, and Off.
std::shared_ptr<MenuPage> App::pageSlang() {
  auto p = std::make_shared<MenuPage>();
  p->title = "Slang Shader";
  p->wide = true;
  p->rebuild = [this](MenuPage& pg) {
    pg.items.clear();
    auto pick = [this](std::wstring rel) {
      cfg.slangPreset = std::move(rel);
      applySlang();
      menu.pop();
    };
    pg.items.push_back(MenuItem::action("Off (use the built-in filter)", [pick]() { pick(L""); }));
    if (!video.slangAvailable()) {
      pg.items.push_back(MenuItem::info("librashader.dll not found",
                                        "Slang shaders need librashader.dll next to the game's exe."));
      return;
    }
    namespace fs = std::filesystem;
    const fs::path root = fs::path(exeDir) / L"shaders";
    std::vector<fs::path> found;
    std::error_code ec;
    if (fs::is_directory(root, ec))
      for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && it->path().extension() == L".slangp") found.push_back(it->path());
      }
    if (found.empty()) {
      pg.items.push_back(MenuItem::info(
          "No presets found", "Put .slangp presets (with their shader files) in the 'shaders' folder next to the "
                              "game, e.g. the libretro slang-shaders collection."));
      return;
    }
    std::sort(found.begin(), found.end());
    fs::path lastDir;
    for (const fs::path& f : found) {
      fs::path rel = fs::relative(f, fs::path(exeDir), ec);
      fs::path dir = fs::relative(f.parent_path(), root, ec);
      if (dir != lastDir) {
        pg.items.push_back(MenuItem::header(dir.empty() || dir == L"." ? "shaders" : narrow(dir.wstring())));
        lastDir = dir;
      }
      std::wstring relW = rel.wstring();
      MenuItem it = MenuItem::action(narrow(f.stem().wstring()), [pick, relW]() { pick(relW); },
                                     "Some presets expect a 4:3 picture: with widescreen they can stretch or frame "
                                     "oddly.");
      if (relW == cfg.slangPreset) it.valueText = []() { return std::string("Active"); };
      pg.items.push_back(it);
    }
  };
  return p;
}

bool App::init(HINSTANCE inst) {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  exeDir = dirOf(path);
  g_log = _wfopen(pathJoin(exeDir, L"smas_port.log").c_str(), L"w");
  logf("starting, exe dir %s", narrow(exeDir).c_str());
  configPath = exeDir + L"\\smas_port.ini";
  saveDir = exeDir + L"\\saves";
  shotDir = exeDir + L"\\screenshots";
  CreateDirectoryW(saveDir.c_str(), nullptr);
  if (test.on || test.runSeconds > 0 || at.on) {
    cfg.setDefaults();
    if (!test.config.empty()) cfg.load(test.config);
    cfg.displayMode = test.displayMode >= 0 ? (DisplayMode)test.displayMode : DisplayMode::Windowed;
    cfg.vsync = test.sync >= 0 ? test.sync == 0 : test.runSeconds > 0;
    if (at.on) cfg.achievements = false;  // (the recording mode: its settings from its script)
    configPath = saveDir + L"\\test_config.ini";
  } else if (!cfg.load(configPath)) {
    cfg.save(configPath);
  }

  const bool haveRom = loadRom();
  if (haveRom) {
    logf("rom loaded: %s", narrow(romPath).c_str());
  } else if (test.on || test.runSeconds > 0 || at.on) {
    logf("rom load failed");
    return false;
  } else {
    logf("rom not found: the launcher says so");
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = L"SmasPortWindow";
  RegisterClassExW(&wc);
  hwnd = CreateWindowExW(0, wc.lpszClassName, kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                         nullptr, nullptr, inst, nullptr);
  if (!hwnd) {
    logf("window creation failed: %lu", GetLastError());
    return false;
  }
  if (!video.init(hwnd)) {
    logf("video init failed");
    MessageBoxW(hwnd, L"Could not initialise Direct3D 11.", kAppTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  applySlang(true);
  input.init(hwnd);
  if (!audio.init(cfg.audioLatency)) menu.notify("Audio device unavailable");
  menu.setCanvas(video.canvas());
  achStore.load(exeDir + L"\\achievements.ini");
  launcher.setVersion(kAppVersion);
  launcher.setAllStarsOnly(cfg.launcherAllStars);
  launcher.selectGame(Launcher::cardGame(cfg.lastGame));
  playing = Launcher::cardGame(cfg.lastGame);
  if (haveRom) romReady();  // (the console was just switched on)
  applyConfig();
  applyDisplayMode();
  if (test.on || at.on) {
    RECT r{0, 0, test.w, test.h};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    video.resize();
  }
  ShowWindow(hwnd, at.on ? SW_HIDE : SW_SHOW);
  // Timing tests need a visible window (hidden windows are not vsynced).
  if (test.runSeconds > 0) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  // The launcher first, unless a game was asked for (--game; test runs).
  inLauncher = !test.on && test.runSeconds <= 0;
  if (inLauncher && haveRom && !startGame.empty()) {
    static const wchar_t* kIds[Launcher::kCards + 1] = {L"smb1", L"smbll", L"smb2", L"smb3", L"allstars"};
    for (int i = 0; i <= Launcher::kCards; i++)
      if (_wcsicmp(startGame.c_str(), kIds[i]) == 0) launchGame(Launcher::cardGame(i));
  }
  if (at.on) inLauncher = true;
  if (!test.on && !at.on && test.runSeconds <= 0 && !achStore.noticeShown) {
    achStore.noticeShown = true;
    achStore.save();
    if (cfg.achievements) achWelcome();
  }
  return true;
}

// Rebuild the video device and everything created from it (the menu's
// Direct2D brush), then restore the display mode. Throttled: `now` in seconds.
void App::recoverVideo(double now) {
  if (now - lastVideoRecovery < 0.5) return;
  lastVideoRecovery = now;
  logf("video device lost (0x%08lx): recreating", (unsigned long)video.lastError());
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  video.shutdown();
  if (!video.init(hwnd)) {
    video.shutdown();
    return;
  }
  menu.setCanvas(video.canvas());
  applySlang(true);
  // The new swap chain already fits the window; only exclusive fullscreen has
  // to be entered again (windowed and borderless windows stay as they are).
  if (cfg.displayMode == DisplayMode::Exclusive) applyDisplayMode();
  menu.notify("The display driver was reset");
  logf("video recreated %dx%d", video.width(), video.height());
}

void App::shutdown() {
  if (!test.on && test.runSeconds <= 0) {
    saveSram();
    cfg.save(configPath);
  }
  music.shutdown();
  launcher.releaseGpu();
  menu.setCanvas(nullptr);
  audio.shutdown();
  input.shutdown();
  video.shutdown();
  if (hwnd) DestroyWindow(hwnd);
}

// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps (within what the audio resampler can absorb), else 0.
static int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.004 ? k : 0;
}

// Frame pacing. The game runs at 60.1 fps.
// * VSync on: one loop pass per display refresh. Refreshes are counted, not
//   timed, so each game frame is shown for a steady number of refreshes:
//   exactly k on 60/120/240 Hz displays (game speed follows the display, the
//   audio resampler absorbs the <0.4% difference), a regular pattern on
//   others (e.g. 2,3,2,3,2 at 144 Hz).
// * VSync off: a high-resolution timer presents one frame every 1/60.1 s
//   immediately (G-SYNC / FreeSync displays then refresh at 60.1 Hz).
void App::run() {
  timeBeginPeriod(1);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);  // present promptly after the vblank
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto seconds = [&]() { return std::chrono::duration<double>(clock::now() - start).count(); };
  const double period = 1.0 / Snes::kFrameRate;
  double last = seconds();
  double logTimer = 0, monTimer = 0;
  int qMin = 1 << 30, qMax = 0;
  double dtMax = 0;
  int sinceFrame = 0;  // refreshes since the last game frame
  int passes = 0;
  UINT lastPresentCount = 0, lastSyncRefresh = 0;
  MonitorInfo mon = monitorForWindow(monitors, hwnd);
  while (running) {
    int mode = cfg.vsync ? kPaceVsync : kPaceTimer;
    double hz = mon.hz;
    if (mode != pacing.mode || std::fabs(hz - pacing.hz) > 0.01) {
      pacing = Pacing();
      pacing.mode = mode;
      pacing.hz = hz;
      pacing.perFrame = lockedRefreshes(hz);
      pacing.deadline = seconds();
      logf("pacing: %s, %.2f Hz display, %d refreshes per frame (0 = uneven)", mode == kPaceVsync ? "vsync" : "timer",
           hz, pacing.perFrame);
    }

    // The timer-paced mode waits for its slot here; VSync waits just before
    // presenting (below).
    if (mode != kPaceVsync) {
      double wait = pacing.deadline - seconds();
      if (wait > 0.002 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((wait - 0.001) * 1e7);
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 100);
      }
      while (seconds() < pacing.deadline) YieldProcessor();  // last millisecond: be exact
    }

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) running = false;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!running) break;
    if (pendingDisplayMode) {
      pendingDisplayMode = false;
      applyDisplayMode();
    }
    double now = seconds();
    double dt = now - last;
    last = now;
    dtMax = std::max(dtMax, dt);
    monTimer += dt;
    if (monTimer > 0.5) {
      monTimer = 0;
      MonitorInfo m = monitorForWindow(monitors, hwnd);
      if (m.handle != mon.handle) video.refreshOutput();  // moved to another display
      mon = m;
    }

    // How many game frames are due.
    int frames = 0;
    if (mode == kPaceVsync) {
      double rp = 1.0 / hz;
      // Refreshes since the last pass. Each pass waits for a vertical blank:
      // the time since the last, rounded, at least one. (Counted from the
      // time with the remainder carried on, a remainder sitting at half a
      // refresh turned the passes' small jitter into 0, 2, 0, 2 ...: the
      // in-between pictures' moments jumped a refresh back and forth.)
      // The waits not a whole number of refreshes apart: they aren't the
      // display's refreshes (a variable-refresh display, G-SYNC / FreeSync,
      // waits for the next picture; a refresh rate reported wrong): the game
      // runs on the clock then, the remainder carried on, none of the time
      // lost. (Counted in rounded refreshes, it ran at a third to two thirds
      // of its speed for players with G-SYNC and VSync on.)
      const int whole = std::max(1, (int)std::lround(dt / rp));
      const bool offBeat = vblankWaited && std::fabs(dt - whole * rp) > 0.3 * rp;
      if (offBeat && !pacing.offBeatLogged && ++pacing.offBeatCount > 120) {
        pacing.offBeatLogged = true;
        logf("pacing: the vertical-blank waits don't follow the %.1f Hz refresh (variable refresh?): game time from "
             "the clock", hz);
      }
      if (!offBeat) pacing.offBeatCount = 0;
      // (and over each 60 passes: the refreshes counted against the time gone
      // by; more than 2% apart (waits a little off each time, a refresh rate
      // reported wrong), the clock for the next 60)
      pacing.sumReal += dt;
      pacing.sumCounted += whole * rp;
      if (++pacing.nWin >= 60) {
        const bool drift = std::fabs(pacing.sumCounted - pacing.sumReal) > 0.02 * pacing.sumReal;
        if (drift && !pacing.clockMode)
          logf("pacing: %.0f refreshes counted in %.2f s at %.1f Hz: game time from the clock",
               pacing.sumCounted / rp, pacing.sumReal, hz);
        pacing.clockMode = drift;
        pacing.sumReal = pacing.sumCounted = 0;
        pacing.nWin = 0;
      }
      int vb;
      if (vblankWaited && !offBeat && !pacing.clockMode) {
        vb = whole;
        pacing.vbResidual = 0;
      } else {
        pacing.vbResidual += dt;
        vb = (int)std::floor(pacing.vbResidual / rp + 0.5);
        pacing.vbResidual -= vb * rp;
      }
      if (cfg.pacingLog) pacing.vbSeen[std::min(std::max(vb, 0), 3)]++;
      if (dt > 0.25) {  // stalled (window drag, system hiccup): resume without fast-forwarding
        vb = 0;
        pacing.vbResidual = 0;
        pacing.vblanks = 0;
        pacing.accum = 0;
        frames = 1;
      }
      sinceFrame += vb;
      if (pacing.perFrame) {
        pacing.vblanks += vb;
        while (pacing.vblanks >= pacing.perFrame) {
          pacing.vblanks -= pacing.perFrame;
          frames++;
        }
      } else {
        pacing.accum += vb * rp;
        while (pacing.accum >= period) {
          pacing.accum -= period;
          frames++;
        }
      }
      frames = std::min(frames, 5);  // (catching up: slow waits need several frames a pass)
    } else if (const int subs = hfrSubFrames(); subs > 1) {
      // High frame rate without VSync (G-SYNC / FreeSync): a pass per
      // in-between picture, the game frame on every subs-th (see hfrT).
      const double step = period / subs;
      pacing.deadline += step;
      if (now - pacing.deadline > period * 3) {
        pacing.deadline = now + step;  // fell far behind: resync instead of racing
        hfrStep = subs - 1;
      }
      hfrStep = (hfrStep + 1) % subs;
      frames = hfrStep == 0 ? 1 : 0;
    } else {
      frames = 1;
      pacing.deadline += period;
      if (now - pacing.deadline > period * 3) {
        pacing.deadline = now + period;  // fell far behind: resync instead of racing
      } else {
        while (now >= pacing.deadline && frames < 3) {
          frames++;
          pacing.deadline += period;
        }
      }
    }
    if (frames > 0 && test.runSeconds > 0 && pacing.cadence.size() < 60) {
      pacing.cadence += (char)('0' + std::min(sinceFrame, 9));
      for (int i = 1; i < frames; i++) pacing.cadence += '0';
    }
    if (frames > 0) sinceFrame = 0;
    // High frame rate (90 Hz and up): in-between pictures, as many as the
    // refreshes a frame gets, drawn with each frame (see Ppu::subFrames).
    {
      const int subs = hfrSubFrames();
      // (a refresh shows the moment a frame back from the next: with VSync the
      // counted refreshes when locked; without, the pass)
      if (subs > 1 && mode == kPaceVsync && !pacing.perFrame) {
        // VSync at a rate that doesn't divide the frame (100, 144, 165 Hz):
        // the refreshes until the next frame fall at known moments, so the new
        // frame's in-between pictures are drawn at exactly those (a fixed k / K
        // would repeat or skip pictures: uneven motion).
        hfrT = pacing.accum / period;
        if (snes && frames > 0) {
          Ppu& p = snes->ppu;
          double at[8];
          int n = 0;
          for (double t = hfrT; t < 1.0 - 1e-6 && n < 8; t += 1.0 / (hz * period)) at[n++] = t;
          // At most 2 drawn with the frame: each costs about what the frame
          // does, and with 3 (144 Hz, co-op's fast CPU) the pass outlasted its
          // refresh and the picture stalled. A third is the one nearest the
          // last frame: that picture itself is shown for it (see hfrPrev), off
          // by under a sixth of a frame at 144 Hz.
          int skip = std::max(0, n - kHfrMaxDrawn);
          hfrPrev.assign(p.frame, p.frame + kLineW * kFrameH);
          hfrPrevExt = p.extL;  // (its left margin: a narrow room slides to the middle)
          hfrPrevOk = skip > 0 || n > 0;
          n -= skip;
          for (int i = 0; i < n && i < Ppu::kMaxSubFrames - 1; i++) p.subAt[i] = (float)at[skip + i];
          n = std::min(n, Ppu::kMaxSubFrames - 1);
          p.subFrames = std::max(n, 1) + 1;
          if (n == 0) p.subAt[0] = 1.0f;
          p.subTimed = true;
        }
      } else {
        if (snes) snes->ppu.subFrames = subs, snes->ppu.subTimed = false;
        hfrT = subs <= 1            ? -1
               : mode != kPaceVsync ? (hfrStep + 1.0) / subs
                                    : (pacing.vblanks + 1.0) / pacing.perFrame;
      }
    }
    // Black frame insertion: of the refreshes each game frame gets, the first
    // half show it and the rest are black (120 Hz: 1 + 1, 180 Hz: 1 + 2,
    // 240 Hz: 2 + 2), each picture lit as briefly as at 120 Hz.
    blackFrame = cfg.blackFrames && mode == kPaceVsync && pacing.perFrame >= 2 &&
                 pacing.vblanks >= std::max(1, pacing.perFrame / 2);

    if (cfg.pacingLog && test.runSeconds <= 0) {
      // Frame pacing as it happens (display.pacing_log=1 in the ini): how
      // many refreshes each pass saw (1 each is right with VSync), the
      // passes' spacing, and what Windows says reached the screen.
      Pacing& p = pacing;
      p.logPasses++;
      p.logFrames += frames;
      p.dtLo = std::min(p.dtLo, dt);
      p.dtHi = std::max(p.dtHi, dt);
      if (now >= p.logAt) {
        DXGI_FRAME_STATISTICS fs{};
        video.frameStats(&fs);
        if (p.logAt > 0)
          logf("pacing: %s %.2f Hz hfr=%d (%d per frame) | %d passes, %d game frames | refreshes per pass 0:%d 1:%d "
               "2:%d 3+:%d | pass %.1f-%.1f ms | presents shown %u, refreshes %u | %s",
               mode == kPaceVsync ? "vsync" : "timer", hz, cfg.highFrameRate ? 1 : 0, hfrSubFrames(), p.logPasses,
               p.logFrames, p.vbSeen[0], p.vbSeen[1], p.vbSeen[2], p.vbSeen[3], p.dtLo * 1000, p.dtHi * 1000,
               fs.PresentCount - p.logPresents, fs.SyncRefreshCount - p.logRefreshes,
               video.fullscreenNow() ? "fullscreen" : "windowed");
        p.logPresents = fs.PresentCount;
        p.logRefreshes = fs.SyncRefreshCount;
        p.logPasses = p.logFrames = 0;
        for (int& v : p.vbSeen) v = 0;
        p.dtLo = 1e9;
        p.dtHi = 0;
        p.logAt = now + 2.0;
      }
    }
    if (test.runSeconds > 0) {
      int q = audio.queuedFrames();
      qMin = std::min(qMin, q);
      qMax = std::max(qMax, q);
      passes++;
      if (now >= logTimer) {
        DXGI_FRAME_STATISTICS fs{};
        video.frameStats(&fs);
        logf("t=%.1f fps=%.1f passes=%d audioQueued=%d (min %d max %d) dtMax=%.1fms presents=%u refreshes=%u game=%s",
             now, fps, passes, q, qMin, qMax, dtMax * 1000, fs.PresentCount - lastPresentCount,
             fs.SyncRefreshCount - lastSyncRefresh, game->describe().c_str());
        lastPresentCount = fs.PresentCount;
        lastSyncRefresh = fs.SyncRefreshCount;
        passes = 0;
        logf("  cadence %s", pacing.cadence.c_str());
        pacing.cadence.clear();
        logTimer += 1.0;
        qMin = 1 << 30;
        qMax = 0;
        dtMax = 0;
      }
      if (now > test.runSeconds) break;
    }
    static bool padsLogged = false;
    if (!padsLogged && now > 0.5 && test.runSeconds > 0) {
      padsLogged = true;
      for (int i = 0; i < input.padCount(); i++) {
        const PadState* ps = input.pad(i);
        std::string held;
        for (int b = 0; b < kPadButtons; b++)
          if (ps->buttons[b]) held += " " + std::to_string(b);
        logf("pad %d: %s  held:%s", i, narrow(ps->name).c_str(), held.c_str());
      }
    }

    frame(dt, frames);
    if (test.loseDeviceAt > 0 && now >= test.loseDeviceAt) {
      test.loseDeviceAt = 0;
      video.simulateDeviceLost();
    }
    if (!video.ready()) {
      // The GPU was reset or removed: keep the game (and its sound) running
      // and rebuild the video layer, retrying until the driver is back.
      recoverVideo(now);
      Sleep(15);
      continue;
    }
    render();
    if (mode == kPaceVsync) {
      // One pass per real display refresh, presented right at the vertical
      // blank. Present(1) alone is not enough: drivers can override vsync (the
      // loop then runs unthrottled and flips land mid-screen as tearing), so
      // the frame is ready before the blank and flipped as soon as it starts.
      vblankWaited = video.waitForVBlank();
      if (!vblankWaited) Sleep(std::max(1, (int)(1000 / hz) - 1));
    }
    video.present(mode == kPaceVsync ? 1 : 0);
    // A hidden or minimised window is not throttled by the display: don't spin.
    if (mode == kPaceVsync && dt < 0.25 / hz) Sleep(1);
  }
  if (timer) CloseHandle(timer);
  timeEndPeriod(1);
}

namespace {
LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  App* a = g_app;
  switch (msg) {
    case WM_CLOSE:
      if (a) a->running = false;
      return 0;
    case WM_SIZE:
      if (a && a->video.width()) a->video.resize();
      return 0;
    case WM_ACTIVATE:
      if (a) {
        a->focused = LOWORD(wp) != WA_INACTIVE || a->at.on;
        if (!a->at.on) a->input.clearKeys();
        // Windows drops exclusive fullscreen on Alt+Tab; take it back when the
        // window is active again.
        if (a->focused && a->cfg.displayMode == DisplayMode::Exclusive && a->video.exclusive() &&
            !a->video.fullscreenNow())
          a->pendingDisplayMode = true;
      }
      break;
    case WM_SETCURSOR:
      if (a && LOWORD(lp) == HTCLIENT && a->inLauncher && !a->menu.open() && a->mouseShown) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        SetCursor(LoadCursorW(nullptr, a->launcher.overLink((float)pt.x, (float)pt.y) ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      if (a && LOWORD(lp) == HTCLIENT && a->cfg.displayMode != DisplayMode::Windowed) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_SYSKEYDOWN:
      if (wp == VK_RETURN && (lp & (1 << 29)) && a) {
        a->cfg.displayMode =
            a->cfg.displayMode == DisplayMode::Windowed ? DisplayMode::Borderless : DisplayMode::Windowed;
        a->applyDisplayMode();
        return 0;
      }
      if (wp == VK_F4) break;
      if (a) a->input.onKey((int)wp, true);
      return 0;
    case WM_SYSKEYUP:
      if (a) a->input.onKey((int)wp, false);
      return 0;
    case WM_KEYDOWN:
      if (a) {
        a->input.onKey((int)wp, true);
        if (a->mouseShown && a->cfg.displayMode != DisplayMode::Windowed) SetCursor(nullptr);
        a->mouseShown = false;
      }
      return 0;
    case WM_MOUSEMOVE: {
      // (a real move: not the one Windows sends as the window appears or the pointer is shown)
      static LPARAM last = -1;
      if (a && a->inLauncher && last != -1 && lp != last) a->mouseShown = true;
      last = lp;
      break;
    }
    case WM_LBUTTONDOWN:
      if (a && a->inLauncher && !a->menu.open()) {
        a->mouseShown = true;
        a->launcherTap = a->launcher.tap((float)(short)LOWORD(lp), (float)(short)HIWORD(lp));
      }
      return 0;
    case WM_KEYUP:
      if (a) a->input.onKey((int)wp, false);
      return 0;
    case WM_CHAR:  // (typed text, for the address editor: see pageTextEntry)
      if (a && wp >= 32 && wp < 127) a->textTyped.push_back((char)wp);
      return 0;
    case WM_DEVICECHANGE:
      if (a) a->input.rescanDevices();
      break;
    case WM_DISPLAYCHANGE:
      if (a) {
        a->monitors = enumerateMonitors();
        a->video.refreshOutput();
      }
      break;
    case WM_ERASEBKGND:
      return 1;
  }
  return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

// Test mode: render a frame (optionally with a menu page open) and save the
// composited back buffer as PNG. Used to verify the presentation pipeline.
void App::runTest() {
  const std::string tp = narrow(test.menuPage);
  if (tp.rfind("launcher", 0) == 0) {
    // The launcher after test.frames refreshes (its previews running), with
    // "launcher-options" / "-achievements" / "-music" / "-credits" / "-quit"
    // open over it, or "launcher-buttons": the Music Player button chosen.
    inLauncher = true;
    launcher.setRom(&romData, narrow(romPath.substr(romPath.find_last_of(L"\\/") + 1)), romExact, snes->sram,
                    sizeof(snes->sram));
    MenuInput none;
    for (int i = 0; i < test.frames; i++) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      launcher.update(none, 1.0f / 60);
      launcher.runPreviews(1, false, audio);
    }
    MenuInput down;
    down.down = true;
    if (tp == "launcher-buttons") launcher.update(down, 1.0f / 60);
    for (int i = 0; i < 30; i++) launcher.update(none, 1.0f / 60);
    if (tp == "launcher-options") menu.push(pageOptions());
    else if (tp == "launcher-achievements") menu.push(pageAchievements());
    else if (tp == "launcher-music") menu.push(pageMusicPlayer());
    else if (tp == "launcher-credits") menu.push(pageCredits());
    else if (tp == "launcher-quit") menu.confirm("Quit?", []() {});
    menuAnim = 1.0f;
    render();
    std::vector<uint32_t> px;
    int w, h;
    if (video.capture(px, &w, &h)) writePng(narrow(test.shot), px.data(), w, h, w);
    return;
  }
  if (tp.rfind("launch-", 0) == 0) {
    // A card's game started from the launcher ("launch-smb1" .. "-smb3"): the
    // loading run (as frame() does it), then test.frames frames.
    static const char* kIds[Launcher::kCards] = {"smb1", "smbll", "smb2", "smb3"};
    for (int i = 0; i < Launcher::kCards; i++)
      if (tp.substr(7) == kIds[i]) launchGame(Launcher::cardGame(i));
    int loads = 0;
    for (; game->levelLoading() && loads < 6000; loads++) {
      IRect dest;
      int ext, srcW;
      computeLayout(&dest, &ext, &srcW);
      game->beforeFrame();
      snes->runFrame();
      game->afterFrame();
      audioBuf.clear();
      snes->takeAudio(audioBuf);
    }
    logf("launch %s: loaded in %d frames (%s)", tp.c_str(), loads, game->describe().c_str());
  }
  if (!test.state.empty()) {
    std::vector<uint8_t> d;
    if (readAll(test.state, d) && snes->loadState(d)) game->onStateLoaded();
  }
  for (int i = 0; i < test.frames; i++) {
    IRect dest;
    int ext, srcW;
    computeLayout(&dest, &ext, &srcW);  // propagates the widescreen request
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    game->beforeFrame();
    snes->runFrame();
    game->afterFrame();
    audioBuf.clear();
    snes->takeAudio(audioBuf);
  }
  if (!test.menuPage.empty() && tp.rfind("launch-", 0) != 0) {
    menu.push(pageMain());
    std::string pg = narrow(test.menuPage);
    if (pg == "display") menu.push(pageDisplay());
    else if (pg == "graphics") menu.push(pageGraphics());
    else if (pg == "slang") {
      menu.push(pageGraphics());
      menu.push(pageSlang());
    }
    else if (pg == "color") menu.push(pageColor());
    else if (pg == "audio") menu.push(pageAudio());
    else if (pg == "controls") menu.push(pageControls());
    else if (pg == "p1pad") menu.push(pageBindings(0, true));
    else if (pg == "cheats") menu.push(pageCheats());
    else if (pg == "levels") menu.push(pageLevelSelect());
    else if (pg == "credits") menu.push(pageCredits());
    else if (pg == "sound") menu.push(pageSoundSelect());
    else if (pg == "music") menu.push(pageMusicPlayer());
    else if (pg == "options") menu.push(pageOptions());
    else if (pg == "achievements") menu.push(pageAchievements());
    else if (pg == "confirm") menu.confirm("Quit to desktop?", []() {}, "Progress is kept only if the game saved it.");
    else if (pg == "welcome") {
      menu.closeAll();
      achWelcome();
    } else if (pg == "banner") {
      menu.closeAll();
      if (const AchievementDef* d = findAchievement("smb1_top")) menu.achievement(d->name, d->desc);
      menu.notify("State 1 saved");
      for (int i = 0; i < 40; i++) menu.tickToasts(1.0f / 60);
    }
    if (pg == "musictest") {
      static const int kIds[5][3] = {{1, 2, 0}, {2, 6, 0x0d}, {1, 7, 0x11}, {0x10, 0x12, 0x15}, {2, 0x0a, 0x0c}};
      for (int b = 0; b < 5; b++)
        for (int k = 0; k < 3; k++) {
          if (!kIds[b][k]) continue;
          int peak;
          double rms;
          music.testRender(b, (uint8_t)kIds[b][k], 5.0, &peak, &rms);
          logf("music bank %d id %02X: peak %d rms %.0f", b, kIds[b][k], peak, rms);
        }
      menu.push(pageMusicPlayer());
    }
    if (pg == "musicall") {
      // QA: render every named track of every collection and flag silent ones.
      int silent = 0, total = 0;
      for (int b = 0; b < MusicPlayer::kBankCount; b++)
        for (const auto& t : MusicPlayer::tracks(b)) {
          int peak = 0;
          double rms = 0;
          music.testRender(b, t.id, 3.0, &peak, &rms);
          total++;
          bool quiet = peak < 500;
          silent += quiet;
          logf("track bank %d id %02X %-28s peak %5d rms %5.0f%s", b, t.id, t.name, peak, rms, quiet ? "  SILENT" : "");
        }
      logf("musicall: %d tracks, %d silent", total, silent);
    }
    menuAnim = 1.0f;
    for (int i = 0; i < 30; i++) music.update(1.0 / 60, true);
  }
  render();
  std::vector<uint32_t> px;
  int w, h;
  if (video.capture(px, &w, &h)) writePng(narrow(test.shot), px.data(), w, h, w);
}

std::string platformClipboardText() {
  std::string out;
  if (!OpenClipboard(g_app ? g_app->hwnd : nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
    if (const wchar_t* w = (const wchar_t*)GlobalLock(h)) {
      out = narrow(std::wstring(w));
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // UI thread (file dialog, D2D, XAudio2)
  static App app;
  g_app = &app;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; i++) {
    std::wstring a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
    if (a == L"--test-shot") {
      app.test.on = true;
      app.test.shot = next();
    } else if (a == L"--test-frames") app.test.frames = _wtoi(next().c_str());
    else if (a == L"--test-state") app.test.state = next();
    else if (a == L"--test-menu") app.test.menuPage = next();
    else if (a == L"--test-config") app.test.config = next();
    else if (a == L"--game") app.startGame = next();
    else if (a == L"--host") {  // --host [port]: straight into online play
      app.netArgHost = 0;
      if (i + 1 < argc && argv[i + 1][0] >= L'0' && argv[i + 1][0] <= L'9') app.netArgHost = _wtoi(argv[++i]);
    } else if (a == L"--join") app.netArgJoin = narrow(next());
    else if (a == L"--test-run") app.test.runSeconds = _wtof(next().c_str());
    else if (a == L"--test-device-lost") app.test.loseDeviceAt = _wtof(next().c_str());
    else if (a == L"--test-display") app.test.displayMode = std::clamp(_wtoi(next().c_str()), 0, 2);
    else if (a == L"--test-sync") app.test.sync = std::clamp(_wtoi(next().c_str()), 0, 1);  // 0 vsync, 1 timer
    else if (a == L"--autotest") {
      app.at.dir = next();
      std::wstring script = next();
      CreateDirectoryW(app.at.dir.c_str(), nullptr);
      app.at.on = app.atLoad(script);
    } else if (a == L"--test-size") {
      std::wstring s = next();
      swscanf(s.c_str(), L"%dx%d", &app.test.w, &app.test.h);
    }
  }
  if (argv) LocalFree(argv);
  if (!app.init(inst)) {
    app.shutdown();
    return 1;
  }
  if (app.test.runSeconds > 0) {
    if (!app.test.state.empty()) {
      std::vector<uint8_t> d;
      if (readAll(app.test.state, d) && app.snes->loadState(d)) app.game->onStateLoaded();
    }
    app.run();
    app.shutdown();
    CoUninitialize();
    return 0;
  }
  if (app.at.on) {
    app.runAutotest();
    app.running = false;
    app.shutdown();
    CoUninitialize();
    return 0;
  }
  if (app.test.on) {
    app.runTest();
    app.running = false;
    app.shutdown();
    CoUninitialize();
    return 0;
  }
  app.run();
  app.shutdown();
  CoUninitialize();
  return 0;
}
