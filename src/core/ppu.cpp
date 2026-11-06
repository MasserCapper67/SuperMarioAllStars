// SNES PPU scanline renderer.
//
// Each visible line is rendered into per-layer buffers covering the widened
// range [-ext, 256+ext), then composited with windows and color math. The
// margins are rendered from the same tilemaps/OAM the game uses, optionally
// overridden through PpuHooks (see game/ for per-game widescreen logic).
#include "ppu.h"
#include "statefix.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
// bits per pixel for each layer by mode (0 = not present)
const uint8_t kBpp[8][4] = {
    {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
    {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0},
};

struct PriEntry {
  uint8_t layer;
  uint8_t pri;  // matches Px::pri
};
// Front-to-back priority lists. BG pri: 1 = low, 2 = high. OBJ pri: 1..4.
const PriEntry kMode0[] = {{4, 4}, {0, 2}, {1, 2}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {2, 2}, {3, 2}, {4, 1}, {2, 1}, {3, 1}};
const PriEntry kMode1a[] = {{2, 2}, {4, 4}, {0, 2}, {1, 2}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {4, 1}, {2, 1}};
const PriEntry kMode1b[] = {{4, 4}, {0, 2}, {1, 2}, {4, 3}, {0, 1}, {1, 1}, {4, 2}, {2, 2}, {4, 1}, {2, 1}};
const PriEntry kMode2[] = {{4, 4}, {0, 2}, {4, 3}, {1, 2}, {4, 2}, {0, 1}, {4, 1}, {1, 1}};
const PriEntry kMode6[] = {{4, 4}, {0, 2}, {4, 3}, {4, 2}, {0, 1}, {4, 1}};
const PriEntry kMode7[] = {{4, 4}, {4, 3}, {1, 2}, {4, 2}, {0, 1}, {4, 1}, {1, 1}};

const uint8_t kObjSizes[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

inline int16_t sext13(int v) { return (int16_t)((v & 0x1fff) ^ 0x1000) - 0x1000; }
inline int floorMod(int a, int m) {
  int r = a % m;
  return r < 0 ? r + m : r;
}
}  // namespace

Ppu::Ppu() { reset(true); }

void Ppu::reset(bool hard) {
  if (hard) {
    std::memset(vram, 0, sizeof(vram));
    std::memset(cgram, 0, sizeof(cgram));
    std::memset(oam, 0, sizeof(oam));
  }
  forcedBlank_ = true;
  brightness_ = 0;
  objSize_ = 0;
  objBase_ = 0;
  objNameSel_ = 0;
  oamAdr_ = oamAdrReload_ = 0;
  oamPriority_ = false;
  oamLatch_ = 0;
  mode_ = 0;
  bg3Priority_ = false;
  mosaicSize_ = 1;
  mosaicStart_ = 1;
  for (auto& b : bg_) b = Bg();
  scrollPrev_ = scrollPrev2_ = m7Prev_ = 0;
  vramAdr_ = 0;
  vramInc_ = 1;
  vramRemap_ = 0;
  vramIncOnHigh_ = false;
  vramReadBuf_ = 0;
  m7sel_ = 0;
  m7a_ = m7b_ = m7c_ = m7d_ = m7x_ = m7y_ = m7hofs_ = m7vofs_ = 0;
  cgAdr_ = 0;
  cgSecond_ = false;
  cgLatch_ = 0;
  std::memset(winSel_, 0, sizeof(winSel_));
  winL_[0] = winL_[1] = winR_[0] = winR_[1] = 0;
  std::memset(winLogic_, 0, sizeof(winLogic_));
  tm_ = ts_ = tmw_ = tsw_ = 0;
  cgwsel_ = cgadsub_ = 0;
  fixedR_ = fixedG_ = fixedB_ = 0;
  setini_ = 0;
  overscan_ = interlace_ = false;
  hCount_ = vCount_ = 0;
  hFlip_ = vFlip_ = false;
  countersLatched_ = false;
  rangeOver_ = timeOver_ = false;
  std::memset(frame, 0, sizeof(frame));
  buildLut();
}

void Ppu::buildLut() {
  for (int b = 0; b < 16; b++) {
    for (int c = 0; c < 32; c++) {
      int c8 = (c << 3) | (c >> 2);
      colorLut_[b][c] = (uint8_t)(c8 * b / 15);
    }
  }
}

uint16_t Ppu::remapVramAdr(uint16_t adr) const {
  switch (vramRemap_) {
    case 1: return (adr & 0xff00) | ((adr & 0x001f) << 3) | ((adr >> 5) & 7);
    case 2: return (adr & 0xfe00) | ((adr & 0x003f) << 3) | ((adr >> 6) & 7);
    case 3: return (adr & 0xfc00) | ((adr & 0x007f) << 3) | ((adr >> 7) & 7);
    default: return adr;
  }
}

uint8_t Ppu::read(uint8_t reg, uint8_t openBus) {
  switch (reg) {
    case 0x34: case 0x35: case 0x36: {
      int32_t res = int32_t(m7a_) * int8_t(m7b_ >> 8);
      ppu1Bus_ = (res >> ((reg - 0x34) * 8)) & 0xff;
      return ppu1Bus_;
    }
    case 0x37: return openBus;  // latch handled by Snes
    case 0x38: {
      uint8_t v;
      if (oamAdr_ >= 512) v = oam[512 + (oamAdr_ & 0x1f)];
      else v = oam[oamAdr_];
      oamAdr_ = (oamAdr_ + 1) & 0x3ff;
      ppu1Bus_ = v;
      return v;
    }
    case 0x39: {
      uint8_t v = vramReadBuf_ & 0xff;
      if (!vramIncOnHigh_) {
        vramReadBuf_ = vram[remapVramAdr(vramAdr_) & 0x7fff];
        vramAdr_ += vramInc_;
      }
      ppu1Bus_ = v;
      return v;
    }
    case 0x3a: {
      uint8_t v = vramReadBuf_ >> 8;
      if (vramIncOnHigh_) {
        vramReadBuf_ = vram[remapVramAdr(vramAdr_) & 0x7fff];
        vramAdr_ += vramInc_;
      }
      ppu1Bus_ = v;
      return v;
    }
    case 0x3b: {
      uint8_t v;
      if (!cgSecond_) {
        v = cgram[cgAdr_] & 0xff;
      } else {
        v = ((cgram[cgAdr_] >> 8) & 0x7f) | (ppu2Bus_ & 0x80);
        cgAdr_++;
      }
      cgSecond_ = !cgSecond_;
      ppu2Bus_ = v;
      return v;
    }
    case 0x3c: {
      uint8_t v = hFlip_ ? ((hCount_ >> 8) & 1) | (ppu2Bus_ & 0xfe) : (hCount_ & 0xff);
      hFlip_ = !hFlip_;
      ppu2Bus_ = v;
      return v;
    }
    case 0x3d: {
      uint8_t v = vFlip_ ? ((vCount_ >> 8) & 1) | (ppu2Bus_ & 0xfe) : (vCount_ & 0xff);
      vFlip_ = !vFlip_;
      ppu2Bus_ = v;
      return v;
    }
    case 0x3e: {
      uint8_t v = (timeOver_ ? 0x80 : 0) | (rangeOver_ ? 0x40 : 0) | 0x01 | (ppu1Bus_ & 0x10);
      ppu1Bus_ = v;
      return v;
    }
    case 0x3f: {
      uint8_t v = (countersLatched_ ? 0x40 : 0) | (frameOdd_ ? 0x80 : 0) | 0x03 | (ppu2Bus_ & 0x20);
      countersLatched_ = false;
      hFlip_ = vFlip_ = false;
      ppu2Bus_ = v;
      return v;
    }
    default: return openBus;
  }
}

void Ppu::write(uint8_t reg, uint8_t v) {
  switch (reg) {
    case 0x00:
      forcedBlank_ = v & 0x80;
      brightness_ = v & 0x0f;
      break;
    case 0x01:
      objSize_ = v >> 5;
      objNameSel_ = (v >> 3) & 3;
      objBase_ = (v & 7) << 13;
      break;
    case 0x02:
      oamAdrReload_ = (oamAdrReload_ & 0x100) | v;
      oamAdr_ = (oamAdrReload_ << 1) & 0x3ff;
      break;
    case 0x03:
      oamAdrReload_ = (oamAdrReload_ & 0xff) | ((v & 1) << 8);
      oamPriority_ = v & 0x80;
      oamAdr_ = (oamAdrReload_ << 1) & 0x3ff;
      break;
    case 0x04:
      if (oamAdr_ >= 512) {
        oam[512 + (oamAdr_ & 0x1f)] = v;
      } else if ((oamAdr_ & 1) == 0) {
        oamLatch_ = v;
      } else {
        oam[oamAdr_ - 1] = oamLatch_;
        oam[oamAdr_] = v;
      }
      oamAdr_ = (oamAdr_ + 1) & 0x3ff;
      break;
    case 0x05:
      mode_ = v & 7;
      bg3Priority_ = v & 8;
      for (int i = 0; i < 4; i++) bg_[i].bigTiles = v & (0x10 << i);
      break;
    case 0x06:
      mosaicSize_ = (v >> 4) + 1;
      for (int i = 0; i < 4; i++) bg_[i].mosaic = v & (1 << i);
      break;
    case 0x07: case 0x08: case 0x09: case 0x0a: {
      Bg& b = bg_[reg - 0x07];
      b.mapBase = (v & 0xfc) << 8;
      b.mapSize = v & 3;
      break;
    }
    case 0x0b:
      bg_[0].charBase = (v & 0x0f) << 12;
      bg_[1].charBase = (v >> 4) << 12;
      break;
    case 0x0c:
      bg_[2].charBase = (v & 0x0f) << 12;
      bg_[3].charBase = (v >> 4) << 12;
      break;
    case 0x0d: case 0x0f: case 0x11: case 0x13: {
      Bg& b = bg_[(reg - 0x0d) >> 1];
      // Fine scroll (bits 0-2) comes from the previous HOFS write, which for
      // the usual low/high byte pair is the low byte (as bsnes' PPU2 latch).
      b.hofs = ((v << 8) | (scrollPrev_ & ~7) | (hofsPrev_ & 7)) & 0x3ff;
      scrollPrev_ = v;
      hofsPrev_ = v;
      if (reg == 0x0d) {
        m7hofs_ = sext13((v << 8) | m7Prev_);
        m7Prev_ = v;
      }
      break;
    }
    case 0x0e: case 0x10: case 0x12: case 0x14: {
      Bg& b = bg_[(reg - 0x0e) >> 1];
      b.vofs = ((v << 8) | scrollPrev_) & 0x3ff;
      scrollPrev_ = v;
      if (reg == 0x0e) {
        m7vofs_ = sext13((v << 8) | m7Prev_);
        m7Prev_ = v;
      }
      break;
    }
    case 0x15: {
      static const uint8_t incs[4] = {1, 32, 128, 128};
      vramInc_ = incs[v & 3];
      vramRemap_ = (v >> 2) & 3;
      vramIncOnHigh_ = v & 0x80;
      break;
    }
    case 0x16:
      vramAdr_ = (vramAdr_ & 0xff00) | v;
      vramReadBuf_ = vram[remapVramAdr(vramAdr_) & 0x7fff];
      break;
    case 0x17:
      vramAdr_ = (vramAdr_ & 0x00ff) | (v << 8);
      vramReadBuf_ = vram[remapVramAdr(vramAdr_) & 0x7fff];
      break;
    case 0x18: {
      uint16_t a = remapVramAdr(vramAdr_) & 0x7fff;
      vram[a] = (vram[a] & 0xff00) | v;
      if (!vramIncOnHigh_) {
        if (vramWatch) vramWatch(vramWatchCtx, a, vram[a]);
        if (hooks) hooks->onVramWrite(*this, a);
      }
      if (!vramIncOnHigh_) vramAdr_ += vramInc_;
      break;
    }
    case 0x19: {
      uint16_t a = remapVramAdr(vramAdr_) & 0x7fff;
      vram[a] = (vram[a] & 0x00ff) | (v << 8);
      if (vramIncOnHigh_) {
        if (vramWatch) vramWatch(vramWatchCtx, a, vram[a]);
        if (hooks) hooks->onVramWrite(*this, a);
      }
      if (vramIncOnHigh_) vramAdr_ += vramInc_;
      break;
    }
    case 0x1a: m7sel_ = v; break;
    case 0x1b: m7a_ = (int16_t)((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x1c: m7b_ = (int16_t)((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x1d: m7c_ = (int16_t)((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x1e: m7d_ = (int16_t)((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x1f: m7x_ = sext13((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x20: m7y_ = sext13((v << 8) | m7Prev_); m7Prev_ = v; break;
    case 0x21:
      cgAdr_ = v;
      cgSecond_ = false;
      break;
    case 0x22:
      if (!cgSecond_) {
        cgLatch_ = v;
      } else {
        cgram[cgAdr_] = ((v & 0x7f) << 8) | cgLatch_;
        cgAdr_++;
      }
      cgSecond_ = !cgSecond_;
      break;
    case 0x23: winSel_[0] = v & 0x0f; winSel_[1] = v >> 4; break;
    case 0x24: winSel_[2] = v & 0x0f; winSel_[3] = v >> 4; break;
    case 0x25: winSel_[4] = v & 0x0f; winSel_[5] = v >> 4; break;
    case 0x26: winL_[0] = v; break;
    case 0x27: winR_[0] = v; break;
    case 0x28: winL_[1] = v; break;
    case 0x29: winR_[1] = v; break;
    case 0x2a:
      for (int i = 0; i < 4; i++) winLogic_[i] = (v >> (i * 2)) & 3;
      break;
    case 0x2b:
      winLogic_[4] = v & 3;
      winLogic_[5] = (v >> 2) & 3;
      break;
    case 0x2c: tm_ = v & 0x1f; break;
    case 0x2d: ts_ = v & 0x1f; break;
    case 0x2e: tmw_ = v & 0x1f; break;
    case 0x2f: tsw_ = v & 0x1f; break;
    case 0x30: cgwsel_ = v; break;
    case 0x31: cgadsub_ = v; break;
    case 0x32:
      if (v & 0x20) fixedR_ = v & 0x1f;
      if (v & 0x40) fixedG_ = v & 0x1f;
      if (v & 0x80) fixedB_ = v & 0x1f;
      break;
    case 0x33:
      setini_ = v;
      overscan_ = v & 0x04;
      interlace_ = v & 0x01;
      break;
    default: break;
  }
}

void Ppu::frameStart() {
  rangeOver_ = timeOver_ = false;
  frameOdd_ = !frameOdd_;
}

void Ppu::vblankStart() {
  if (!forcedBlank_) {
    oamAdr_ = (oamAdrReload_ << 1) & 0x3ff;
  }
}

// ---------------------------------------------------------------------------

uint16_t Ppu::mapEntry(int l, int tx, int ty) const {
  const Bg& b = bg_[l];
  int off = ((ty & 31) << 5) | (tx & 31);
  switch (b.mapSize) {
    case 1: off += (tx & 32) << 5; break;
    case 2: off += (ty & 32) << 5; break;
    case 3: off += ((tx & 32) << 5) + ((ty & 32) << 6); break;
    default: break;
  }
  return vram[(b.mapBase + off) & 0x7fff];
}

uint16_t Ppu::directColor(uint8_t idx, uint8_t pal) const {
  // BBGGGRRR + palette bits bgr -> 15-bit color
  int r = ((idx & 0x07) << 2) | ((pal & 1) << 1);
  int g = ((idx & 0x38) >> 1) | ((pal & 2));
  int b = ((idx & 0xc0) >> 3) | ((pal & 4) >> 1);
  return r | (g << 5) | (b << 10);
}

void Ppu::renderBg(int l, int row, int x0, int x1) {
  Px* out = layer_[l] + kMaxExt;
  const Bg& b = bg_[l];
  int bpp = kBpp[mode_][l];
  int palBase = mode_ == 0 ? l * 32 : 0;
  int tileWords = bpp * 4;
  bool direct = bpp == 8 && (cgwsel_ & 1);
  int msize = b.mosaic ? mosaicSize_ : 1;

  int line = row + 1;
  int ly = line;
  if (msize > 1) ly = line - ((line - 1) % msize);

  bool opt = (mode_ == 2 || mode_ == 4 || mode_ == 6) && l < 2;
  int tileShift = b.bigTiles ? 4 : 3;

  // cached tile row
  int cachedKey = 0x7fffffff;
  uint8_t pix[8] = {};
  uint8_t cPal = 0, cPri = 0;

  for (int x = x0; x < x1; x++) {
    bool margin = x < 0 || x >= 256;
    if (margin && hideWideBg[l]) {
      out[x].pri = 0;
      continue;
    }
    int sx = x;
    if (msize > 1) sx = x - floorMod(x, msize);
    int hofs = (b.hofs + bgHofsAdj[l]) & 0x3ff, vofs = b.vofs + (sub_ ? subVofs_[l] : 0);
    if (opt && sx >= 0 && sx < 256) {
      int col = (sx + (hofs & 7)) >> 3;
      if (col > 0) {
        const Bg& b3 = bg_[2];
        int ox = (((col - 1) << 3) + (b3.hofs & ~7)) >> 3;
        int oy = b3.vofs >> 3;
        uint16_t eh, ev;
        auto m3 = [&](int tx, int ty) {
          int off = ((ty & 31) << 5) | (tx & 31);
          switch (b3.mapSize) {
            case 1: off += (tx & 32) << 5; break;
            case 2: off += (ty & 32) << 5; break;
            case 3: off += ((tx & 32) << 5) + ((ty & 32) << 6); break;
          }
          return vram[(b3.mapBase + off) & 0x7fff];
        };
        uint16_t bit = l == 0 ? 0x2000 : 0x4000;
        if (mode_ == 4) {
          uint16_t e = m3(ox, oy);
          if (e & bit) {
            if (e & 0x8000) vofs = e & 0x3ff;
            else hofs = (e & 0x3f8) | (hofs & 7);
          }
        } else {
          eh = m3(ox, oy);
          ev = m3(ox, oy + 1);
          if (eh & bit) hofs = (eh & 0x3f8) | (hofs & 7);
          if (ev & bit) vofs = ev & 0x3ff;
        }
      }
    }
    int px = (sx + hofs) & 0x3ff;
    int py = (ly + vofs) & 0x3ff;
    int tx = px >> tileShift, ty = py >> tileShift;
    int key = (tx << 16) | ((px >> 3) & 1) << 15 | (py & 0x3ff) | (margin ? 0x8000000 : 0);
    if (opt || key != cachedKey) {  // offset-per-tile: fetch every pixel
      cachedKey = key;
      uint16_t e;
      int wr = (margin || wideFetchAll[l]) && hooks ? hooks->fetchWideTile(*this, l, tx, ty, x, &e)
                                                    : PpuHooks::kWideVram;
      if (wr == PpuHooks::kWideBlank) {
        cachedKey = 0x7fffffff;
        out[x].pri = 0;
        continue;
      }
      if (wr == PpuHooks::kWideVram) e = mapEntry(l, tx, ty);
      int tile = e & 0x3ff;
      cPal = (e >> 10) & 7;
      cPri = (e & 0x2000) ? 2 : 1;
      bool hf = e & 0x4000, vf = e & 0x8000;
      if (b.bigTiles) {
        int subx = (px >> 3) & 1, suby = (py >> 3) & 1;
        if (hf) subx ^= 1;
        if (vf) suby ^= 1;
        tile += subx + suby * 16;
      }
      int fy = py & 7;
      if (vf) fy = 7 - fy;
      uint16_t adr = (b.charBase + (tile & 0x3ff) * tileWords + fy) & 0x7fff;
      uint16_t w0 = vram[adr];
      uint16_t w1 = bpp >= 4 ? vram[(adr + 8) & 0x7fff] : 0;
      uint16_t w2 = bpp == 8 ? vram[(adr + 16) & 0x7fff] : 0;
      uint16_t w3 = bpp == 8 ? vram[(adr + 24) & 0x7fff] : 0;
      for (int c = 0; c < 8; c++) {
        int s = hf ? c : 7 - c;
        uint8_t p = ((w0 >> s) & 1) | (((w0 >> (s + 8)) & 1) << 1);
        if (bpp >= 4) p |= (((w1 >> s) & 1) << 2) | (((w1 >> (s + 8)) & 1) << 3);
        if (bpp == 8) {
          p |= (((w2 >> s) & 1) << 4) | (((w2 >> (s + 8)) & 1) << 5);
          p |= (((w3 >> s) & 1) << 6) | (((w3 >> (s + 8)) & 1) << 7);
        }
        pix[c] = p;
      }
    }
    uint8_t p = pix[px & 7];
    if (!p) {
      out[x].pri = 0;
      continue;
    }
    uint16_t col;
    if (direct) col = directColor(p, cPal);
    else if (bpp == 8) col = cgram[p];
    else if (bpp == 4) col = cgram[(cPal << 4) + p];
    else col = cgram[palBase + (cPal << 2) + p];
    out[x].col = col;
    out[x].pri = cPri;
    out[x].pal = cPal;
  }
}

void Ppu::renderMode7(int l, int row, int x0, int x1) {
  Px* out = layer_[l] + kMaxExt;
  int line = row + 1;
  int msize = bg_[l].mosaic ? mosaicSize_ : 1;
  int ly = line;
  if (msize > 1) ly = line - ((line - 1) % msize);
  int yy = (m7sel_ & 2) ? 255 - ly : ly;
  auto clip = [](int v) { return (v & 0x2000) ? (v | ~0x3ff) : (v & 0x3ff); };
  int hofs = m7hofs_, vofs = m7vofs_, cx = m7x_, cy = m7y_;
  int ox = ((m7a_ * clip(hofs - cx)) & ~63) + ((m7b_ * clip(vofs - cy)) & ~63) + ((m7b_ * yy) & ~63) + (cx << 8);
  int oy = ((m7c_ * clip(hofs - cx)) & ~63) + ((m7d_ * clip(vofs - cy)) & ~63) + ((m7d_ * yy) & ~63) + (cy << 8);
  bool direct = (cgwsel_ & 1) && l == 0;
  for (int x = x0; x < x1; x++) {
    bool margin = x < 0 || x >= 256;
    if (margin && hideWideBg[l]) {
      out[x].pri = 0;
      continue;
    }
    int sx = x;
    if (msize > 1) sx = x - floorMod(x, msize);
    int xx = (m7sel_ & 1) ? 255 - sx : sx;
    int X = (ox + m7a_ * xx) >> 8;
    int Y = (oy + m7c_ * xx) >> 8;
    bool outside = (X & ~1023) || (Y & ~1023);
    uint8_t tile;
    if (outside && (m7sel_ & 0x80)) {
      if (m7sel_ & 0x40) tile = 0;
      else {
        out[x].pri = 0;
        continue;
      }
    } else {
      tile = vram[((Y >> 3) & 127) * 128 + ((X >> 3) & 127)] & 0xff;
    }
    uint8_t p = vram[(tile * 64 + (Y & 7) * 8 + (X & 7)) & 0x7fff] >> 8;
    uint8_t pri = 1;
    if (l == 1) {  // EXTBG
      pri = (p & 0x80) ? 2 : 1;
      p &= 0x7f;
    }
    if (!p) {
      out[x].pri = 0;
      continue;
    }
    out[x].col = direct ? directColor(p, 0) : cgram[p];
    out[x].pri = pri;
    out[x].pal = 0;
  }
}

void Ppu::renderObj(int row, int x0, int x1) {
  Px* out = layer_[4] + kMaxExt;
  for (int x = x0; x < x1; x++) out[x].pri = 0;

  int first = oamPriority_ ? ((oamAdrReload_ >> 1) & 0x7f) : 0;
  int list[128];
  int count = 0, coreCount = 0;
  int rightLimit = 256 + extR;
  // 9-bit X covers 512 px; values past the right margin wrap to the left.
  // Split the part neither margin shows at its midpoint, so a sprite just off
  // either edge of lopsided margins doesn't reappear in the other one. (For
  // 4:3 this matches the hardware: sprites are at most 64 px wide.)
  int split = rightLimit + std::max(0, 256 - extL - extR) / 2;
  for (int i = 0; i < 128; i++) {
    int idx = (first + i) & 127;
    uint8_t hi = oam[512 + (idx >> 2)] >> ((idx & 3) * 2);
    bool big = hi & 2;
    int w = kObjSizes[objSize_][big][0];
    int h = kObjSizes[objSize_][big][1];
    int y = (oam[idx * 4 + 1] + (sub_ ? objSubDy_[idx] : 0)) & 0xff;
    int sy = (row - y) & 0xff;
    if (sy >= h) continue;
    int x9 = oam[idx * 4] | ((hi & 1) << 8);
    int sx = (objTrueXSet[idx] ? objTrueX[idx] : x9 < split ? x9 : x9 - 512) + (sub_ ? objSubDx_[idx] : 0);
    if (sx <= x0 - w || sx >= rightLimit) continue;  // x0 = -extL on full lines
    bool core = sx > -w && sx < 256;
    if (core) {
      // Only sprites touching the original 256px area count towards the
      // hardware limit, so widescreen margins never change 4:3 behavior.
      if (coreCount >= 32 && !noSpriteLimit) {
        rangeOver_ = true;
        continue;
      }
      coreCount++;
    }
    list[count++] = idx;
  }

  // (Sprites with their own colours (SMB1 co-op's Luigi) go behind the rest:
  // his OAM entries come before Mario's, and overlapping him cut Mario in half.)
  bool anyExtra = false;
  for (const ObjExtra& g : objExtra) anyExtra = anyExtra || g.mask[0] || g.mask[1];
  if (objAltMask[0] | objAltMask[1] || anyExtra)
    std::stable_partition(list, list + count, [&](int idx) {
      return !((objAltMask[idx >> 6] >> (idx & 63)) & 1) && (!anyExtra || objExtraOf(idx) < 0);
    });
  int tiles = 0;
  for (int k = count - 1; k >= 0; k--) {
    int idx = list[k];
    uint8_t hi = oam[512 + (idx >> 2)] >> ((idx & 3) * 2);
    bool big = hi & 2;
    int w = kObjSizes[objSize_][big][0];
    int h = kObjSizes[objSize_][big][1];
    int y = (oam[idx * 4 + 1] + (sub_ ? objSubDy_[idx] : 0)) & 0xff;
    int x9 = oam[idx * 4] | ((hi & 1) << 8);
    int sx = (objTrueXSet[idx] ? objTrueX[idx] : x9 < split ? x9 : x9 - 512) + (sub_ ? objSubDx_[idx] : 0);
    for (int z = 0; z < objZoneCount; z++) {
      const PpuObjZone& zn = objZones[z];
      int ty = y >= 240 ? y - 256 : y;
      if (sx >= zn.x0 && sx < zn.x1 && ty >= zn.y0 && ty < zn.y1) {
        sx += zn.dx;
        break;
      }
    }
    uint8_t attr = oam[idx * 4 + 3];
    int tileBase = oam[idx * 4 + 2] | ((attr & 1) << 8);
    int pal = (attr >> 1) & 7;
    const bool alt = (objAltMask[idx >> 6] >> (idx & 63)) & 1;
    const bool alt2 = (objAlt2Mask[idx >> 6] >> (idx & 63)) & 1;
    const ObjExtra* ex = nullptr;
    if (anyExtra && !alt)
      if (int g = objExtraOf(idx); g >= 0) ex = &objExtra[g];
    int pri = ((attr >> 4) & 3) + 1;
    bool hf = attr & 0x40, vf = attr & 0x80;
    int sy = (row - y) & 0xff;
    if (vf) {
      // Rectangular sprites flip within their height.
      sy = h - 1 - sy;
    }
    int tileRow = sy >> 3;
    int fy = sy & 7;
    int nTiles = w >> 3;
    for (int c = 0; c < nTiles; c++) {
      int tx = sx + c * 8;
      if (tx <= x0 - 8 || tx >= x1) continue;
      bool core = tx > -8 && tx < 256;
      if (core && !noSpriteLimit) {
        if (tiles >= 34) {
          timeOver_ = true;
          continue;
        }
        tiles++;
      }
      int col = hf ? nTiles - 1 - c : c;
      int t = ((tileBase & 0xf0) + (tileRow << 4)) & 0xf0;
      t |= (tileBase + col) & 0x0f;
      uint16_t adr = objBase_ + t * 16;
      if (tileBase & 0x100) adr += (objNameSel_ + 1) << 12;
      adr = (adr + fy) & 0x7fff;
      uint16_t w0 = vram[adr], w1 = vram[(adr + 8) & 0x7fff];
      if (alt && (uint16_t)(adr - objAltTileBase) < objAltTileWords) {
        w0 = objAltTiles[adr - objAltTileBase];
        uint16_t a1 = (adr + 8) & 0x7fff;
        if ((uint16_t)(a1 - objAltTileBase) < objAltTileWords) w1 = objAltTiles[a1 - objAltTileBase];
      } else if (ex && (uint16_t)(adr - ex->tileBase) < ex->tileWords) {
        w0 = ex->tiles[adr - ex->tileBase];
        uint16_t a1 = (adr + 8) & 0x7fff;
        if ((uint16_t)(a1 - ex->tileBase) < ex->tileWords) w1 = ex->tiles[a1 - ex->tileBase];
      }
      for (int px = 0; px < 8; px++) {
        int x = tx + px;
        if (x < x0 || x >= x1 || x < objShowLo || x >= objShowHi) continue;
        int s = hf ? px : 7 - px;
        uint8_t p = ((w0 >> s) & 1) | (((w0 >> (s + 8)) & 1) << 1) | (((w1 >> s) & 1) << 2) |
                    (((w1 >> (s + 8)) & 1) << 3);
        if (!p) continue;
        out[x].col = alt                                  ? objAltColors[p]
                     : ex                                 ? ex->colors[p]
                     : alt2                               ? objAlt2Colors[p]
                     : (objPalOverrideMask & (1 << pal)) ? objPalOverride[pal][p]
                                                          : cgram[128 + pal * 16 + p];
        out[x].pri = pri;
        out[x].pal = pal;
      }
    }
  }
}

void Ppu::computeWindows(int x0, int x1) {
  for (int l = 0; l < 6; l++) {
    uint8_t sel = winSel_[l];
    uint8_t* m = winMask_[l] + kMaxExt;
    bool en1 = sel & 2, en2 = sel & 8;
    if (!en1 && !en2) {
      for (int x = x0; x < x1; x++) m[x] = 0;
      continue;
    }
    bool inv1 = sel & 1, inv2 = sel & 4;
    int logic = winLogic_[l];
    for (int x = x0; x < x1; x++) {
      int ex = x - winShift;
      bool w1, w2;
      if ((ex < 0 || ex > 255) && wideWindowMode == 1) {
        w1 = inv1;
        w2 = inv2;
      } else {
        if (ex < 0) ex = 0;
        if (ex > 255) ex = 255;
        w1 = (ex >= winL_[0] && ex <= winR_[0]) != inv1;
        w2 = (ex >= winL_[1] && ex <= winR_[1]) != inv2;
      }
      bool r;
      if (en1 && !en2) r = w1;
      else if (!en1 && en2) r = w2;
      else {
        switch (logic) {
          case 0: r = w1 || w2; break;
          case 1: r = w1 && w2; break;
          case 2: r = w1 != w2; break;
          default: r = w1 == w2; break;
        }
      }
      m[x] = r;
    }
  }
}

// A layer line of a screen drawn as one 256 px picture, carried on over the
// margins: per side, the shortest span (1-96 px) whose pixels the next 32 px
// (at least twice its width) repeat, starting at the edge or, where the edge
// column is blank (SMB2's), after it, covering that column too. Where none
// repeats, the edge pixel goes on (after a blank column), the line is
// mirrored or the margin left transparent (edge modes 1-3).
void Ppu::edgeRemap(int l, PpuLayerRemap* rm) {
  const Px* src = layer_[l] + kMaxExt;
  auto same = [&](int a, int b) {
    return src[a].pri == src[b].pri && (src[a].pri == 0 || src[a].col == src[b].col);
  };
  // (pixel at step k from the edge inwards)
  auto at = [](bool right, int k) { return right ? 255 - k : k; };
  auto find = [&](bool right, int* skip, int* period) {
    // (a blank edge column: all transparent or one colour, unlike the next)
    bool blank = !same(at(right, 0), at(right, 8));
    for (int k = 1; k < 8 && blank; k++) blank = same(at(right, 0), at(right, k));
    for (int s = 0; s <= (blank ? 8 : 0); s += 8)
      for (int p = 1; p <= 96; p++) {
        int n = std::max(32, 2 * p), k = 0;
        while (k < n && s + k + p < 256 && same(at(right, s + k), at(right, s + k + p))) k++;
        if (k == n) {
          *skip = s;
          *period = p;
          return true;
        }
      }
    *skip = blank ? 8 : 0;
    *period = 1;
    return false;
  };
  int sl, pl, sr, pr;
  bool fl = find(false, &sl, &pl), fr = find(true, &sr, &pr);
  int L = extL, R = extR, DL = rm->mirror ? rm->mirror : L, DR = rm->mirror ? rm->mirror : R;
  if (rm->mirror == 0xff) {
    // The shape the edge cuts through: from the edge (or up to 16 px in, as
    // near a hill's top) to the first clear pixel after it.
    auto run = [&](bool right, int cap) {
      int k = 0;
      while (k < 16 && k < cap && !src[at(right, k)].pri) k++;
      if (k == 16 || k == cap) return 0;
      while (k < cap && src[at(right, k)].pri) k++;
      return k;
    };
    DL = run(false, L);
    DR = run(true, R);
  }
  int n = 0;
  auto add = [&](int d0, int d1, int s0, int p) { rm->seg[n++] = {(int16_t)d0, (int16_t)d1, (int16_t)s0, (int16_t)p}; };
  // (a source far off the line: transparent)
  if (fl || rm->edge == 1) {
    add(sl - (L + sl + pl - 1) / pl * pl, sl, sl, pl);
  } else if (rm->edge == 2) {
    add(-std::min(L, DL), 0, std::min(L, DL) - 1, -1);
    if (L > DL) add(-L, -DL, -20000, 0);
  } else {
    add(-L, 0, -20000, 0);
  }
  add(sl, 256 - sr, sl, 0);
  if (fr || rm->edge == 1) {
    add(256 - sr, 256 + R, 256 - sr - pr, pr);
  } else if (rm->edge == 2) {
    add(256, 256 + std::min(R, DR), 255, -1);
    if (R > DR) add(256 + DR, 256 + R, -20000, 0);
  } else {
    add(256, 256 + R, -20000, 0);
  }
  rm->count = n;
}

void Ppu::applyRemap(int l, int x0, int x1) {
  PpuLayerRemap& rm = layerRemap[l];
  if (rm.edge) edgeRemap(l, &rm);
  if (!rm.count) return;
  static Px tmp[kLineW];
  Px* src = layer_[l] + kMaxExt;
  Px* dst = tmp + kMaxExt;
  for (int x = x0; x < x1; x++) dst[x].pri = 0;
  for (int s = 0; s < rm.count; s++) {
    const PpuRemapSeg& sg = rm.seg[s];
    int a = std::max<int>(sg.dst0, x0), b = std::min<int>(sg.dst1, x1);
    for (int x = a; x < b; x++) {
      int sx = sg.period > 0   ? sg.src0 + floorMod(x - sg.dst0, sg.period)
               : sg.period < 0 ? sg.src0 - (x - sg.dst0)
                               : sg.src0 + (x - sg.dst0);
      if (sx < x0 || sx >= x1) continue;
      dst[x] = src[sx];
    }
  }
  for (int x = x0; x < x1; x++) src[x] = dst[x];
}

void Ppu::renderLine(int line) {
  int row = line - 1;
  if (row < 0 || row >= kFrameH) return;
  uint32_t* fb = frame + row * kLineW;

  // Per-line hook configuration (the hook may also change extL/extR at row 0)
  for (auto& r : layerRemap) r.count = r.edge = r.mirror = 0;
  objZoneCount = 0;
  objAltMask[0] = objAltMask[1] = 0;
  objAlt2Mask[0] = objAlt2Mask[1] = 0;
  for (ObjExtra& g : objExtra) g.mask[0] = g.mask[1] = 0;
  for (int l = 0; l < 4; l++) {
    hideWideBg[l] = false;
    bgHofsAdj[l] = 0;
    wideFetchAll[l] = false;
  }
  hideWideObj = false;
  maskWide = false;
  subStill = false;
  dimLeft = dimRight = 0;
  wideWindowMode = 0;
  wideShowLo = -kMaxExt;
  wideShowHi = 256 + kMaxExt;
  objShowLo = -kMaxExt;
  objShowHi = 256 + kMaxExt;
  winShift = 0;
  if (hooks) hooks->beginLine(*this, row);

  int eL = extL, eR = extR;
  int x0 = -eL, x1 = 256 + eR;
  const int subs = subFrames < 1 ? 1 : subFrames > kMaxSubFrames ? kMaxSubFrames : subFrames;
  if (subs > 1) {
    // (this line's scroll, kept for the next frame's in-between pictures; at
    // the frame's first line, the sprites' moves since the last)
    if (row == 0) {
      scrollCur_ ^= 1;
      objMoves();
      prevLit_ = curLit_;
      curLit_ = false;
    }
    lineExt_[scrollCur_][row] = (int16_t)eL;
    for (int l = 0; l < 4; l++) {
      lineScroll_[scrollCur_][l][row][0] = (int16_t)((bg_[l].hofs + bgHofsAdj[l]) & 0x3ff);
      lineScroll_[scrollCur_][l][row][1] = (int16_t)(bg_[l].vofs & 0x3ff);
    }
  }
  if (forcedBlank_) {
    for (int x = 0; x < kLineW; x++) fb[x] = 0;
    for (int k = 1; k < subs; k++)
      for (int x = 0; x < kLineW; x++) subFrame[k - 1][row * kLineW + x] = 0;
    return;
  }
  if (skipRender) return;

  if (brightness_ > 0) curLit_ = true;
  uint8_t used = (tm_ | ts_) & debugLayers;
  for (int l = 0; l < 4; l++) {
    Px* o = layer_[l] + kMaxExt;
    bool present = mode_ == 7 ? (l == 0 || (l == 1 && (setini_ & 0x40))) : kBpp[mode_][l] != 0;
    if (!present || !(used & (1 << l))) {
      for (int x = x0; x < x1; x++) o[x].pri = 0;
      continue;
    }
    if (mode_ == 7) renderMode7(l, row, x0, x1);
    else renderBg(l, row, x0, x1);
    applyRemap(l, x0, x1);
  }
  if (used & 0x10) {
    renderObj(row, x0, x1);
    if (hideWideObj) {
      Px* o = layer_[4] + kMaxExt;
      for (int x = x0; x < 0; x++) o[x].pri = 0;
      for (int x = 256; x < x1; x++) o[x].pri = 0;
    }
  } else {
    Px* o = layer_[4] + kMaxExt;
    for (int x = x0; x < x1; x++) o[x].pri = 0;
  }
  computeWindows(x0, x1);
  composeLine(fb, x0, x1, eL, eR);
  if (subs > 1) std::memcpy(layerMain_, layer_, sizeof layerMain_);
  for (int k = 1; k < subs; k++) renderSubLine(row, k, x0, x1, eL, eR);
}

// The sprites' moves since the last frame. Games reorder their OAM entries
// from frame to frame (SMB3, SMB1: the flicker rotation), so a sprite is
// matched by its look, not its entry: last frame's sprite of the same tile,
// palette, flips, size (and co-op recolouring), a short way off, each one
// taken once, nearest pairs first (a trail of fireballs, Larry's rings: each
// its own). Unless last frame had a sprite of its palette and size right
// where it is: then it's that one, animated (a Goomba's walk flips it: its
// look matched the other Goomba's, 16 px off, and the two slid through each
// other). Otherwise it moves as the matched sprite of its palette nearest it
// (the rest of the character); failing that, one not drawn near there last
// frame (flickered) is looked for the frame before, its move halved; else
// it's new, not moved. (Sprites parked below the picture match nothing.)
void Ppu::objMoves() {
  constexpr int kMatch = 20;
  int rightLimit = 256 + extR;
  int split = rightLimit + std::max(0, 256 - extL - extR) / 2;
  int curX[128], curY[128], curW[128], nearD[128], nearP[128];
  uint16_t curKey[128];
  bool curBig[128], curOn[128], matched[128], taken[128], exact[128];
  const int fh = frameHeight();
  for (int idx = 0; idx < 128; idx++) {
    uint8_t hi = oam[512 + (idx >> 2)] >> ((idx & 3) * 2);
    bool big = hi & 2;
    int x9 = oam[idx * 4] | ((hi & 1) << 8);
    int y = oam[idx * 4 + 1];
    int h = kObjSizes[objSize_][big][1];
    curX[idx] = objTrueXSet[idx] ? objTrueX[idx] : x9 < split ? x9 : x9 - 512;
    curY[idx] = y;
    curW[idx] = kObjSizes[objSize_][big][0];
    curBig[idx] = big;
    // (tile, name, palette, flips; bit 12: recoloured, co-op's Luigi)
    curKey[idx] = (uint16_t)(oam[idx * 4 + 2] | (oam[idx * 4 + 3] & 0xcf) << 8 |
                             (((objAltMask[idx >> 6] >> (idx & 63)) & 1) << 12) |
                             ((objExtraOf(idx) >= 0) << 13));  // (bit 13: another player's)
    curOn[idx] = !(y >= fh && y + h <= 256);
    objDx_[idx] = objDy_[idx] = 0;
    matched[idx] = taken[idx] = exact[idx] = false;
  }
  auto dyOf = [](int y, int py) { return (int)(int8_t)(uint8_t)(y - py); };
  auto look = [](uint16_t k) { return k & 0x3e00; };  // (palette, recoloured)
  // Last frame's sprite of its palette and size nearest its place (within
  // half its width).
  for (int idx = 0; idx < 128; idx++) {
    nearD[idx] = 1 << 30;
    nearP[idx] = -1;
    if (!curOn[idx]) continue;
    int lim = curW[idx] / 2;
    for (int p = 0; p < 128; p++) {
      if (!objPrevOk_[p] || look(objPrevKey_[p]) != look(curKey[idx]) || objPrevBig_[p] != curBig[idx]) continue;
      int dx = curX[idx] - objPrevX_[p], dy = dyOf(curY[idx], objPrevY_[p]);
      if (std::abs(dx) >= lim || std::abs(dy) >= lim) continue;
      int d = std::abs(dx) + std::abs(dy);
      if (d < nearD[idx]) nearD[idx] = d, nearP[idx] = p;
    }
  }
  // The same look: pairs nearest first, each sprite once.
  struct Pair {
    int d, idx, p;
  };
  static std::vector<Pair> pairs;
  pairs.clear();
  for (int idx = 0; idx < 128; idx++) {
    if (!curOn[idx]) continue;
    for (int p = 0; p < 128; p++) {
      if (!objPrevOk_[p] || objPrevKey_[p] != curKey[idx] || objPrevBig_[p] != curBig[idx]) continue;
      int dx = curX[idx] - objPrevX_[p], dy = dyOf(curY[idx], objPrevY_[p]);
      if (std::abs(dx) > kMatch || std::abs(dy) > kMatch) continue;
      int d = std::abs(dx) + std::abs(dy);
      if (nearP[idx] >= 0 && d > nearD[idx] + 2) continue;  // (something was right here: animated)
      pairs.push_back({d * 2 + (p != idx), idx, p});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
  for (const Pair& pr : pairs) {
    if (matched[pr.idx] || taken[pr.p]) continue;
    objDx_[pr.idx] = (int16_t)(curX[pr.idx] - objPrevX_[pr.p]);
    objDy_[pr.idx] = (int16_t)dyOf(curY[pr.idx], objPrevY_[pr.p]);
    matched[pr.idx] = taken[pr.p] = exact[pr.idx] = true;
  }
  // Animated in place.
  for (int idx = 0; idx < 128; idx++) {
    if (!curOn[idx] || matched[idx] || nearP[idx] < 0) continue;
    objDx_[idx] = (int16_t)(curX[idx] - objPrevX_[nearP[idx]]);
    objDy_[idx] = (int16_t)dyOf(curY[idx], objPrevY_[nearP[idx]]);
    matched[idx] = true;
  }
  for (int idx = 0; idx < 128; idx++) {
    if (!curOn[idx] || matched[idx]) continue;
    // A matched neighbour of its palette.
    int best = -1, bestD = 16 + 1;
    for (int j = 0; j < 128; j++) {
      if (!matched[j] || look(curKey[j]) != look(curKey[idx])) continue;
      int d = std::max(std::abs(curX[j] - curX[idx]), std::abs(dyOf(curY[j], curY[idx])));
      if (d < bestD) bestD = d, best = j;
    }
    if (best >= 0) {
      objDx_[idx] = objDx_[best];
      objDy_[idx] = objDy_[best];
      continue;
    }
    // Not drawn near here last frame (flickered): the frame before's, half the way.
    bool drawnNear = false;
    for (int p = 0; p < 128 && !drawnNear; p++)
      drawnNear = objPrevOk_[p] && look(objPrevKey_[p]) == look(curKey[idx]) &&
                  std::abs(curX[idx] - objPrevX_[p]) <= kMatch && std::abs(dyOf(curY[idx], objPrevY_[p])) <= kMatch;
    if (drawnNear) continue;
    best = -1;
    bestD = 1 << 30;
    for (int p = 0; p < 128; p++) {
      if (!objPrev2Ok_[p] || objPrev2Key_[p] != curKey[idx] || objPrev2Big_[p] != curBig[idx]) continue;
      int dx = curX[idx] - objPrev2X_[p], dy = dyOf(curY[idx], objPrev2Y_[p]);
      if (std::abs(dx) > kMatch || std::abs(dy) > kMatch) continue;
      int d = (std::abs(dx) + std::abs(dy)) * 2 + (p != idx);
      if (d < bestD) bestD = d, best = p;
    }
    if (best >= 0) {
      objDx_[idx] = (int16_t)((curX[idx] - objPrev2X_[best]) / 2);
      objDy_[idx] = (int16_t)(dyOf(curY[idx], objPrev2Y_[best]) / 2);
    }
  }
  // One character moves as one. Touching sprites of a palette are its
  // pieces; each offers every move that puts a sprite of exactly its look
  // (tile, palette, flips) last frame where it came from, and a move clearly
  // most of the pieces offer is the character's, for the pieces whose own
  // move was a guess (animated in place, a neighbour's, none). (Raccoon
  // Mario, three columns 8 px apart, ran 6 px a frame: each piece was 2 px
  // from a neighbour's place, taken for itself animated in place, and on the
  // in-between pictures he slid 8 px off and tore, a Piranha Plant he
  // overlapped with him.) A piece matched by its exact look keeps its move,
  // and so does one with a sprite of its look where its guess says it came
  // from: touching sprites of a palette can be things moving apart (Bowser's
  // bridge falling piece by piece). Two Goombas that swapped their walk flip
  // offer each other's places, no majority: the rules above stand. (Checked
  // on the trailer clips, the first in-between at t = 0 against the last
  // frame: smastool objgroup 0/1 and subdiff.)
  if (objGroupMoves) {
    int group[128];
    for (int i = 0; i < 128; i++) group[i] = i;
    auto root = [&](int i) {
      while (group[i] != i) i = group[i] = group[group[i]];
      return i;
    };
    for (int i = 0; i < 128; i++) {
      if (!curOn[i]) continue;
      const int hi = kObjSizes[objSize_][curBig[i]][1];
      for (int j = i + 1; j < 128; j++) {
        if (!curOn[j] || look(curKey[j]) != look(curKey[i])) continue;
        const int hj = kObjSizes[objSize_][curBig[j]][1];
        const int dy = dyOf(curY[j], curY[i]);
        if (curX[j] > curX[i] + curW[i] || curX[i] > curX[j] + curW[j] || dy > hi || -dy > hj) continue;
        group[root(j)] = root(i);
      }
    }
    for (int i = 0; i < 128; i++) {
      if (!curOn[i] || exact[i]) continue;
      for (int p = 0; p < 128 && !exact[i]; p++)
        exact[i] = objPrevOk_[p] && objPrevKey_[p] == curKey[i] && objPrevBig_[p] == curBig[i] &&
                   curX[i] - objPrevX_[p] == objDx_[i] && dyOf(curY[i], objPrevY_[p]) == objDy_[i];
    }
    static std::vector<std::pair<int, int>> offers;  // (move, piece)
    for (int r = 0; r < 128; r++) {
      if (!curOn[r] || root(r) != r) continue;
      offers.clear();
      int n = 0;
      for (int i = 0; i < 128; i++) {
        if (!curOn[i] || root(i) != r) continue;
        n++;
        for (int p = 0; p < 128; p++) {
          if (!objPrevOk_[p] || objPrevKey_[p] != curKey[i] || objPrevBig_[p] != curBig[i]) continue;
          int dx = curX[i] - objPrevX_[p], dy = dyOf(curY[i], objPrevY_[p]);
          if (std::abs(dx) > kMatch || std::abs(dy) > kMatch) continue;
          offers.push_back({(dx + 64) << 8 | (dy + 64), i});
        }
      }
      if (n < 3 || offers.empty()) continue;
      std::sort(offers.begin(), offers.end());
      offers.erase(std::unique(offers.begin(), offers.end()), offers.end());  // (a piece offers a move once)
      int best = -1, bestN = 0, second = 0;
      for (size_t a = 0; a < offers.size();) {
        size_t b = a;
        while (b < offers.size() && offers[b].first == offers[a].first) b++;
        const int c = (int)(b - a);
        if (c > bestN) second = bestN, bestN = c, best = offers[a].first;
        else if (c > second) second = c;
        a = b;
      }
      // (clearly: 3 pieces and two thirds; a plant's animated mouth tiles
      // swap places, two of its four pieces offering a wrong move)
      if (bestN < 3 || bestN * 3 < n * 2 || bestN == second) continue;
      const int mdx = (best >> 8) - 64, mdy = (best & 255) - 64;
      for (int i = 0; i < 128; i++)
        if (curOn[i] && !exact[i] && root(i) == r) objDx_[i] = (int16_t)mdx, objDy_[i] = (int16_t)mdy;
    }
  }
  std::memcpy(objPrev2X_, objPrevX_, sizeof objPrevX_);
  std::memcpy(objPrev2Y_, objPrevY_, sizeof objPrevY_);
  std::memcpy(objPrev2Key_, objPrevKey_, sizeof objPrevKey_);
  std::memcpy(objPrev2Big_, objPrevBig_, sizeof objPrevBig_);
  std::memcpy(objPrev2Ok_, objPrevOk_, sizeof objPrevOk_);
  for (int idx = 0; idx < 128; idx++) {
    objPrevX_[idx] = (int16_t)curX[idx];
    objPrevY_[idx] = (int16_t)curY[idx];
    objPrevKey_[idx] = curKey[idx];
    objPrevBig_[idx] = curBig[idx];
    objPrevOk_[idx] = curOn[idx];
  }
}

// An in-between picture's line (t = k / subFrames): each layer drawn again
// with its scroll part of the way back to the last frame's on this line (a
// change too big for a move, a cut, isn't), vertically through the scroll,
// horizontally by moving the drawn pixels (the margins, drawn by the game
// layer at the line's own scroll, go along); sprites part of the way back.
// (After a black frame (a fade, the SMB3 iris) nothing comes from it.)
void Ppu::renderSubLine(int row, int k, int x0, int x1, int eL, int eR) {
  constexpr int kMaxMove = 32;
  const float t = subTimed ? subAt[k - 1] : (float)k / std::max(subFrames, 2);
  const float back = prevLit_ ? 1.0f - t : 0.0f;
  const int prev = scrollCur_ ^ 1;
  // The view itself may have slid on the screen since the last frame (the
  // left margin changing: a narrow room eased to the middle, the view shifted
  // at a level's end): what moved with it goes part of the way back too, or
  // the room would jump to its new place on the in-between pictures and back
  // on the frames, a shake. Line by line: the HUD's lines keep their margin.
  const int dExtRaw = lineExt_[scrollCur_][row] - lineExt_[prev][row];
  const int dExt = std::abs(dExtRaw) <= kMaxMove ? dExtRaw : 0;
  sub_ = true;
  uint8_t used = (tm_ | ts_) & debugLayers;
  static Px tmp[kLineW];
  for (int l = 0; l < 4; l++) {
    Px* o = layer_[l] + kMaxExt;
    bool present = mode_ == 7 ? (l == 0 || (l == 1 && (setini_ & 0x40))) : kBpp[mode_][l] != 0;
    if (!present || !(used & (1 << l))) {
      for (int x = x0; x < x1; x++) o[x].pri = 0;
      continue;
    }
    if (mode_ == 7) {
      renderMode7(l, row, x0, x1);
      applyRemap(l, x0, x1);
      continue;
    }
    auto wrap10 = [](int d) { return ((d + 512) & 1023) - 512; };
    int dh = wrap10(lineScroll_[scrollCur_][l][row][0] - lineScroll_[prev][l][row][0]);
    int dv = wrap10(lineScroll_[scrollCur_][l][row][1] - lineScroll_[prev][l][row][1]);
    if (std::abs(dh) > kMaxMove || std::abs(dv) > kMaxMove || subStill) dh = dv = 0;  // (a cut either way)
    // (its move on the screen: scrolled on by dh, the view slid by dExt; a
    // layer the game places itself, SMB3's status bar centred on the display,
    // was placed for the slide already)
    if (!layerRemap[l].count) dh -= dExt;
    subVofs_[l] = (int16_t)-std::lround(back * dv);
    const int shift = (int)std::lround(back * dh);  // (scrolled on by dh: the picture back that way)
    // (4:3: the columns shifted in drawn too, the map carrying on; wide, the
    // margins' drawing ends at the display's edges (SMB2 gave blank tiles
    // past them, a stripe of the backdrop): its edge column is repeated.)
    const bool wide = eL || eR;
    const int ex0 = wide ? x0 : std::max(x0 - std::abs(shift), -kMaxExt);
    const int ex1 = wide ? x1 : std::min(x1 + std::abs(shift), 256 + kMaxExt);
    if (subVofs_[l] == 0 && (wide || shift == 0)) {
      // Only moved across (or not at all): the frame's own line of the layer,
      // shifted, without drawing it again. (Drawing every layer again for
      // each in-between cost as much as the frame: at 144 Hz, 2-3 of them a
      // frame, the pass outlasted a refresh and pictures were shown twice.)
      std::memcpy(o + x0, layerMain_[l] + kMaxExt + x0, (size_t)(x1 - x0) * sizeof(Px));
    } else {
      renderBg(l, row, ex0, ex1);
      applyRemap(l, x0, x1);
    }
    if (shift) {
      Px* t = tmp + kMaxExt;
      for (int x = ex0; x < ex1; x++) t[x] = o[x];
      for (int x = x0; x < x1; x++) o[x] = t[std::clamp(x - shift, ex0, ex1 - 1)];
    }
  }
  if (used & 0x10) {
    for (int i = 0; i < 128; i++) {
      objSubDx_[i] = (int16_t)-std::lround(back * (objDx_[i] + dExt));
      objSubDy_[i] = (int16_t)-std::lround(back * objDy_[i]);
    }
    renderObj(row, x0, x1);
    if (hideWideObj) {
      Px* o = layer_[4] + kMaxExt;
      for (int x = x0; x < 0; x++) o[x].pri = 0;
      for (int x = 256; x < x1; x++) o[x].pri = 0;
    }
  } else {
    Px* o = layer_[4] + kMaxExt;
    for (int x = x0; x < x1; x++) o[x].pri = 0;
  }
  sub_ = false;
  // (the windows are the line's own, worked out for the frame: winMask_ as it left them)
  composeLine(subFrame[k - 1] + row * kLineW, x0, x1, eL, eR);
}

// The line's layers composed into its pixels (priority, windows, colour maths,
// brightness, the margins' dimming and masks).
void Ppu::composeLine(uint32_t* fb, int x0, int x1, int eL, int eR) {
  const PriEntry* list;
  int n;
  switch (mode_) {
    case 0: list = kMode0; n = sizeof(kMode0) / sizeof(PriEntry); break;
    case 1:
      if (bg3Priority_) { list = kMode1a; n = sizeof(kMode1a) / sizeof(PriEntry); }
      else { list = kMode1b; n = sizeof(kMode1b) / sizeof(PriEntry); }
      break;
    case 6: list = kMode6; n = sizeof(kMode6) / sizeof(PriEntry); break;
    case 7: list = kMode7; n = sizeof(kMode7) / sizeof(PriEntry); break;
    default: list = kMode2; n = sizeof(kMode2) / sizeof(PriEntry); break;
  }

  uint16_t fixed = fixedR_ | (fixedG_ << 5) | (fixedB_ << 10);
  int clipMode = cgwsel_ >> 6;
  int mathMode = (cgwsel_ >> 4) & 3;
  bool useSub = cgwsel_ & 2;
  bool subtract = cgadsub_ & 0x80;
  bool halfFlag = cgadsub_ & 0x40;
  const uint8_t* lut = colorLut_[brightness_];
  const uint8_t* colWin = winMask_[5] + kMaxExt;

  for (int x = x0; x < x1; x++) {
    int mainL = 5, subL = 5;
    uint16_t mainC = cgram[0], subC = fixed;
    uint8_t mainPal = 0;
    for (int k = 0; k < n; k++) {
      int l = list[k].layer;
      if (!(tm_ & (1 << l))) continue;
      const Px& p = layer_[l][x + kMaxExt];
      if (p.pri != list[k].pri) continue;
      if ((tmw_ & (1 << l)) && winMask_[l][x + kMaxExt]) continue;
      mainL = l;
      mainC = p.col;
      mainPal = p.pal;
      break;
    }
    if (useSub) {
      for (int k = 0; k < n; k++) {
        int l = list[k].layer;
        if (!(ts_ & (1 << l))) continue;
        const Px& p = layer_[l][x + kMaxExt];
        if (p.pri != list[k].pri) continue;
        if ((tsw_ & (1 << l)) && winMask_[l][x + kMaxExt]) continue;
        subL = l;
        subC = p.col;
        break;
      }
    }
    bool cw = colWin[x];
    bool clip = clipMode == 3 || (clipMode == 1 && !cw) || (clipMode == 2 && cw);
    bool mathOk = mathMode == 0 || (mathMode == 1 && cw) || (mathMode == 2 && !cw);
    bool layerMath;
    if (mainL == 4) layerMath = (cgadsub_ & 0x10) && mainPal >= 4;
    else layerMath = cgadsub_ & (1 << mainL);

    int r = mainC & 31, g = (mainC >> 5) & 31, b = (mainC >> 10) & 31;
    if (clip) r = g = b = 0;
    if (mathOk && layerMath) {
      int sr = subC & 31, sg = (subC >> 5) & 31, sb = (subC >> 10) & 31;
      bool half = halfFlag && !clip && !(useSub && subL == 5);
      if (subtract) {
        r = std::max(0, r - sr);
        g = std::max(0, g - sg);
        b = std::max(0, b - sb);
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
      } else {
        r += sr;
        g += sg;
        b += sb;
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
        else {
          r = std::min(31, r);
          g = std::min(31, g);
          b = std::min(31, b);
        }
      }
    }
    int dim = x < 0 ? dimLeft : x >= 256 ? dimRight : 0;
    if (dim > 0 && mainL != 4) {  // (the margins' scenery; sprites, the HUD, as they are)
      int k = 256 - dim;
      fb[x + kMaxExt] = (uint32_t(lut[r] * k >> 8) << 16) | (uint32_t(lut[g] * k >> 8) << 8) | (lut[b] * k >> 8);
    } else {
      fb[x + kMaxExt] = (uint32_t(lut[r]) << 16) | (uint32_t(lut[g]) << 8) | lut[b];
    }
  }
  // clear unused area outside current extension
  for (int x = 0; x < kMaxExt - eL; x++) fb[x] = 0;
  for (int x = kMaxExt + 256 + eR; x < kLineW; x++) fb[x] = 0;
  if (maskWide) {
    for (int x = kMaxExt - eL; x < kMaxExt; x++) fb[x] = 0;
    for (int x = kMaxExt + 256; x < kMaxExt + 256 + eR; x++) fb[x] = 0;
  }
  for (int x = -eL; x < std::min(0, wideShowLo); x++) fb[x + kMaxExt] = 0;
  for (int x = std::max(256, wideShowHi); x < 256 + eR; x++) fb[x + kMaxExt] = 0;
}

int Ppu::bgBpp(int l) const { return kBpp[mode_][l]; }

std::string Ppu::debugString() const {
  char buf[1024];
  int n = snprintf(buf, sizeof buf,
                   "mode=%d bg3pri=%d TM=%02X TS=%02X TMW=%02X TSW=%02X CGWSEL=%02X CGADSUB=%02X bright=%d fblank=%d "
                   "objsel size=%d base=%04X W1=%d-%d W2=%d-%d\n",
                   mode_, bg3Priority_, tm_, ts_, tmw_, tsw_, cgwsel_, cgadsub_, brightness_, forcedBlank_, objSize_,
                   objBase_, winL_[0], winR_[0], winL_[1], winR_[1]);
  for (int l = 0; l < 4; l++) {
    n += snprintf(buf + n, sizeof buf - n, "BG%d map=%04X size=%d chr=%04X big=%d hofs=%d vofs=%d win=%X\n", l + 1,
                  bg_[l].mapBase, bg_[l].mapSize, bg_[l].charBase, bg_[l].bigTiles, bg_[l].hofs, bg_[l].vofs,
                  winSel_[l]);
  }
  return buf;
}

void Ppu::debugRenderMap(int l, uint32_t* out, int* w, int* h) const {
  const Bg& b = bg_[l];
  int ts = b.bigTiles ? 16 : 8;
  int mw = (b.mapSize & 1) ? 64 : 32, mh = (b.mapSize & 2) ? 64 : 32;
  *w = mw * ts;
  *h = mh * ts;
  int bpp = kBpp[mode_][l];
  if (!bpp) bpp = 2;
  int tileWords = bpp * 4;
  int palBase = mode_ == 0 ? l * 32 : 0;
  for (int py = 0; py < *h; py++) {
    for (int px = 0; px < *w; px++) {
      uint16_t e = mapEntry(l, px / ts, py / ts);
      int tile = e & 0x3ff, pal = (e >> 10) & 7;
      bool hf = e & 0x4000, vf = e & 0x8000;
      if (b.bigTiles) {
        int sx = (px >> 3) & 1, sy = (py >> 3) & 1;
        if (hf) sx ^= 1;
        if (vf) sy ^= 1;
        tile += sx + sy * 16;
      }
      int fy = py & 7, fx = px & 7;
      if (vf) fy = 7 - fy;
      if (hf) fx = 7 - fx;
      uint16_t adr = (b.charBase + (tile & 0x3ff) * tileWords + fy) & 0x7fff;
      uint16_t w0 = vram[adr], w1 = bpp >= 4 ? vram[(adr + 8) & 0x7fff] : 0;
      int s = 7 - fx;
      int p = ((w0 >> s) & 1) | (((w0 >> (s + 8)) & 1) << 1);
      if (bpp >= 4) p |= (((w1 >> s) & 1) << 2) | (((w1 >> (s + 8)) & 1) << 3);
      uint16_t c;
      if (!p) c = cgram[0];
      else if (bpp == 4) c = cgram[(pal << 4) + p];
      else if (bpp == 8) c = cgram[p];
      else c = cgram[palBase + (pal << 2) + p];
      int r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
      out[py * *w + px] = ((r << 3) << 16) | ((g << 3) << 8) | (bl << 3);
    }
  }
}

// ---------------------------------------------------------------------------
// Serialization: registers are copied as a POD block between two markers.

namespace {
template <typename T>
void putv(uint8_t*& p, const T& v) {
  std::memcpy(p, &v, sizeof(T));
  p += sizeof(T);
}
template <typename T>
void getv(const uint8_t*& p, T& v) {
  std::memcpy(&v, p, sizeof(T));
  p += sizeof(T);
}
}  // namespace

#define PPU_STATE_FIELDS(X)                                                                       \
  X(forcedBlank_) X(brightness_) X(objSize_) X(objBase_) X(objNameSel_) X(oamAdr_) X(oamAdrReload_) \
  X(oamPriority_) X(oamLatch_) X(mode_) X(bg3Priority_) X(mosaicSize_) X(mosaicStart_) X(bg_)       \
  X(scrollPrev_) X(scrollPrev2_) X(m7Prev_) X(vramAdr_) X(vramInc_) X(vramRemap_) X(vramIncOnHigh_) \
  X(vramReadBuf_) X(m7sel_) X(m7a_) X(m7b_) X(m7c_) X(m7d_) X(m7x_) X(m7y_) X(m7hofs_) X(m7vofs_)  \
  X(cgAdr_) X(cgSecond_) X(cgLatch_) X(winSel_) X(winL_) X(winR_) X(winLogic_) X(tm_) X(ts_)       \
  X(tmw_) X(tsw_) X(cgwsel_) X(cgadsub_) X(fixedR_) X(fixedG_) X(fixedB_) X(setini_) X(overscan_)  \
  X(interlace_) X(hCount_) X(vCount_) X(hFlip_) X(vFlip_) X(countersLatched_) X(ppu1Bus_)          \
  X(ppu2Bus_) X(rangeOver_) X(timeOver_) X(frameOdd_) X(vram) X(cgram) X(oam)

size_t Ppu::stateSize() const {
  size_t n = 0;
#define X(f) n += sizeof(f);
  PPU_STATE_FIELDS(X)
#undef X
  return n;
}

void Ppu::saveState(uint8_t* dst) const {
  uint8_t* p = dst;
#define X(f) putv(p, f);
  PPU_STATE_FIELDS(X)
#undef X
}

void Ppu::loadState(const uint8_t* src) {
  const uint8_t* p = src;
#define X(f) getv(p, f);
  PPU_STATE_FIELDS(X)
#undef X
  // Bring file data into range (see statefix.h); addresses masked where they
  // are used are left as they are, so valid states load bit-exact.
  for (bool* b : {&forcedBlank_, &oamPriority_, &bg3Priority_, &vramIncOnHigh_, &cgSecond_, &overscan_, &interlace_,
                  &hFlip_, &vFlip_, &countersLatched_, &rangeOver_, &timeOver_, &frameOdd_})
    fixBool(*b);
  for (Bg& b : bg_) {
    fixBool(b.bigTiles);
    fixBool(b.mosaic);
    b.hofs &= 0x3ff;
    b.vofs &= 0x3ff;
    b.mapSize &= 3;
  }
  brightness_ &= 15;
  objSize_ &= 7;
  oamAdr_ &= 0x3ff;
  oamAdrReload_ &= 0x1ff;
  mode_ &= 7;
  mosaicSize_ = (uint8_t)std::clamp<int>(mosaicSize_, 1, 16);
  if (vramInc_ != 1 && vramInc_ != 32 && vramInc_ != 128) vramInc_ = 1;
  vramRemap_ &= 3;
}
