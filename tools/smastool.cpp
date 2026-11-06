// Headless runner / debugger used during development and reverse engineering.
// Usage: smastool <rom> <script-file | -c "cmd; cmd; ...">
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <algorithm>
#include "core/snes.h"
#include "core/fileio.h"
#include "core/png.h"
#include "core/rewind.h"
#include "core/disasm.h"
#include "game/smas.h"
#include "game/smb1.h"
#include "game/smb3.h"
#include <map>

#include <map>
static Snes* g_snes;
static SmasGame* g_game;
static std::map<std::string, std::vector<uint8_t>> g_snaps;
static uint32_t g_camHi = 0x7e071a, g_camLo = 0x7e071c;  // printed by vramlog
static FILE* g_watchOut;
static FILE* g_portOut;

static uint16_t parseButtons(const std::string& s) {
  uint16_t v = 0;
  std::stringstream ss(s);
  std::string t;
  while (std::getline(ss, t, '+')) {
    if (t == "b") v |= kPadB;
    else if (t == "y") v |= kPadY;
    else if (t == "select") v |= kPadSelect;
    else if (t == "start") v |= kPadStart;
    else if (t == "up") v |= kPadUp;
    else if (t == "down") v |= kPadDown;
    else if (t == "left") v |= kPadLeft;
    else if (t == "right") v |= kPadRight;
    else if (t == "a") v |= kPadA;
    else if (t == "x") v |= kPadX;
    else if (t == "l") v |= kPadL;
    else if (t == "r") v |= kPadR;
  }
  return v;
}

static void watchFn(void*, uint32_t adr, uint8_t v, uint32_t pc) {
  if (g_watchOut)
  {
    fprintf(g_watchOut, "f%u v%03d h%04d pc=%06X W %06X=%02X S=%04X stk", g_snes->frameCount(), g_snes->vPos(),
            g_snes->hPos(), pc, adr, v, g_snes->cpu.r.sp);
    for (int i = 1; i <= 12; i++) fprintf(g_watchOut, " %02X", g_snes->wram[(g_snes->cpu.r.sp + i) & 0x1fff]);
    fprintf(g_watchOut, "\n");
  }
}

static void portFn(void*, int port, uint8_t v) {
  if (g_portOut)
    fprintf(g_portOut, "f%u pc=%06X port%d=%02X\n", g_snes->frameCount(), g_snes->cpu.lastPc, port, v);
}

static void shot(const std::string& path) {
  Ppu& p = g_snes->ppu;
  int w = 256 + p.extL + p.extR;
  writePng(path, p.frame + (kMaxExt - p.extL), w, p.frameHeight(), kLineW);
}

static void traceFrames(const std::string& path, int frames, uint32_t pcLo, uint32_t pcHi) {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  Snes& s = *g_snes;
  // Instruction trace: step until N frames have elapsed
  uint32_t target = s.frameCount() + frames;
  while (s.frameCount() < target) {
    const CpuRegs& r = s.cpu.r;
    uint32_t pc = (uint32_t(r.pb) << 16) | r.pc;
    if (pc >= pcLo && pc <= pcHi) {
      std::string d;
      disasm65816(pc, r.mf, r.xf, [&](uint32_t a) { return s.peek(a); }, d);
      fprintf(f, "%06X %-20s A:%04X X:%04X Y:%04X S:%04X D:%04X DB:%02X %c%c%c%c%c%c%c%c V:%3d H:%4d\n", pc,
              d.c_str(), r.a, r.x, r.y, r.sp, r.dp, r.db, r.n ? 'N' : 'n', r.v ? 'V' : 'v', r.mf ? 'M' : 'm',
              r.xf ? 'X' : 'x', r.d ? 'D' : 'd', r.i ? 'I' : 'i', r.z ? 'Z' : 'z', r.c ? 'C' : 'c', s.vPos(),
              s.hPos());
    }
    s.stepInstruction();
  }
  fclose(f);
}

static bool writeFile(const std::string& path, const void* d, size_t n) {
  if (writeFileSafely(path, d, n)) return true;
  printf("write failed: %s\n", path.c_str());
  return false;
}

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<int16_t> g_audio;
static bool g_recordAudio = false;

