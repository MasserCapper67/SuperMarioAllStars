#include "config.h"
#include "core/fileio.h"
#include "app/canvas.h"
#include "app/sys.h"
#include "app/vkeys.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

// Standard pad button indices (Xbox positional layout)
enum {
  kPadBtnA = 0, kPadBtnB, kPadBtnX, kPadBtnY, kPadBtnLB, kPadBtnRB, kPadBtnBack, kPadBtnStart,
  kPadBtnLS, kPadBtnRS, kPadBtnUp, kPadBtnDown, kPadBtnLeft, kPadBtnRight, kPadBtnLT, kPadBtnRT
};

static const char* kActionNames[kActCount] = {
    "Up", "Down", "Left", "Right", "A", "B", "X", "Y", "L", "R", "Start", "Select",
    "Menu", "Save State", "Load State", "Fast Forward", "Fullscreen", "Screenshot", "Rewind",
};
static const char* kActionKeys[kActCount] = {
    "up", "down", "left", "right", "a", "b", "x", "y", "l", "r", "start", "select",
    "menu", "savestate", "loadstate", "fastforward", "fullscreen", "screenshot", "rewind",
};

const char* actionName(int a) { return (a >= 0 && a < kActCount) ? kActionNames[a] : "?"; }

static std::string keyName(int vk) {
  switch (vk) {
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Esc";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_LSHIFT: return "Left Shift";
    case VK_RSHIFT: return "Right Shift";
    case VK_SHIFT: return "Shift";
    case VK_LCONTROL: return "Left Ctrl";
    case VK_RCONTROL: return "Right Ctrl";
    case VK_CONTROL: return "Ctrl";
    case VK_LMENU: return "Left Alt";
    case VK_RMENU: return "Right Alt";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2: return "/";
    case VK_OEM_1: return ";";
    case VK_OEM_7: return "'";
    case VK_OEM_4: return "[";
    case VK_OEM_6: return "]";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_3: return "`";
    case VK_OEM_5: return "\\";
    default: break;
  }
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
  char buf[16];
  snprintf(buf, sizeof buf, "Key %d", vk);
  return buf;
}

std::string bindingName(const Binding& b) {
  static const char* padNames[16] = {"A", "B", "X", "Y", "LB", "RB", "View/Select", "Menu/Start",
                                     "L-Stick Press", "R-Stick Press", "D-Pad Up", "D-Pad Down",
                                     "D-Pad Left", "D-Pad Right", "LT", "RT"};
  static const char* axisNames[6] = {"L-Stick X", "L-Stick Y", "R-Stick X", "R-Stick Y", "Axis 5", "Axis 6"};
  switch (b.type) {
    case Binding::None: return "-";
    case Binding::Key: return keyName(b.code);
    case Binding::PadButton:
      if (b.code < 16) return padNames[b.code];
      return "Button " + std::to_string(b.code - 16 + 1);
    case Binding::PadAxisPos:
    case Binding::PadAxisNeg: {
      std::string n = b.code < 6 ? axisNames[b.code] : "Axis";
      if (b.code == 1 || b.code == 3) return n.substr(0, 7) + (b.type == Binding::PadAxisPos ? " Down" : " Up");
      return n.substr(0, 7) + (b.type == Binding::PadAxisPos ? " Right" : " Left");
    }
    case Binding::PadHat: {
      static const char* d[4] = {"Hat Up", "Hat Down", "Hat Left", "Hat Right"};
      return d[b.code & 3];
    }
  }
  return "?";
}

