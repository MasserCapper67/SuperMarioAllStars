// Super Mario All-Stars port: the application (launcher, game, menus,
// achievements), the same on every platform. The platform's part (window,
// main loop, video, sound, controllers) is in app_win.cpp (Windows) and
// platform/android/app_android.cpp (Android).
#pragma once
#include "app/updatecheck.h"
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/achstore.h"
#include "app/canvas.h"
#include "app/config.h"
#include "app/launcher.h"
#include "app/menu.h"
#include "app/music.h"
#include "app/netplay.h"
#include "core/rewind.h"
#include "core/snes.h"
#include "game/smas.h"
#ifdef _WIN32
#include "app/audio.h"
#include "app/input.h"
#include "app/monitors.h"
#include "app/video.h"
#else
#include "platform/android/audio_sdl.h"
#include "platform/android/input_sdl.h"
#include "platform/android/touch.h"
#include "platform/android/video_gles.h"
#endif

extern const wchar_t* kAppTitle;
extern const char* kAppVersion;

struct App {
#ifdef _WIN32
  HWND hwnd = nullptr;
#endif
  bool running = true;
  bool focused = true;
  Config cfg;
  std::wstring exeDir, configPath, saveDir, shotDir, romPath;
  UpdateCheck updater;      // (a newer release on GitHub? see updatecheck.h)
  std::string updateTag;    // the newer release's tag ("v1.2"), or empty
  bool updateManual = false, updateStarted = false, updateTold = false;
  std::wstring msuDir;     // MSU-1 music packs (see app/msupack.h); exeDir/msu unless the platform says
  std::wstring msuLoaded;  // the pack the console runs with (empty: none)
  std::string msuStatus;   // what became of the chosen pack, for the Audio page
  std::wstring msuBase;    // its tracks' path without "-N.pcm" (empty: none in use)
  std::unique_ptr<Snes> snes;
  std::unique_ptr<SmasGame> game;
  Video video;
  Audio audio;
  Input input;
  Menu menu;
  MusicPlayer music;
  // The launcher (see launcher.h): shown at start and after Return to
  // Launcher; the game's console isn't run meanwhile.
  Launcher launcher;
  bool inLauncher = false;
  GameId playing = GameId::SMB1;  // the game last started from it
  bool menuHintShown = false;
  std::vector<uint8_t> romData;
  bool romExact = true;
  double romPoll = 0;
  std::wstring startGame;  // --game smb1|smbll|smb2|smb3: straight into it
  bool blackFrame = false;  // this refresh is black (black frame insertion)
  int hfrStep = 0;          // high frame rate without VSync: the pass within the frame
  std::vector<uint32_t> hfrPrev;  // high frame rate, timed: the last frame's picture (shown for a moment near it)
  bool hfrPrevOk = false;
  int hfrPrevExt = 0;
  bool vblankWaited = false;  // the last pass waited for a vertical blank (VSync)
  static constexpr int kHfrMaxDrawn = 2;  // in-between pictures drawn with a frame, at most
  double hfrT = -1;         // high frame rate: this refresh's moment, from the last frame (0) to this one (1); -1 off
  int hfrSubFrames() const;  // in-between pictures per frame for this display (1: none)
#ifdef _WIN32
  std::vector<MonitorInfo> monitors;
  RECT windowedRect{};
  bool inSizeMove = false;
#else
  TouchControls touch;  // (on-screen controls: see platform/android/touch.h)
  bool touchActive = true;  // touch is the input in use (else a controller or keys)
  struct SDL_Window* window = nullptr;
  bool pickingRom = false;
  void pickRom();   // the system file picker, the file copied into the app (see SmasActivity)
  void pollPick();
  void pickArt(int card);  // a cover (card 0-4) or the background (-1) chosen with the system picker (Android)
  void pollArtPick();
  bool pickingArt = false;
  void touchFrame();  // taps and drags on the launcher and menus, the on-screen pad's buttons
#endif
  Launcher::Action launcherTap = Launcher::kNone;  // (a card or button tapped or clicked: see frameLauncher)
  bool mouseShown = false;  // (Windows: the mouse moved on the launcher, its pointer shown till a key)
  int lastWantExt = 0;  // widescreen margin chosen by the last usable layout
  bool pendingDisplayMode = false;  // re-apply the display mode from the main loop
  float menuAnim = 0;
  // input state
  uint16_t padState[2] = {0, 0};
  bool prevAction[2][kActCount] = {};
  bool prevSelect[2] = {false, false};
  bool prevMenuBtn[2] = {false, false};
  float selectHeld[2] = {0, 0};
  bool selectConsumed[2] = {false, false};
  float selectPulse[2] = {0, 0};  // seconds of game Select still to send
  bool menuWasOpen = false;
  bool prevMenuNav[8] = {};
  // timing
  struct Pacing {
    int mode = -1;        // kPace* in effect
    double hz = 60;       // display refresh
    int perFrame = 0;     // refreshes per game frame when locked (0 = not a whole number)
    double vbResidual = 0;
    int vblanks = 0;      // refreshes since the last game frame (locked mode)
    double accum = 0;     // game time owed (unlocked vsync)
    double deadline = 0;  // next frame time (timer-paced modes)
    std::string cadence;  // refreshes per game frame, for the test log
    // (display.pacing_log: refreshes per pass seen, 0 1 2 3+; the passes' spacing)
    int vbSeen[4] = {}, logPasses = 0, logFrames = 0;
    double dtLo = 1e9, dtHi = 0, logAt = 0;
    int offBeatCount = 0;           // passes in a row whose wait wasn't a whole number of refreshes
    bool offBeatLogged = false;
    double sumReal = 0, sumCounted = 0;  // (this window's time gone by, and its refreshes counted)
    int nWin = 0;
    bool clockMode = false;         // game time from the clock: the refreshes counted drifted
    unsigned logPresents = 0, logRefreshes = 0;
  } pacing;
  double fpsTimer = 0;
  int fpsFrames = 0;
  double fps = 0;
  int stateSlot = 1;
  double sramTimer = 0;
  bool sramFailed = false;  // the last save of game progress failed (reported once)
  std::vector<int16_t> audioBuf;
  bool fastForward = false;
  // Rewind: a snapshot every other frame (see stepRewind).
  Rewind rewind;
  bool rewindHeld = false;  // the Rewind button is held (and rewinding is on)
  bool rewinding = false;   // showing the past: the machine is at a snapshot (plus a frame to draw it)
  unsigned rewindCount = 0;
  bool stepRewind(int steps);
  void endRewind();
  void restoreSnapshot(const std::vector<uint8_t>& st);
  // automated test mode (--test-shot)
  struct Test {
    bool on = false;
    std::wstring shot, state, menuPage, config;
    int frames = 120;
    int w = 1920, h = 1080;
    std::string input;
    double runSeconds = 0;  // --test-run: drive the real main loop for N seconds
    double loseDeviceAt = 0;  // --test-device-lost: pretend the GPU was reset at N seconds
    int displayMode = -1;   // --test-display 0/1/2 (default windowed)
    int sync = -1;          // --test-sync 0/1/2
  } test;
  void runTest();
#ifdef _WIN32
  // The trailer's recording mode (--autotest <dir> <script>): see autotest_win.cpp.
  struct AtStep {
    int pass = 0;
    std::string cmd, arg;
  };
  struct Autotest {
    bool on = false;
    std::wstring dir;
    std::vector<AtStep> steps;
    size_t pos = 0;
    int pass = 0;
    int subs = 1;                  // pictures per game frame (hfr)
    std::vector<uint16_t> pads;    // the two controllers per game frame (pads)
    size_t padPos = 0;
    FILE* vid = nullptr;           // ffmpeg's input (record)
    FILE* pcm = nullptr;
    std::wstring recName;
    int recFrames = 0, recW = 0, recH = 0;
    int64_t recSamples = 0;
    std::string shot;
  } at;
  bool atLoad(const std::wstring& path);
  void atSet(const std::string& kv);
  void atRunSteps();
  void atStartRec(const std::string& name);
  void atStopRec();
  void atRecordPicture();
  void atRecordAudio(const int16_t* s, size_t frames);
  void runAutotest();
#endif