// Widescreen picture scan (seamscan on TOP BOTTOM PATH): after every frame,
// at each edge of the 4:3 part, the longest run of rows (TOP..BOTTOM) where
// the picture breaks between the margin and the 4:3 side, against columns 2-8
// px either side, and whether the 4:3 column there changed since last frame
// (the picture moving: a real break stays put at the edge as it scrolls).
// Also margin rows black from the display's edge in where the 4:3 row isn't.
static FILE* g_scanOut;
static int g_scanTop = 0, g_scanBottom = 224;
static uint32_t g_scanSig[2];
static void scanFrame() {
  Ppu& p = g_snes->ppu;
  int eL = p.extL, eR = p.extR, h = std::min(g_scanBottom, p.frameHeight());
  if (!g_scanOut || eL + eR == 0 || p.forcedBlank() || p.brightness() < 15) return;  // (not while fading)
  const uint32_t* f = p.frame + (kMaxExt - eL);
  auto px = [&](int x, int y) { return f[y * kLineW + x]; };
  auto dist = [](uint32_t a, uint32_t b) {
    return std::abs(int(a >> 16 & 255) - int(b >> 16 & 255)) + std::abs(int(a >> 8 & 255) - int(b >> 8 & 255)) +
           std::abs(int(a & 255) - int(b & 255));
  };
  auto run = [&](int x, int* at) {
    int best = 0, cur = 0;
    for (int y = g_scanTop; y < h; y++) {
      cur = dist(px(x - 1, y), px(x, y)) > 60 ? cur + 1 : 0;
      if (cur > best) {
        best = cur;
        *at = y - cur + 1;
      }
    }
    return best;
  };
  unsigned fr = (unsigned)g_snes->frameCount();
  for (int side = 0; side < 2; side++) {
    int ext = side ? eR : eL;
    if (ext < 8) continue;
    int b = side ? eL + 256 : eL, in = side ? b - 1 : b;  // (in: the 4:3 side's column)
    uint32_t sig = 2166136261u;
    for (int y = g_scanTop; y < h; y++) sig = (sig ^ px(in, y)) * 16777619u;
    bool moved = sig != g_scanSig[side];
    g_scanSig[side] = sig;
    // (controls at several distances, so textures that repeat every 2-8 px cancel out)
    int at = 0, a2 = 0, seam = run(b, &at), ctl = 0;
    for (int d : {2, 3, 4, 5, 6, 8}) ctl = std::max({ctl, run(b - d, &a2), run(b + d, &a2)});
    if (seam >= 12 && seam > 2 * ctl) fprintf(g_scanOut, "S %u %c %d %d %d %d\n", fr, side ? 'R' : 'L', seam, at, ctl, moved);
    // Margin rows black from the display's edge in (at least 24 px: unbuilt
    // or missing scenery, space beyond the level) where the 4:3 row isn't.
    int rows = 0, first = -1, need = std::min(ext, 24);
    for (int y = g_scanTop; y < h; y++) {
      int mb = 0, ab = 0;
      if (side)
        while (mb < ext && (px(eL + 256 + ext - 1 - mb, y) & 0xffffff) == 0) mb++;
      else
        while (mb < ext && (px(mb, y) & 0xffffff) == 0) mb++;
      for (int x = eL; x < eL + 256; x++) ab += (px(x, y) & 0xffffff) == 0;
      if (mb >= need && ab * 10 < 256) {
        rows++;
        if (first < 0) first = y;
      }
    }
    if (rows >= 24) fprintf(g_scanOut, "B %u %c %d %d\n", fr, side ? 'R' : 'L', rows, first);
  }
}

static FILE* g_padRec = nullptr;  // padrec: the controllers per frame (the app's autotest "pads")

static void runFrames(int n) {
  for (int i = 0; i < n; i++) {
    if (g_padRec) {
      uint16_t p[2] = {g_snes->input[0], g_snes->input[1]};
      fwrite(p, 2, 2, g_padRec);
    }
    g_game->beforeFrame();
    g_snes->runFrame();
    g_game->afterFrame();
    scanFrame();
    for (const AchievementEvent& e : g_game->achievements().take())
      printf("ACH %s %s %d f%u\n", e.kind == AchievementEvent::Unlock ? "unlock" : e.kind == AchievementEvent::Mark ? "mark" : e.kind == AchievementEvent::Add ? "add" : "progress",
             e.id.c_str(), e.value, (unsigned)g_snes->frameCount());
    std::vector<int16_t> a;
    g_snes->takeAudio(a);
    if (g_recordAudio) g_audio.insert(g_audio.end(), a.begin(), a.end());
  }
}

static void writeWav(const std::string& path, const std::vector<int16_t>& s) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  uint32_t dataBytes = (uint32_t)s.size() * 2;
  uint32_t rate = 32040;
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f);
  w32(36 + dataBytes);
  fwrite("WAVEfmt ", 1, 8, f);
  w32(16);
  w16(1);
  w16(2);
  w32(rate);
  w32(rate * 4);
  w16(4);
  w16(16);
  fwrite("data", 1, 4, f);
  w32(dataBytes);
  fwrite(s.data(), 2, s.size(), f);
  fclose(f);
}