void Config::setDefaults() {
  *this = Config();
  for (int p = 0; p < 2; p++)
    for (int a = 0; a < kActCount; a++) keys[p][a] = pad[p][a] = Binding();
  auto K = [](int vk) { Binding b; b.type = Binding::Key; b.code = (uint16_t)vk; return b; };
  auto P = [](int btn) { Binding b; b.type = Binding::PadButton; b.code = (uint16_t)btn; return b; };
  // Player 1 keyboard
  keys[0][kActUp] = K(VK_UP);
  keys[0][kActDown] = K(VK_DOWN);
  keys[0][kActLeft] = K(VK_LEFT);
  keys[0][kActRight] = K(VK_RIGHT);
  keys[0][kActB] = K('Z');
  keys[0][kActA] = K('X');
  keys[0][kActY] = K('A');
  keys[0][kActX] = K('S');
  keys[0][kActL] = K('Q');
  keys[0][kActR] = K('W');
  keys[0][kActStart] = K(VK_RETURN);
  keys[0][kActSelect] = K(VK_RSHIFT);
  keys[0][kActMenu] = K(VK_ESCAPE);
  keys[0][kActSaveState] = K(VK_F5);
  keys[0][kActLoadState] = K(VK_F8);
  keys[0][kActFastForward] = K(VK_TAB);
  keys[0][kActFullscreen] = K(VK_F11);
  keys[0][kActScreenshot] = K(VK_F12);
  keys[0][kActRewind] = K(VK_BACK);
  // Player 2 keyboard
  keys[1][kActUp] = K('I');
  keys[1][kActDown] = K('K');
  keys[1][kActLeft] = K('J');
  keys[1][kActRight] = K('L');
  keys[1][kActB] = K('N');
  keys[1][kActA] = K('M');
  keys[1][kActY] = K('H');
  keys[1][kActX] = K('U');
  keys[1][kActL] = K('Y');
  keys[1][kActR] = K('O');
  keys[1][kActStart] = K('P');
  keys[1][kActSelect] = K('B');
  // Joypads (Xbox positional layout: bottom = SNES B, right = SNES A)
  for (int p = 0; p < 2; p++) {
    pad[p][kActUp] = P(kPadBtnUp);
    pad[p][kActDown] = P(kPadBtnDown);
    pad[p][kActLeft] = P(kPadBtnLeft);
    pad[p][kActRight] = P(kPadBtnRight);
    pad[p][kActB] = P(kPadBtnA);
    pad[p][kActA] = P(kPadBtnB);
    pad[p][kActY] = P(kPadBtnX);
    pad[p][kActX] = P(kPadBtnY);
    pad[p][kActL] = P(kPadBtnLB);
    pad[p][kActR] = P(kPadBtnRB);
    pad[p][kActStart] = P(kPadBtnStart);
    pad[p][kActSelect] = P(kPadBtnBack);
    pad[p][kActMenu] = P(kPadBtnBack);  // Select/View doubles as the menu button
  }
  pad[0][kActFastForward] = P(kPadBtnRT);
  pad[0][kActRewind] = P(kPadBtnLT);
  padIndex[0] = 0;
  padIndex[1] = 1;
}

namespace {
std::string narrow(const std::wstring& w) { return narrowUtf8(w); }
std::wstring widen(const std::string& s) { return widenUtf8(s); }
std::string encodeBinding(const Binding& b) {
  switch (b.type) {
    case Binding::Key: return "key:" + std::to_string(b.code);
    case Binding::PadButton: return "btn:" + std::to_string(b.code);
    case Binding::PadAxisPos: return "axis+:" + std::to_string(b.code);
    case Binding::PadAxisNeg: return "axis-:" + std::to_string(b.code);
    case Binding::PadHat: return "hat:" + std::to_string(b.code);
    default: return "none";
  }
}
Binding decodeBinding(const std::string& s) {
  Binding b;
  size_t c = s.find(':');
  if (c == std::string::npos) return b;
  std::string t = s.substr(0, c);
  int v = atoi(s.c_str() + c + 1);
  if (t == "key") b.type = Binding::Key;
  else if (t == "btn") b.type = Binding::PadButton;
  else if (t == "axis+") b.type = Binding::PadAxisPos;
  else if (t == "axis-") b.type = Binding::PadAxisNeg;
  else if (t == "hat") b.type = Binding::PadHat;
  b.code = (uint16_t)v;
  return b;
}
}  // namespace

