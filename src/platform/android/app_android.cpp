// Super Mario All-Stars port: the Android side (SDL window and event loop, frame
// pacing, touch, app lifecycle, the ROM picker), as the Mickey & Donald
// Collection's. The application itself is app/app.cpp.
#include <SDL.h>
#include <SDL_syswm.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <algorithm>
#include <cmath>
#include "app/app.h"
#include "app/sys.h"
#include "app/vkeys.h"

namespace {
// Calls a static method of SmasActivity (the SDL activity) through JNI.
jint callActivityInt(const char* name) {
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return -2;
  jclass cls = env->GetObjectClass(act);
  jint r = -2;
  if (jmethodID m = env->GetStaticMethodID(cls, name, "()I")) r = env->CallStaticIntMethod(cls, m);
  env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
  return r;
}

void callActivityVoid(const char* name) {
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return;
  jclass cls = env->GetObjectClass(act);
  if (jmethodID m = env->GetStaticMethodID(cls, name, "()V")) env->CallStaticVoidMethod(cls, m);
  env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
}

// Tells Android the game runs at 60 fps (ANativeWindow_setFrameRate, Android
// 11+), so a phone with an adaptive display can run it at 60 Hz or a multiple.
void setFrameRate(SDL_Window* w) {
  typedef int32_t (*SetFrameRate)(ANativeWindow*, float, int8_t);
  static SetFrameRate fn = (SetFrameRate)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (fn && w && SDL_GetWindowWMInfo(w, &info) && info.subsystem == SDL_SYSWM_ANDROID && info.info.android.window)
    fn(info.info.android.window, (float)Snes::kFrameRate, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
}

// Refreshes per game frame when the display rate is a whole multiple of the
// game's 60.1 fps, else 0. (Phones' rates are 60, 90, 120 Hz and the like;
// the measured one wanders a little.)
int lockedRefreshes(double hz) {
  int k = (int)std::lround(hz / Snes::kFrameRate);
  return k >= 1 && std::fabs(hz / k - Snes::kFrameRate) / Snes::kFrameRate < 0.025 ? k : 0;
}

std::vector<std::wstring> romDirs(const std::wstring& data) {
  std::vector<std::wstring> dirs = {pathJoin(data, L"roms")};
  if (const char* ext = SDL_AndroidGetExternalStoragePath()) {
    dirs.push_back(widenUtf8(ext));
    dirs.push_back(pathJoin(widenUtf8(ext), L"roms"));
  }
  return dirs;
}
}  // namespace

void openUrl(const char* url) { SDL_OpenURL(url); }

bool httpGet(const char* url, std::string& out) {
  out.clear();
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();  // (attaches this thread)
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return false;
  jclass cls = env->GetObjectClass(act);
  if (jmethodID m = env->GetStaticMethodID(cls, "httpGet", "(Ljava/lang/String;)Ljava/lang/String;")) {
    jstring u = env->NewStringUTF(url);
    jstring r = (jstring)env->CallStaticObjectMethod(cls, m, u);
    if (r) {
      if (const char* c = env->GetStringUTFChars(r, nullptr)) {
        out = c;
        env->ReleaseStringUTFChars(r, c);
      }
      env->DeleteLocalRef(r);
    }
    env->DeleteLocalRef(u);
  }
  env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
  return !out.empty();
}
void openFolder(const std::wstring&) {}

// (Android has one full-screen window: the aspect and scaling settings apply as it's drawn)
void App::applyDisplayMode() {}

// The ROM: in the app's own "roms" folder (where the picker copies it), or its
// external files folder (Android/data/<app>/files, and a "roms" folder there).
bool App::findRom(std::wstring* out) {
  for (const std::wstring& dir : romDirs(exeDir)) {
    std::vector<std::wstring> names;
    if (!listFiles(dir, names)) continue;
    for (const std::wstring& n : names) {
      std::wstring lower = n;
      for (wchar_t& c : lower) c = (wchar_t)towlower(c);
      bool sfc = false;
      for (const wchar_t* ext : {L".sfc", L".smc", L".swc", L".fig"})
        sfc = sfc || (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ext) == 0);
      if (!sfc) continue;
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
  if (pickingRom) return;
  pickingRom = true;
  callActivityVoid("pickRoms");
}

// The picker runs over the app (paused meanwhile); when it closes, the copied
// files are looked through at once.
void App::pollPick() {
  if (!pickingRom) return;
  jint r = callActivityInt("pollPickResult");
  if (r == -1) return;
  pickingRom = false;
  if (r <= 0) return;
  romPoll = 0;
  pollRom(0);
  if (!snes) menu.notify("That isn't the Super Mario All-Stars ROM - see the game card", 4.0f);
}

// A cover or the background chosen with the system picker (copied into
// files/covers by SmasActivity.pickImage), read again once it's there.
void App::pickArt(int card) {
  if (pickingArt) return;
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return;
  jclass cls = env->GetObjectClass(act);
  if (jmethodID m = env->GetStaticMethodID(cls, "pickImage", "(Ljava/lang/String;)V")) {
    jstring name = env->NewStringUTF(Launcher::artName(card));
    env->CallStaticVoidMethod(cls, m, name);
    env->DeleteLocalRef(name);
    pickingArt = true;
  }
  env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
}

void App::pollArtPick() {
  if (!pickingArt) return;
  jint r = callActivityInt("pollImageResult");
  if (r == -1) return;
  pickingArt = false;
  if (r <= 0) return;
  applyLauncherArt(true);
  menu.notify("Picture added", 2.5f);
}

// Taps and drags: on the launcher's cards and buttons, the menus' rows; the
// on-screen pad's Menu button.
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
  SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
  SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // Back is a key (the menu, back), not "close the app"
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return false;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  window = SDL_CreateWindow("Super Mario All-Stars", 0, 0, 0, 0,
                            SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) return false;
  setFrameRate(window);

  const char* internal = SDL_AndroidGetInternalStoragePath();
  exeDir = widenUtf8(internal ? internal : ".");
  g_log = openFile(pathJoin(exeDir, L"smas_port.log"), "w");
  logf("starting, data dir %s", narrow(exeDir).c_str());
  configPath = pathJoin(exeDir, L"smas_port.ini");
  saveDir = pathJoin(exeDir, L"saves");
  const char* external = SDL_AndroidGetExternalStoragePath();
  shotDir = pathJoin(external ? widenUtf8(external) : exeDir, L"screenshots");
  msuDir = pathJoin(external ? widenUtf8(external) : exeDir, L"msu");  // (MSU-1 packs: copied in over USB)
  makeDir(saveDir);
  makeDir(pathJoin(exeDir, L"roms"));
  if (!cfg.load(configPath)) cfg.save(configPath);
  cfg.vsync = true;

  if (!video.init(window)) {
    logf("video init failed");
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Super Mario All-Stars", "OpenGL ES 3.0 could not be started.",
                             window);
    return false;
  }
  logf("video ok %dx%d", video.width(), video.height());
  if (!audio.init(cfg.audioLatency)) menu.notify("Audio device unavailable");
  input.init();
  menu.setCanvas(video.canvas());
  menu.touchUi = true;
  menu.keysHint = L"Tap to choose      Drag to scroll      Back or \x2190 to go back";
  launcher.setHint("Tap a game to choose it, again to play");

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

// One pass per display refresh (SDL_GL_SwapWindow waits for it). The refresh
// rate is the median of recent intervals; on a whole multiple of the game's
// 60.1 fps each game frame gets a steady number of refreshes, else the game
// time owed decides.
void App::run() {
  const Uint64 freq = SDL_GetPerformanceFrequency();
  Uint64 last = SDL_GetPerformanceCounter();
  const double period = 1.0 / Snes::kFrameRate;
  float intervals[48] = {};
  int nIntervals = 0;
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
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          if (e.key.repeat) break;
          int vk = vkFromScancode(e.key.keysym.scancode);
          if (vk) input.onKey(vk, e.type == SDL_KEYDOWN);
          if (e.key.keysym.scancode != SDL_SCANCODE_AC_BACK && e.type == SDL_KEYDOWN) touchActive = false;
          break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED: input.rescanDevices(); break;
        case SDL_CONTROLLERBUTTONDOWN: touchActive = false; break;
        case SDL_CONTROLLERAXISMOTION:
          if (std::abs(e.caxis.value) > 20000) touchActive = false;
          break;
        case SDL_TEXTINPUT: textTyped += e.text.text; break;
        case SDL_FINGERDOWN:
          touchActive = true;
          touch.onDown((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H);
          break;
        case SDL_FINGERMOTION: touch.onMove((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_FINGERUP: touch.onUp((int64_t)e.tfinger.fingerId, e.tfinger.x * W, e.tfinger.y * H); break;
        case SDL_APP_WILLENTERBACKGROUND:
          // (Android may end the app while it's in the background: keep the game's saves and the settings)
          saveSram();
          cfg.save(configPath);
          touch.releaseAll();
          input.clearKeys();
          break;
        case SDL_APP_DIDENTERFOREGROUND:
          last = SDL_GetPerformanceCounter();
          nIntervals = 0;
          pacing.vblanks = 0;
          pacing.accum = 0;
          audio.flush();
          setFrameRate(window);  // (the surface is new after a pause)
          break;
        default: break;
      }
    }
    if (!running) break;
    pollPick();
    pollArtPick();
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
      if (SDL_GetCurrentDisplayMode(0, &m) == 0 && m.refresh_rate > 0) hz = m.refresh_rate;
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
    if (vb > 6) {  // stalled: resume without fast-forwarding
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
  }
}

extern "C" int SDL_main(int, char*[]) {
  // (A new App each time: after Quit, Android keeps the process, and opening
  // the app again calls this again. The old one, kept, had been told to stop
  // (it closed at once) or crashed loading the ROM over its old game.)
  std::unique_ptr<App> app = std::make_unique<App>();
  g_app = app.get();
  int rc = 0;
  if (!app->init()) {
    rc = 1;
  } else {
    app->run();
  }
  app->shutdown();
  g_app = nullptr;
  return rc;
}

std::string platformClipboardText() {
  char* t = SDL_GetClipboardText();
  std::string s = t ? t : "";
  SDL_free(t);
  return s;
}
