// User settings, persisted as an INI file next to the executable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class DisplayMode { Windowed = 0, Borderless = 1, Exclusive = 2 };

enum AspectMode {
  kAspect4x3 = 0,
  kAspect16x10,
  kAspect16x9,
  kAspect21x9,
  kAspectAuto,  // match the display
  kAspectCount
};

enum FilterMode {
  kFilterNearest = 0,
  kFilterSharp,
  kFilterBilinear,
  kFilterCrtScanlines,
  kFilterCrtAperture,
  kFilterLcd,
  kFilterCount
};

enum ColorProfile {
  kColorOriginal = 0,
  kColorVivid,
  kColorCrt,
  kColorMuted,
  kColorGrayscale,
  kColorSepia,
  kColorPocket,     // 4-shade green handheld look
  kColorVirtual,    // red monochrome
  kColorNight,      // warm, reduced blue light
  kColorConsole,    // SNES video DAC response (darker mid-tones)
  kColorCount
};

// Bindable actions. The first 12 map to SNES buttons.
enum Action {
  kActUp = 0, kActDown, kActLeft, kActRight,
  kActA, kActB, kActX, kActY, kActL, kActR, kActStart, kActSelect,
  kActMenu, kActSaveState, kActLoadState, kActFastForward, kActFullscreen, kActScreenshot,
  kActRewind,
  kActCount
};

// Players at once: the console's two controllers, and three more the co-op
// engines read (see SmasGame::pads); local or online (see netplay.h).
constexpr int kMaxPlayers = 5;

// A binding: keyboard VK code or joypad control.
struct Binding {
  enum Type : uint8_t { None = 0, Key, PadButton, PadAxisPos, PadAxisNeg, PadHat };
  Type type = None;
  uint16_t code = 0;  // VK / button index / axis index / hat direction
  bool operator==(const Binding& o) const { return type == o.type && code == o.code; }
};

struct Config {
  // Display
  DisplayMode displayMode = DisplayMode::Borderless;
  std::wstring monitor;  // device name, empty = primary
  int windowScale = 4;
  int aspect = kAspectAuto;
  bool pixelAspectCrt = true;  // 8:7 pixels (as seen on a TV) vs square pixels
  int scaling = 0;             // 0 fit, 1 integer, 2 stretch
  bool vsync = true;           // off: present at the game's own 60 fps (for G-SYNC / FreeSync)
  bool showFps = false;
  bool blackFrames = false;    // black refreshes between the game's frames (120/180/240 Hz displays)
  bool pacingLog = false;      // display.pacing_log: frame pacing to the log every 2 s (diagnosis)
  bool highFrameRate = false;  // in-between pictures on the refreshes between the game's frames (90 Hz+ displays)
  int lastGame = 0;            // the launcher's card chosen last
  bool launcherAllStars = true;   // the launcher shows one card: Super Mario All-Stars (its own game select)
  bool launcherCovers = false;    // the cards show the player's covers (covers folder) in place of live previews
  bool launcherBackground = false;  // the launcher's background is the player's picture (covers/background.png)
  bool launcherCoverFill = false;  // covers fill their frames (trimmed); else shown whole (the default)
  bool hudAnchor = true;
  bool dimWrap = true;         // SMB2: dim the margins of areas that wrap round
  int pillarbox = 1;           // 0 black, 1 blurred game, 2 dark frame
  int widescreenEnemies = 1;   // 1 = enemies active in full view
  bool coop = false;           // SMB1, LL, SMB3: two players at once, Luigi on controller 2
  bool nesBrickBounce = false; // SMB1, LL: a brick broken from below sends Mario back down, as on the NES
  // Graphics
  int filter = kFilterSharp;
  int scanlines = 45;          // 0-100
  int crtCurve = 0;            // 0-100
  std::wstring slangPreset;    // a slang shader preset (.slangp) in place of the filters; relative to the exe's folder, or empty
  bool noSpriteLimit = false;
  // Color
  int colorProfile = kColorOriginal;
  int brightness = 100, contrast = 100, saturation = 100, gamma = 100;
  int colorblind = 0;          // 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
  int colorTemp = 0;           // -100 cool .. +100 warm
  int playerPalette = 0;       // character palette variant: Mario's
  int luigiPalette = 0;        // ... Luigi's
  // Audio
  int volume = 80;
  bool muteUnfocused = false;
  int audioLatency = 64;       // ms
  int musicVolume = 100;       // the games' music, percent (0: off, the sound effects kept)
  int sfxVolume = 100;         // ... and their sound effects
  std::wstring msuPack;        // an MSU-1 music pack: its folder in the msu folder, or empty (see app/msupack.h)
  int msuVolume = 100;         // its music's volume, percent
  int interpolation = 0;       // 0 = hardware gaussian
  // Input
  int selectMode = 0;          // 0 = Select opens the menu, 1 = tap: game Select / hold: menu
  bool checkUpdates = true;    // once a day, the launcher looks up the latest release on GitHub
  std::string updateCheckedOn; // (the day it last did, "2026-11-06", and what it found, "v1.2")
  std::string updateLatest;
  int menuStyle = 0;           // the menus' look: 0 Storybook, 1 Retro, 2 Themed Dark, 3 Classic (Menu::Style)
  bool confirmRight = false;   // menus: the right face button confirms, the bottom one backs out (Nintendo's way)
  int deadzone = 35;           // percent
  int touchMode = 0;           // on-screen controls (touch screens): 0 while touch is used, 1 always, 2 never
  int touchOpacity = 70;       // percent
  bool rewind = true;          // hold Rewind to step back through the last minute
  bool fastForward = true;     // hold Fast Forward to run at 4x
  // Achievements count (cheats, level select, fast forward and rewind are off meanwhile)
  bool achievements = true;
  int padIndex[kMaxPlayers] = {0, 1, 2, 3, 4};  // which physical pad drives each player
  Binding keys[kMaxPlayers][kActCount];
  Binding pad[kMaxPlayers][kActCount];
  // Online play
  int netPort = 7845;          // the UDP port to host on
  std::string netAddress;      // the host joined last
  int netDelay = 0;            // input delay in frames (0: from the ping)
  int netLocalPlayers = 1;     // players on this computer in online play (their players 1..N controls)
  // Cheats (bit flags per cheat id)
  uint64_t cheats = 0;

  void setDefaults();
  bool load(const std::wstring& path);
  bool save(const std::wstring& path) const;
};

const char* actionName(int a);
std::string bindingName(const Binding& b);