bool Config::load(const std::wstring& path) {
  setDefaults();
  std::string text;
  if (!readText(path, text)) return false;
  std::istringstream f(text);
  std::map<std::string, std::string> kv;
  std::string line, section;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line[0] == '[') {
      section = line.substr(1, line.find(']') - 1);
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!k.empty() && k.back() == ' ') k.pop_back();
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    kv[section + "." + k] = v;
  }
  auto geti = [&](const char* k, int& out, int lo, int hi) {
    auto it = kv.find(k);
    if (it != kv.end()) {
      int v = atoi(it->second.c_str());
      if (v >= lo && v <= hi) out = v;
    }
  };
  auto getb = [&](const char* k, bool& out) {
    auto it = kv.find(k);
    if (it != kv.end()) out = atoi(it->second.c_str()) != 0;
  };
  int dm = (int)displayMode;
  geti("display.mode", dm, 0, 2);
  displayMode = (DisplayMode)dm;
  if (kv.count("display.monitor")) monitor = widen(kv["display.monitor"]);
  geti("display.window_scale", windowScale, 1, 10);
  geti("display.aspect", aspect, 0, kAspectCount - 1);
  getb("display.crt_pixel_aspect", pixelAspectCrt);
  geti("display.scaling", scaling, 0, 2);
  getb("display.vsync", vsync);
  if (kv.count("display.sync")) vsync = atoi(kv["display.sync"].c_str()) == 0;  // older builds
  getb("display.show_fps", showFps);
  getb("display.black_frames", blackFrames);
  getb("display.high_frame_rate", highFrameRate);
  getb("display.pacing_log", pacingLog);
  geti("launcher.last_game", lastGame, 0, 3);
  getb("launcher.allstars_only", launcherAllStars);
  getb("launcher.covers", launcherCovers);
  getb("launcher.background", launcherBackground);
  getb("launcher.cover_fill", launcherCoverFill);
  getb("display.hud_anchor", hudAnchor);
  getb("display.dim_wrap", dimWrap);
  geti("display.pillarbox", pillarbox, 0, 2);
  geti("display.widescreen_enemies", widescreenEnemies, 0, 1);
  getb("game.coop", coop);
  getb("game.nes_brick_bounce", nesBrickBounce);
  geti("netplay.port", netPort, 1, 65535);
  if (kv.count("netplay.address")) netAddress = kv["netplay.address"];
  geti("netplay.delay", netDelay, 0, 10);
  geti("graphics.filter", filter, 0, kFilterCount - 1);
  geti("graphics.scanlines", scanlines, 0, 100);
  geti("graphics.crt_curve", crtCurve, 0, 100);
  if (kv.count("graphics.slang_preset")) slangPreset = widen(kv["graphics.slang_preset"]);
  getb("graphics.no_sprite_limit", noSpriteLimit);
  geti("color.profile", colorProfile, 0, kColorCount - 1);
  geti("color.brightness", brightness, 50, 150);
  geti("color.contrast", contrast, 50, 150);
  geti("color.saturation", saturation, 0, 200);
  geti("color.gamma", gamma, 50, 150);
  geti("color.colorblind", colorblind, 0, 3);
  geti("color.temperature", colorTemp, -100, 100);
  geti("color.player_palette", playerPalette, 0, 15);
  geti("color.luigi_palette", luigiPalette, 0, 15);
  geti("audio.volume", volume, 0, 100);
  getb("audio.mute_unfocused", muteUnfocused);
  geti("audio.latency", audioLatency, 20, 200);
  geti("audio.music_volume", musicVolume, 0, 100);
  geti("audio.sfx_volume", sfxVolume, 0, 100);
  if (kv.count("audio.msu_pack")) msuPack = widen(kv["audio.msu_pack"]);
  geti("audio.msu_volume", msuVolume, 0, 100);
  geti("input.select_button", selectMode, 0, 1);
  getb("input.menu_confirm_right", confirmRight);
  geti("interface.menu_style", menuStyle, 0, 3);
  getb("updates.check", checkUpdates);
  if (kv.count("updates.checked_on")) updateCheckedOn = kv["updates.checked_on"];
  if (kv.count("updates.latest")) updateLatest = kv["updates.latest"];
  geti("input.deadzone", deadzone, 5, 90);
  geti("input.touch_mode", touchMode, 0, 2);
  geti("input.touch_opacity", touchOpacity, 10, 100);
  getb("input.rewind", rewind);
  getb("input.fast_forward", fastForward);
  getb("achievements.on", achievements);
  geti("input.p1_pad", padIndex[0], 0, 7);
  geti("input.p2_pad", padIndex[1], 0, 7);
  for (int p = 0; p < 2; p++) {
    for (int a = 0; a < kActCount; a++) {
      std::string k = "p" + std::to_string(p + 1) + "_keys." + kActionKeys[a];
      if (kv.count(k)) keys[p][a] = decodeBinding(kv[k]);
      k = "p" + std::to_string(p + 1) + "_pad." + kActionKeys[a];
      if (kv.count(k)) pad[p][a] = decodeBinding(kv[k]);
    }
  }
  if (kv.count("cheats.flags")) cheats = strtoull(kv["cheats.flags"].c_str(), nullptr, 16);
  return true;
}