static void execLine(const std::string& line) {
  std::stringstream ss(line);
  std::string cmd;
  ss >> cmd;
  if (cmd.empty() || cmd[0] == '#') return;
  Snes& s = *g_snes;
  if (cmd == "run") {
    int n = 1;
    ss >> n;
    runFrames(n);
  } else if (cmd == "rewindtest") {
    // rewindtest PLAY BACK: play PLAY frames (held buttons, A pulsed) taking a
    // snapshot every other frame as the app does, rewind BACK steps as the app
    // does (restore keeping SRAM, one drawn frame each), let go, then replay
    // the recorded input back to frame PLAY and compare with the first run.
    int play = 600, back = 100;
    ss >> play >> back;
    auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    uint16_t base = s.input[0];
    std::vector<uint16_t> in(play);
    std::vector<std::vector<uint8_t>> orig;
    Rewind rw;
    double pushMs = 0;
    for (int i = 0; i < play; i++) {
      in[i] = base | ((i % 48) < 12 ? 0x0080 : 0);  // (A, pulsed)
      s.input[0] = in[i];
      runFrames(1);
      if (((i + 1) & 1) == 0) {
        double t0 = now();
        auto st = s.saveState();
        rw.push(st);
        pushMs += now() - t0;
        orig.push_back(std::move(st));
      }
    }
    std::vector<uint8_t> endState = s.saveState();
    s.sram[0x100] ^= 0x55;  // (a save the game made just now)
    uint8_t sramMark = s.sram[0x100];
    int bad = 0, steps = 0;
    double stepMs = 0;
    std::vector<uint8_t> st;
    for (int k = 0; k < back; k++) {
      double t0 = now();
      if (!rw.stepBack(&st)) break;
      steps++;
      if (st != orig[orig.size() - 1 - steps]) bad++;
      std::vector<uint8_t> keep(s.sram, s.sram + sizeof(s.sram));
      if (s.loadState(st)) {
        std::memcpy(s.sram, keep.data(), keep.size());
        g_game->onStateLoaded();
      }
      runFrames(1);  // (drawn, not kept)
      stepMs += now() - t0;
    }
    // Let go: back to the last snapshot shown, then play on with the same input.
    std::vector<uint8_t> keep(s.sram, s.sram + sizeof(s.sram));
    s.loadState(rw.newest());
    std::memcpy(s.sram, keep.data(), keep.size());
    g_game->onStateLoaded();
    bool sramKept = s.sram[0x100] == sramMark;
    int from = 2 * (int)(orig.size() - steps);  // frames played to reach that snapshot
    for (int i = from; i < play; i++) {
      s.input[0] = in[i];
      runFrames(1);
    }
    s.sram[0x100] ^= 0x55;  // (undo the mark before comparing)
    std::vector<uint8_t> again = s.saveState();
    bool same = again == endState;
    size_t diff = 0;
    for (size_t i = 0; i < std::min(again.size(), endState.size()); i++) diff += again[i] != endState[i];
    printf("snapshots %zu (%.1f KB kept, %.1f KB each), push %.3f ms, step back %d: %d mismatched, %.3f ms each\n",
           rw.snapshots() + steps, rw.bytes() / 1024.0, rw.bytes() / 1024.0 / std::max<size_t>(1, rw.snapshots()),
           pushMs / std::max<size_t>(1, orig.size()), steps, bad, stepMs / std::max(1, steps));
    printf("resumed from frame %d: %s (%zu bytes differ of %zu); SRAM kept: %s\n", from,
           same ? "identical to the first run" : "DIFFERS", diff, endState.size(), sramKept ? "yes" : "NO");
    s.input[0] = base;
  } else if (cmd == "hold") {
    std::string b;
    ss >> b;
    s.input[0] = parseButtons(b);
  } else if (cmd == "press") {
    std::string b;
    int n = 2, m = 1;
    ss >> b >> n >> m;
    s.input[0] = parseButtons(b);
    runFrames(n);
    s.input[0] = 0;
    runFrames(m);
  } else if (cmd == "shot") {
    std::string p;
    ss >> p;
    shot(p);
  } else if (cmd == "wide") {
    int e = 0;
    ss >> e;
    g_game->setWidescreen(e);
  } else if (cmd == "hfr") {
    // hfr K: in-between pictures per frame (high frame rate; 1 = off)
    int k = 1;
    ss >> k;
    s.ppu.subFrames = k;
    s.ppu.subTimed = false;
  } else if (cmd == "subdiff") {
    // subdiff (each frame, with "hfrat 0"): pixels where the first in-between differs from the last
    // frame's picture (at t = 0 they should be the same but for animation: a sprite moved wrong shows)
    static std::vector<uint32_t> last;
    Ppu& pp = s.ppu;
    const int n = kLineW * kFrameH;
    if ((int)last.size() == n && pp.subFrames > 1) {
      int d = 0;
      for (int i = 0; i < n; i++) d += pp.subFrame[0][i] != last[i];
      printf("subdiff f%d %d\n", (int)s.frameCount(), d);
    }
    last.assign(pp.frame, pp.frame + n);
  } else if (cmd == "msu") {
    // msu BASE: an MSU-1 music pack attached (BASE: its files' path without "-N.pcm" / ".msu"); "msu" alone: none
    std::string b;
    ss >> b;
    if (b.empty()) {
      s.msu.reset();
    } else {
      s.msu = std::make_unique<Msu1>();
      s.msu->open(std::wstring(b.begin(), b.end()));
    }
  } else if (cmd == "brickbounce") {
    // brickbounce 0|1: Super Mario Bros. / The Lost Levels' NES brick bounce (SmasGame::setNesBrickBounce)
    int v = 1;
    ss >> v;
    g_game->setNesBrickBounce(v != 0);
  } else if (cmd == "msustat") {
    printf("msu %s track=%d playing=%d\n", s.msu ? "on" : "off", s.msu ? s.msu->track() : -1, s.msu && s.msu->playing());
  } else if (cmd == "skiprender") {
    // skiprender 0|1: no pixels drawn (as a launcher preview boots)
    int v = 1;
    ss >> v;
    s.ppu.skipRender = v != 0;
  } else if (cmd == "objgroup") {
    // objgroup 0|1: high frame rate's sprite moves per character (1) or per sprite (0)
    int v = 1;
    ss >> v;
    s.ppu.objGroupMoves = v != 0;
  } else if (cmd == "hfrat") {
    // hfrat T1 [T2 ..]: in-between pictures drawn at these moments (0 = the last frame, 1 = this one)
    int n = 0;
    float t;
    while (n < Ppu::kMaxSubFrames - 1 && ss >> t) s.ppu.subAt[n++] = t;
    s.ppu.subFrames = n + 1;
    s.ppu.subTimed = n > 0;
  } else if (cmd == "moves") {
    // moves: each shown sprite's entry, X, Y, tile/attr, and its move since the last frame (high frame rate)
    Ppu& pp = s.ppu;
    printf("moves f%d", (int)s.frameCount());
    for (int i = 0; i < 128; i++) {
      int y = pp.oam[i * 4 + 1];
      if (y >= 224 && y < 240) continue;
      uint8_t hi = pp.oam[512 + (i >> 2)] >> ((i & 3) * 2);
      int x9 = pp.oam[i * 4] | ((hi & 1) << 8);
      int x = pp.objTrueXSet[i] ? pp.objTrueX[i] : x9 >= 384 ? x9 - 512 : x9;
      int dx, dy;
      pp.objMove(i, &dx, &dy);
      printf(" %d:%d,%d,%02x%02x,%d,%d", i, x, y, pp.oam[i * 4 + 3], pp.oam[i * 4 + 2], dx, dy);
    }
    printf("\n");
  } else if (cmd == "s3pw") {
    // s3pw PATH N: the WRAM addresses SMB3's player update writes in N frames, with counts and a pc
    std::string path;
    int n = 1;
    ss >> path >> n;
    static std::map<uint32_t, std::pair<int, uint32_t>> seen;
    seen.clear();
    s.setWriteWatch(0x7e0000, 0x7fffff,
                    [](void*, uint32_t adr, uint8_t, uint32_t pc) {
                      Smb3Engine* e = g_game->smb3();
                      if (e && e->inPlayerUpdate()) {
                        auto& v = seen[adr];
                        v.first++;
                        v.second = pc;
                      }
                    },
                    nullptr);
    runFrames(n);
    s.setWriteWatch(1, 0, nullptr, nullptr);
    if (FILE* f = fopen(path.c_str(), "a")) {
      for (auto& kv : seen) fprintf(f, "%06X %d pc=%06X\n", kv.first, kv.second.first, kv.second.second);
      fclose(f);
    }
  } else if (cmd == "s3pr") {
    // s3pr LO HI N: reads of [LO, HI] made inside SMB3's player update in N frames (address, pc)
    std::string lo, hi;
    int n = 1;
    ss >> lo >> hi >> n;
    static std::map<std::pair<uint32_t, uint32_t>, int> seen;
    seen.clear();
    s.setReadWatch(strtoul(lo.c_str(), 0, 16), strtoul(hi.c_str(), 0, 16),
                   [](void*, uint32_t adr, uint8_t, uint32_t pc) {
                     Smb3Engine* e = g_game->smb3();
                     if (e && e->inPlayerUpdate()) seen[{adr, pc}]++;
                   },
                   nullptr);
    runFrames(n);
    s.setReadWatch(1, 0, nullptr, nullptr);
    for (auto& kv : seen) printf("read %06X pc=%06X x%d\n", kv.first.first, kv.first.second, kv.second);
  } else if (cmd == "lscroll") {
    // lscroll STEP: every STEP-th line's scroll per layer, last frame -> this one (high frame rate)
    int step = 16;
    ss >> step;
    Ppu& pp = s.ppu;
    printf("lscroll f%d", (int)s.frameCount());
    for (int row = 1; row <= pp.frameHeight(); row += std::max(1, step)) {
      printf(" r%d", row);
      for (int l = 0; l < 3; l++) {
        int h0, v0, h1, v1;
        pp.lineScroll(1, l, row, &h0, &v0);
        pp.lineScroll(0, l, row, &h1, &v1);
        printf(" %d,%d>%d,%d", h0, v0, h1, v1);
      }
    }
    printf("\n");
  } else if (cmd == "shotsub") {
    // shotsub K PATH: the K-th in-between picture of the last frame (t = K / subFrames)
    int k = 1;
    std::string p;
    ss >> k >> p;
    Ppu& pp = s.ppu;
    if (k >= 1 && k < pp.subFrames)
      writePng(p, pp.subFrame[k - 1] + (kMaxExt - pp.extL), 256 + pp.extL + pp.extR, pp.frameHeight(), kLineW);
  } else if (cmd == "save") {
    std::string p;
    ss >> p;
    auto st = s.saveState();
    writeFile(p, st.data(), st.size());
  } else if (cmd == "load") {
    std::string p;
    ss >> p;
    auto d = readFile(p);
    if (!s.loadState(d)) printf("loadstate failed\n");
    else g_game->onStateLoaded();
  } else if (cmd == "trace") {
    std::string p;
    int n = 1;
    std::string lo = "0", hi = "ffffff";
    ss >> p >> n >> lo >> hi;
    traceFrames(p, n, strtoul(lo.c_str(), 0, 16), strtoul(hi.c_str(), 0, 16));
  } else if (cmd == "watch") {
    std::string lo, hi, p;
    int n = 1;
    ss >> lo >> hi >> p >> n;
    g_watchOut = fopen(p.c_str(), "w");
    s.setWriteWatch(strtoul(lo.c_str(), 0, 16), strtoul(hi.c_str(), 0, 16), watchFn, nullptr);
    runFrames(n);
    s.setWriteWatch(1, 0, nullptr, nullptr);
    fclose(g_watchOut);
    g_watchOut = nullptr;
  } else if (cmd == "ports") {
    std::string p;
    int n = 1;
    ss >> p >> n;
    g_portOut = fopen(p.c_str(), "w");
    s.setPortWatch(portFn, nullptr);
    runFrames(n);
    s.setPortWatch(nullptr, nullptr);
    fclose(g_portOut);
    g_portOut = nullptr;
  } else if (cmd == "dump") {
    std::string p;
    ss >> p;
    writeFile(p + ".wram", s.wram, sizeof(s.wram));
    writeFile(p + ".vram", s.ppu.vram, sizeof(s.ppu.vram));
    writeFile(p + ".cgram", s.ppu.cgram, sizeof(s.ppu.cgram));
    writeFile(p + ".oam", s.ppu.oam, sizeof(s.ppu.oam));
    writeFile(p + ".aram", s.apu.ram, sizeof(s.apu.ram));
    writeFile(p + ".sram", s.sram, sizeof(s.sram));
  } else if (cmd == "peek") {
    std::string a;
    int n = 16;
    ss >> a >> n;
    uint32_t adr = strtoul(a.c_str(), 0, 16);
    for (int i = 0; i < n; i++) {
      if (i % 16 == 0) printf("%s%06X:", i ? "\n" : "", adr + i);
      printf(" %02X", s.peek(adr + i));
    }
    printf("\n");
  } else if (cmd == "poke") {
    std::string a, v;
    ss >> a >> v;
    s.poke(strtoul(a.c_str(), 0, 16), (uint8_t)strtoul(v.c_str(), 0, 16));
  } else if (cmd == "regs") {
    const CpuRegs& r = s.cpu.r;
    printf("PC=%02X:%04X A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X frame=%u\n", r.pb, r.pc, r.a, r.x, r.y,
           r.sp, r.dp, r.db, s.frameCount());
  } else if (cmd == "disasm") {
    std::string a;
    int n = 20, m = 1, x = 1;
    ss >> a >> n >> m >> x;
    uint32_t pc = strtoul(a.c_str(), 0, 16);
    for (int i = 0; i < n; i++) {
      std::string d;
      int len = disasm65816(pc, m, x, [&](uint32_t ad) { return s.peek(ad); }, d);
      printf("%06X  %s\n", pc, d.c_str());
      pc = (pc & 0xff0000) | ((pc + len) & 0xffff);
    }
  } else if (cmd == "bench") {
    int n = 600;
    ss >> n;
    auto t0 = std::chrono::steady_clock::now();
    runFrames(n);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("%d frames in %.1f ms (%.1f fps)\n", n, ms, n * 1000.0 / ms);
  } else if (cmd == "apu") {
    const Apu::Regs& r = s.apu.r;
    printf("SPC PC=%04X A=%02X X=%02X Y=%02X SP=%02X stopped=%d ports in=%02X %02X %02X %02X out=%02X %02X %02X %02X\n",
           r.pc, r.a, r.x, r.y, r.sp, r.stopped, s.apu.inPorts[0], s.apu.inPorts[1], s.apu.inPorts[2],
           s.apu.inPorts[3], s.apu.outPorts[0], s.apu.outPorts[1], s.apu.outPorts[2], s.apu.outPorts[3]);
    for (int i = 0; i < 128; i++) {
      if (i % 16 == 0) printf("%sDSP %02X:", i ? "\n" : "", i);
      printf(" %02X", s.apu.dsp.regs[i]);
    }
    printf("\n");
  } else if (cmd == "aram") {
    // aram ADDR LEN: the sound CPU's RAM (hex), 16 bytes a line
    std::string a;
    int n = 256;
    ss >> a >> n;
    const unsigned at = strtoul(a.c_str(), 0, 16);
    for (int i = 0; i < n; i++) {
      if (i % 16 == 0) printf("%sARAM %04X:", i ? "\n" : "", (at + i) & 0xffff);
      printf(" %02X", s.apu.ram[(at + i) & 0xffff]);
    }
    printf("\n");
  } else if (cmd == "warp") {
    // warp GAME WORLD LEVEL [CHAR]  (GAME: 1=SMB1 2=LL 3=SMB2 4=SMB3; world/level 1-based; CHAR: SMB2's $008F)
    int gm = 1, w = 1, l = 1, ch = -1;
    ss >> gm >> w >> l >> ch;
    GameId ids[] = {GameId::SMB1, GameId::SMB1, GameId::SMBLL, GameId::SMB2, GameId::SMB3};
    std::string err;
    if (!g_game->requestLevel(ids[std::clamp(gm, 1, 4)], w - 1, l - 1, &err, ch)) printf("warp failed: %s\n", err.c_str());
  } else if (cmd == "coop") {
    int on = 1;
    ss >> on;
    g_game->coop = on != 0;
  } else if (cmd == "p2poke") {
    std::string a2, v2;
    ss >> a2 >> v2;
    if (Smb1Engine* e = g_game->smb1Engine()) e->coopPoke((uint16_t)strtoul(a2.c_str(), 0, 16), (uint8_t)strtoul(v2.c_str(), 0, 16));
    if (Smb3Engine* e = g_game->smb3()) e->coopPoke((uint32_t)strtoul(a2.c_str(), 0, 16), (uint8_t)strtoul(v2.c_str(), 0, 16));
  } else if (cmd == "winwrites") {
    // winwrites START END PATH FRAMES: WRAM writes made between reaching
    // START and reaching END (e.g. around one JSR), counted per address.
    std::string a, b, p;
    int n = 1;
    ss >> a >> b >> p >> n;
    uint32_t pa = strtoul(a.c_str(), 0, 16), pb = strtoul(b.c_str(), 0, 16);
    static bool inWin = false;
    static std::map<uint32_t, std::pair<int, uint32_t>> hits;
    hits.clear();
    inWin = false;
    s.setWriteWatch(0x7e0000, 0x7fffff,
                    [](void*, uint32_t adr, uint8_t, uint32_t pc) {
                      if (!inWin) return;
                      auto& h = hits[adr];
                      h.first++;
                      h.second = pc;
                    },
                    nullptr);
    uint32_t target = s.frameCount() + n;
    while (s.frameCount() < target) {
      uint32_t pc = (uint32_t(s.cpu.r.pb) << 16) | s.cpu.r.pc;
      if (pc == pa) inWin = true;
      if (pc == pb) inWin = false;
      uint32_t f0 = s.frameCount();
      s.stepInstruction();
      if (s.frameCount() != f0) {
        g_game->afterFrame();
        g_game->beforeFrame();
      }
    }
    s.setWriteWatch(1, 0, nullptr, nullptr);
    FILE* f = fopen(p.c_str(), "w");
    for (auto& kv : hits) fprintf(f, "%06X %d pc=%06X\n", kv.first, kv.second.first, kv.second.second);
    fclose(f);
  } else if (cmd == "launch") {
    // launch GAME [startfile]: the launcher's start of a game (GAME as for warp)
    int gm = 1, sf = 0;
    ss >> gm >> sf;
    GameId ids[] = {GameId::SMB1, GameId::SMB1, GameId::SMBLL, GameId::SMB2, GameId::SMB3};
    g_game->requestTitle(ids[std::clamp(gm, 1, 4)], sf != 0);
  } else if (cmd == "loading") {
    printf("loading=%d game=%s f%u\n", (int)g_game->levelLoading(), g_game->describe().c_str(),
           (unsigned)g_snes->frameCount());
  } else if (cmd == "ach") {
    // ach on|off: achievement checks (a counting run from now); events print as "ACH ..."
    std::string v;
    ss >> v;
    g_game->achievements().newRun(v != "off");
  } else if (cmd == "verifywide") {
    for (int l = 0; l < 3; l++) printf("%s\n", g_game->verifyWide(l).c_str());
  } else if (cmd == "ext") {
    printf("ext %d %d %u\n", s.ppu.extL, s.ppu.extR, (unsigned)s.frameCount());
  } else if (cmd == "seamscan") {
    std::string mode, path;
    ss >> mode;
    if (g_scanOut) fclose(g_scanOut);
    g_scanOut = nullptr;
    if (mode == "on") {
      ss >> g_scanTop >> g_scanBottom >> path;
      g_scanOut = fopen(path.c_str(), "w");
      g_scanSig[0] = g_scanSig[1] = 0;
    }
  } else if (cmd == "reset") {
    s.reset(false);
  } else if (cmd == "palette") {
    // palette MARIO [LUIGI]: character colour variants (Luigi's kept if not given)
    ss >> g_game->playerPalette;
    int l = -1;
    if (ss >> l) g_game->luigiPalette = l;
  } else if (cmd == "cheats") {
    std::string v;
    ss >> v;
    g_game->setCheats(strtoull(v.c_str(), 0, 16));
  } else if (cmd == "layers") {
    std::string v;
    ss >> v;
    g_snes->ppu.debugLayers = (uint8_t)strtoul(v.c_str(), 0, 16);  // e.g. 02 = BG2 only
  } else if (cmd == "enemies") {
    int e = 1;
    ss >> e;
    g_game->enemyRange = e != 0;  // 0 = original timing, 1 = active in full view
  } else if (cmd == "camaddr") {
    std::string hi, lo;
    ss >> hi >> lo;
    g_camHi = strtoul(hi.c_str(), 0, 16);
    g_camLo = strtoul(lo.c_str(), 0, 16);
  } else if (cmd == "hold2") {
    std::string b;
    ss >> b;
    s.input[1] = parseButtons(b);
  } else if (cmd == "hold3" || cmd == "hold4" || cmd == "hold5") {
    // players 3-5's controllers (co-op: SmasGame::pads)
    std::string b;
    ss >> b;
    g_game->pads[cmd[4] - '1'] = parseButtons(b);
  } else if (cmd == "hash") {
    // the machine's state in short: frame, CPU cycles, line, WRAM's FNV-1a hash, the APU's cycles
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : s.wram) h = (h ^ b) * 1099511628211ull;
    printf("hash f%u cyc %llu v%u wram %016llx apu %llu\n", s.frameCount(), (unsigned long long)s.masterCycles(), s.vPos(),
           (unsigned long long)h, (unsigned long long)s.apu.cycles());
  } else if (cmd == "midturn") {
    // co-op: a player's turn left unfinished at this frame's end (should never be)
    if (g_game->coopMidTurn()) printf("MIDTURN at frame %u\n", s.frameCount());
  } else if (cmd == "present") {
    // which players are there (bit per player, hex: online's session)
    std::string b;
    ss >> b;
    g_game->padsPresent = (uint8_t)strtoul(b.c_str(), 0, 16);
  } else if (cmd == "oam") {
    // print visible OAM entries; sx = where the PPU draws it relative to the
    // 4:3 screen (the game layer's true X, else the 9-bit X split as in renderObj)
    int split = 256 + s.ppu.extR + std::max(0, 256 - s.ppu.extL - s.ppu.extR) / 2;
    for (int i = 0; i < 128; i++) {
      uint8_t hi = s.ppu.oam[512 + (i >> 2)] >> ((i & 3) * 2);
      int x = s.ppu.oam[i * 4] | ((hi & 1) << 8);
      int y = s.ppu.oam[i * 4 + 1];
      if (y >= 224) continue;
      int sx = s.ppu.objTrueXSet[i] ? s.ppu.objTrueX[i] : x < split ? x : x - 512;
      printf("  #%3d x=%3d y=%3d tile=%03X attr=%02X big=%d sx=%d%s\n", i, x, y,
             s.ppu.oam[i * 4 + 2] | ((s.ppu.oam[i * 4 + 3] & 1) << 8), s.ppu.oam[i * 4 + 3], (hi >> 1) & 1, sx,
             s.ppu.objTrueXSet[i] ? " true" : "");
    }
  } else if (cmd == "snap") {
    std::string name;
    ss >> name;
    g_snaps[name].assign(s.wram, s.wram + sizeof(s.wram));
  } else if (cmd == "diff") {
    // diff NAME [lo hi] - list WRAM bytes that changed since snapshot NAME
    std::string name, lo = "0", hi = "1ffff";
    ss >> name >> lo >> hi;
    auto& snap = g_snaps[name];
    if (snap.empty()) return;
    uint32_t l = strtoul(lo.c_str(), 0, 16), h = strtoul(hi.c_str(), 0, 16);
    int n = 0;
    for (uint32_t i = l; i <= h && i < sizeof(s.wram); i++) {
      if (snap[i] != s.wram[i]) {
        printf("  %05X: %02X -> %02X\n", i, snap[i], s.wram[i]);
        if (++n > 400) break;
      }
    }
    printf("%d changed\n", n);
  } else if (cmd == "find") {
    // find VALUE [lo hi]
    std::string v, lo = "0", hi = "1ffff";
    ss >> v >> lo >> hi;
    uint8_t val = (uint8_t)strtoul(v.c_str(), 0, 16);
    uint32_t l = strtoul(lo.c_str(), 0, 16), h = strtoul(hi.c_str(), 0, 16);
    int n = 0;
    for (uint32_t i = l; i <= h && i < sizeof(s.wram); i++)
      if (s.wram[i] == val) {
        printf(" %05X", i);
        if (++n > 200) break;
      }
    printf("\n");
  } else if (cmd == "rwatch") {
    std::string lo, hi, p;
    int n = 1;
    ss >> lo >> hi >> p >> n;
    g_watchOut = fopen(p.c_str(), "w");
    s.setReadWatch(strtoul(lo.c_str(), 0, 16), strtoul(hi.c_str(), 0, 16),
                   [](void*, uint32_t adr, uint8_t v, uint32_t pc) {
                     if (g_watchOut)
                       fprintf(g_watchOut, "f%u v%03d pc=%06X R %06X=%02X\n", g_snes->frameCount(), g_snes->vPos(),
                               pc, adr, v);
                   },
                   nullptr);
    runFrames(n);
    s.setReadWatch(1, 0, nullptr, nullptr);
    fclose(g_watchOut);
    g_watchOut = nullptr;
  } else if (cmd == "vramlog") {
    // vramlog PATH N [lo hi] - log runs of VRAM word writes
    std::string p, lo = "0", hi = "7fff";
    int n = 1;
    ss >> p >> n >> lo >> hi;
    static FILE* vf;
    static uint32_t vlo, vhi;
    static int runStart = -1, runLast = -1, runStep = 0, runCount = 0;
    static uint32_t runFrame = 0;
    static int runLine = 0;
    vf = fopen(p.c_str(), "w");
    vlo = strtoul(lo.c_str(), 0, 16);
    vhi = strtoul(hi.c_str(), 0, 16);
    runStart = -1;
    auto flush = []() {
      if (runStart >= 0 && vf)
        fprintf(vf, "f%u %04X..%04X step %d count %d  cam=%02X%02X line %d\n", runFrame, runStart, runLast, runStep,
                runCount, g_snes->peek(g_camHi), g_snes->peek(g_camLo), runLine);
      runStart = -1;
    };
    s.ppu.vramWatchCtx = (void*)+flush;
    s.ppu.vramWatch = [](void* ctx, uint16_t adr, uint16_t) {
      if (adr < vlo || adr > vhi) return;
      uint32_t f = g_snes->frameCount();
      int step = adr - runLast;
      if (runStart >= 0 && f == runFrame && (runCount == 1 || step == runStep) && step != 0) {
        runStep = step;
        runLast = adr;
        runCount++;
        return;
      }
      ((void (*)())ctx)();
      runStart = runLast = adr;
      runStep = 0;
      runCount = 1;
      runFrame = f;
      runLine = g_snes->vPos();
    };
    runFrames(n);
    flush();
    s.ppu.vramWatch = nullptr;
    fclose(vf);
  } else if (cmd == "ppu") {
    printf("%s", s.ppu.debugString().c_str());
  } else if (cmd == "bgshot") {
    int l = 1;
    std::string p;
    ss >> l >> p;
    std::vector<uint32_t> buf(1024 * 1024);
    int w, h;
    s.ppu.debugRenderMap(l - 1, buf.data(), &w, &h);
    writePng(p, buf.data(), w, h, w);
  } else if (cmd == "spctrace") {
    std::string p;
    int n = 1000, frames = 1;
    ss >> p >> n >> frames;
    FILE* f = fopen(p.c_str(), "w");
    s.apu.traceFile = f;
    s.apu.traceLimit = n;
    runFrames(frames);
    s.apu.traceFile = nullptr;
    fclose(f);
  } else if (cmd == "gains") {
    // gains MUSIC SFX: the music's and the sound effects' volumes, percent (Dsp::musicGain / sfxGain)
    int m = 100, x = 100;
    ss >> m >> x;
    s.apu.dsp.musicGain = m * 256 / 100;
    s.apu.dsp.sfxGain = x * 256 / 100;
  } else if (cmd == "rec") {
    g_audio.clear();
    g_recordAudio = true;
  } else if (cmd == "wav") {
    std::string p;
    ss >> p;
    writeWav(p, g_audio);
    int64_t peak = 0;
    double rms = 0;
    for (int16_t v : g_audio) {
      peak = std::max<int64_t>(peak, std::abs((int)v));
      rms += (double)v * v;
    }
    if (!g_audio.empty()) rms = std::sqrt(rms / g_audio.size());
    printf("audio: %zu samples, peak %lld, rms %.1f\n", g_audio.size() / 2, (long long)peak, rms);
    g_recordAudio = false;
  } else if (cmd == "padrec") {
    // padrec PATH: from now, both controllers per frame to PATH (uint16 each); padrec off: stop
    std::string p;
    ss >> p;
    if (g_padRec) fclose(g_padRec);
    g_padRec = p == "off" || p.empty() ? nullptr : fopen(p.c_str(), "wb");
  } else if (cmd == "hash") {
    // hash NAME: an FNV-1a of work RAM (the app's autotest "mark")
    std::string name;
    ss >> name;
    uint32_t h = 2166136261u;
    for (uint8_t b : s.wram) h = (h ^ b) * 16777619u;
    printf("HASH %s %08x\n", name.c_str(), h);
  } else if (cmd == "game") {
    printf("%s\n", g_game->describe().c_str());
  } else if (cmd == "echo") {
    std::string rest;
    std::getline(ss, rest);
    printf("%s\n", rest.c_str());
  } else {
    printf("unknown command: %s\n", cmd.c_str());
  }
  fflush(stdout);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: smastool <rom> <script | -c commands>\n");
    return 1;
  }
  auto rom = readFile(argv[1]);
  if (rom.empty()) {
    fprintf(stderr, "cannot read rom\n");
    return 1;
  }
  static Snes snes;
  g_snes = &snes;
  if (!snes.loadRom(rom)) return 1;
  static SmasGame game(snes);
  g_game = &game;
  game.install();
  snes.reset(true);

  std::string script;
  if (std::string(argv[2]) == "-c") {
    for (int i = 3; i < argc; i++) {
      script += argv[i];
      script += " ";
    }
    for (auto& c : script)
      if (c == ';') c = '\n';
  } else {
    auto d = readFile(argv[2]);
    script.assign(d.begin(), d.end());
  }
  std::stringstream ss(script);
  std::string line;
  while (std::getline(ss, line)) execLine(line);
  return 0;
}