  // ---------------------------------------------------------------------
#ifdef _WIN32
  bool init(HINSTANCE inst);
#else
  bool init();
#endif
  void shutdown();
  void run();
  void frame(double dt, int gameFrames);
  void render();
  bool loadRom();
  bool findRom(std::wstring* out);
  void romReady();                    // a ROM was loaded: the console, music and previews
  void pollRom(double dt);            // looks for the ROM again while it is missing
  void launchGame(GameId g);
  void returnToLauncher();
  void applyMsu(std::vector<uint8_t>& rom);  // (loadRom: the chosen pack's patch and chip)
  void reloadForMsu();
  void checkForUpdate(bool manual);  // (manual: Check Now, which says what it found)
  void updateTick();                 // (each pass: a finished check's result)
  void applyUpdateTag(const std::string& tag);
  std::string updateVersion() const {  // ("v1.2": "1.2")
    return updateTag.size() > 1 && (updateTag[0] == 'v' || updateTag[0] == 'V') ? updateTag.substr(1) : updateTag;
  }                        // the chosen pack changed: the ROM again, outside a game
  void frameLauncher(double dt, int gameFrames);
  std::string blackFramesNote() const;
  void applyDisplayMode();
  void applyConfig();
  void applyLauncherArt(bool reload = false);  // the covers and background (see Launcher::setArt)
  // The slang shader preset in cfg.slangPreset loaded (or dropped) if it
  // isn't what the video layer has; force after the device was recreated.
#ifdef _WIN32
  void applySlang(bool force = false);
  std::wstring slangLoaded;  // the preset loaded now (full path; empty: none)
  std::shared_ptr<MenuPage> pageSlang();
#endif
  void computeLayout(IRect* dest, int* ext, int* srcW);
  void updateInput(double dt);
  void readPlayer(int p, uint16_t* snesPad, double dt);
  MenuInput menuInput();
  void openMenu();
  void saveSram();
  void loadSram();
  void saveState(int slot);
  int pendingSave = -1;  // (a save asked for in a co-op turn's middle: see saveState)
  void loadState(int slot);
#ifdef _WIN32
  void recoverVideo(double now);
  double lastVideoRecovery = -1e9;
#endif
  void screenshot();
  std::wstring statePath(int slot) const;
  // menu pages
  std::shared_ptr<MenuPage> pageMain();
  std::shared_ptr<MenuPage> pageDisplay();
  std::shared_ptr<MenuPage> pageGraphics();
  std::shared_ptr<MenuPage> pageColor();
  std::shared_ptr<MenuPage> pageAudio();
  std::shared_ptr<MenuPage> pageControls();
  std::shared_ptr<MenuPage> pageBindings(int player, bool pad);
  std::shared_ptr<MenuPage> pageCheats();
  std::shared_ptr<MenuPage> pageLevelSelect();
  std::shared_ptr<MenuPage> pageCredits();
  std::shared_ptr<MenuPage> pageSoundSelect();
  std::shared_ptr<MenuPage> pageMusicPlayer();
  std::shared_ptr<MenuPage> pageOptions();
  std::shared_ptr<MenuPage> pageAchievements();
  std::string bindingsText(int action) const;  // "F5 / LB": what does it (player 1)
  // achievements (see game/achievements.h)
  AchievementStore achStore;
  void achNewRun(bool counts);       // power-on / console reset (counts if on) or level select (doesn't)
  void achFrame();                   // after emulation: earned ones pop up and are stored
  void achAward(const std::string& id);
  void achCheckCollection();
  void setAchievements(bool on);
  MenuItem achievementsSwitch();     // Options, and the top of the Achievements page
  void achWelcome();                 // once, on the first start
  bool achLocked() const { return cfg.achievements; }  // cheats, level select, fast forward, rewind off
  // Online play (see netplay.h; the app's side in app_net.cpp)
  Netplay net;
  NetSettings netSet;           // the session's settings (the last sync point's)
  bool netHaveSet = false;
  bool netDirty = false;        // host: the console changed outside lockstep: a sync point when play resumes
  int netWantExt = 0;           // the widescreen margin this display wants (the host's goes out at a sync point)
  bool netSramForeign = false;  // player 2: the console's battery save is the host's (never written here)
  double netStall = 0;          // seconds this side has waited on the other's controller
  double netExtWait = 0;        // host: seconds the display has wanted another widescreen margin
  Netplay::Phase netLastPhase = Netplay::Phase::Off;
  int netArgHost = -1;          // --host [port] (0: the configured port); -1 not given
  std::string netArgJoin;       // --join address
  bool netArgsDone = false;
  std::string textTyped;        // text typed since the last pass (the platform's text events)
  std::string typedNow;         // ... this pass's, for the text entry page
  void netPass(double dt);
  bool netGameFrames(double dt, int gameFrames, bool emulate);
  void netHostSync();
  void netApplySync(NetSync& s);
  void netEnded(const std::string& why);
  NetSettings netCurrentSettings() const;
  void netApplySettings(const NetSettings& s);
  bool netBlocked(const char* what);  // player 2: what only the host does (told so); false: go ahead
  Netplay::Identity netIdentity() const;
  void netHost(int port);
  void netJoin(const std::string& address);
  std::string netStatus() const;   // the on-screen line (empty: nothing to say)
  std::string netSummary() const;  // the Online Play page's status row
  std::shared_ptr<MenuPage> pageOnline();
  std::shared_ptr<MenuPage> pageTextEntry(const std::string& title, const std::string& hint, const std::string& initial,
                                          bool digitsOnly, std::function<void(const std::string&)> done);
  // rebinding
  int bindPlayer = 0, bindAction = -1;
  bool bindPad = false;
  double bindTimer = 0;
  void startBind(int player, bool pad, int action);
  void updateBind(double dt);
};


extern App* g_app;
extern FILE* g_log;
void logf(const char* fmt, ...);
std::string narrow(const std::wstring& w);
bool readAll(const std::wstring& path, std::vector<uint8_t>& out);
bool writeAll(const std::wstring& path, const void* d, size_t n);
// Whether the data is the Super Mario All-Stars ROM (headered or not); `exact`:
// the USA dump the port is tuned for.
bool isSmasRom(const std::vector<uint8_t>& d, bool* exact);
// The system clipboard's text (UTF-8; empty if none). By the platform's file.
std::string platformClipboardText();
// The pacing modes (see App::run).
enum { kPaceVsync = 0, kPaceTimer = 1 };