bool Config::save(const std::wstring& path) const {
  std::ostringstream f;
  f << "; Super Mario All-Stars PC port settings\n";
  f << "[display]\n";
  f << "mode=" << (int)displayMode << "\n";
  f << "monitor=" << narrow(monitor) << "\n";
  f << "window_scale=" << windowScale << "\n";
  f << "aspect=" << aspect << "\n";
  f << "crt_pixel_aspect=" << pixelAspectCrt << "\n";
  f << "scaling=" << scaling << "\n";
  f << "vsync=" << vsync << "\n";
  f << "show_fps=" << showFps << "\n";
  f << "black_frames=" << blackFrames << "\n";
  f << "high_frame_rate=" << highFrameRate << "\n";
  if (pacingLog) f << "pacing_log=1\n";
  f << "hud_anchor=" << hudAnchor << "\n";
  f << "dim_wrap=" << dimWrap << "\n";
  f << "pillarbox=" << pillarbox << "\n";
  f << "widescreen_enemies=" << widescreenEnemies << "\n";
  f << "[game]\n";
  f << "coop=" << coop << "\n";
  f << "nes_brick_bounce=" << nesBrickBounce << "\n";
  f << "[netplay]\n";
  f << "port=" << netPort << "\n";
  f << "address=" << netAddress << "\n";
  f << "delay=" << netDelay << "\n";
  f << "[launcher]\n";
  f << "last_game=" << lastGame << "\n";
  f << "allstars_only=" << launcherAllStars << "\n";
  f << "covers=" << launcherCovers << "\n";
  f << "background=" << launcherBackground << "\n";
  f << "cover_fill=" << launcherCoverFill << "\n";
  f << "[graphics]\n";
  f << "filter=" << filter << "\n";
  f << "scanlines=" << scanlines << "\n";
  f << "crt_curve=" << crtCurve << "\n";
  f << "slang_preset=" << narrow(slangPreset) << "\n";
  f << "no_sprite_limit=" << noSpriteLimit << "\n";
  f << "[color]\n";
  f << "profile=" << colorProfile << "\n";
  f << "brightness=" << brightness << "\n";
  f << "contrast=" << contrast << "\n";
  f << "saturation=" << saturation << "\n";
  f << "gamma=" << gamma << "\n";
  f << "colorblind=" << colorblind << "\n";
  f << "temperature=" << colorTemp << "\n";
  f << "player_palette=" << playerPalette << "\n";
  f << "luigi_palette=" << luigiPalette << "\n";
  f << "[audio]\n";
  f << "volume=" << volume << "\n";
  f << "mute_unfocused=" << muteUnfocused << "\n";
  f << "latency=" << audioLatency << "\n";
  f << "music_volume=" << musicVolume << "\n";
  f << "sfx_volume=" << sfxVolume << "\n";
  f << "msu_pack=" << narrow(msuPack) << "\n";
  f << "msu_volume=" << msuVolume << "\n";
  f << "[updates]\n";
  f << "check=" << checkUpdates << "\n";
  f << "checked_on=" << updateCheckedOn << "\n";
  f << "latest=" << updateLatest << "\n";
  f << "[interface]\n";
  f << "menu_style=" << menuStyle << "\n";
  f << "[input]\n";
  f << "select_button=" << selectMode << "\n";
  f << "menu_confirm_right=" << confirmRight << "\n";
  f << "deadzone=" << deadzone << "\n";
  f << "touch_mode=" << touchMode << "\n";
  f << "touch_opacity=" << touchOpacity << "\n";
  f << "rewind=" << rewind << "\n";
  f << "fast_forward=" << fastForward << "\n";
  f << "p1_pad=" << padIndex[0] << "\n";
  f << "p2_pad=" << padIndex[1] << "\n";
  for (int p = 0; p < 2; p++) {
    f << "[p" << (p + 1) << "_keys]\n";
    for (int a = 0; a < kActCount; a++) f << kActionKeys[a] << "=" << encodeBinding(keys[p][a]) << "\n";
    f << "[p" << (p + 1) << "_pad]\n";
    for (int a = 0; a < kActCount; a++) f << kActionKeys[a] << "=" << encodeBinding(pad[p][a]) << "\n";
  }
  f << "[achievements]\non=" << achievements << "\n";
  char buf[32];
  snprintf(buf, sizeof buf, "%llx", (unsigned long long)cheats);
  f << "[cheats]\nflags=" << buf << "\n";
  std::string text = f.str();
  return writeFileSafely(path, text.data(), text.size());
}
