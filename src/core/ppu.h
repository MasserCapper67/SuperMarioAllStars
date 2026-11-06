// SNES PPU (S-PPU1/2) scanline renderer with widescreen extension support.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

constexpr int kMaxExt = 224;                  // max extra pixels on each side
constexpr int kLineW = 256 + 2 * kMaxExt;     // framebuffer line width
constexpr int kFrameH = 240;

// Horizontal remap for a layer on one scanline (used to re-anchor HUDs).
// For output x in [dst0,dst1): source x = src0 + (x - dst0)  (or modulo period)
struct PpuRemapSeg {
  int16_t dst0, dst1;
  int16_t src0;
  int16_t period;  // 0 = linear copy, >0 = repeat source span of this width, -1 = mirrored (src0 - (x - dst0))
};
struct PpuLayerRemap {
  uint8_t count = 0;  // 0 = no remap
  PpuRemapSeg seg[8];
  // Instead of segments: carry the layer's line on over the margins from the
  // screen's edges, worked out on its pixels (see Ppu::edgeRemap). Where no
  // span repeats there: 1, the edge pixel is carried on; 2, the line is
  // mirrored (only `mirror` px into the margin if set, or with $FF as far as
  // the shape at the edge reaches; transparent beyond);
  // 3, the margin is left transparent.
  uint8_t edge = 0;
  uint8_t mirror = 0;
};
// Sprites whose top-left (screen coords) lies in the zone get shifted by dx.
struct PpuObjZone {
  int16_t x0, y0, x1, y1;
  int16_t dx;
};

class Ppu;
class PpuHooks {
public:
  virtual ~PpuHooks() = default;
  // Called before each visible line is rendered. `row` is 0-based output row.
  virtual void beginLine(Ppu& ppu, int row) {}
  // Called when a BG layer fetches a tilemap entry in the widescreen margin.
  // Return kWideVram to use VRAM, kWideEntry to use *entry, kWideBlank for a
  // transparent tile.
  enum { kWideVram = 0, kWideEntry = 1, kWideBlank = 2 };
  virtual int fetchWideTile(Ppu& ppu, int layer, int mapX, int mapY, int screenX, uint16_t* entry) {
    return kWideVram;
  }
  // Called for every VRAM word write (CPU or DMA).
  virtual void onVramWrite(Ppu& ppu, uint16_t adr) {}
};

class Ppu {
public:
  Ppu();
  void reset(bool hard);

  uint8_t read(uint8_t reg, uint8_t openBus);
  void write(uint8_t reg, uint8_t val);

  void frameStart();
  void vblankStart();
  void renderLine(int line);  // line = V counter (1..239)
  void latchCounters(uint16_t h, uint16_t v) {
    hCount_ = h;
    vCount_ = v;
    countersLatched_ = true;
  }
  bool forcedBlank() const { return forcedBlank_; }
  bool overscan() const { return overscan_; }

  // ---- widescreen / enhancement parameters
  // Active extension on each side (0..kMaxExt). extL + extR must be < 256 so
  // 9-bit sprite X coordinates stay unambiguous.
  int extL = 0, extR = 0;
  void setExt(int l, int r) {
    extL = l < 0 ? 0 : (l > kMaxExt ? kMaxExt : l);
    extR = r < 0 ? 0 : (r > kMaxExt ? kMaxExt : r);
  }
  bool noSpriteLimit = false;
  int wideWindowMode = 0;    // margins for this line's windows: 0 = extend edge values, 1 = outside of windows
  PpuHooks* hooks = nullptr;
  PpuLayerRemap layerRemap[5];  // per line, filled by hooks (BG1-4, OBJ unused)
  PpuObjZone objZones[8];
  int objZoneCount = 0;
  // Per-OBJ-palette color overrides (character color variants).
  uint8_t objPalOverrideMask = 0;
  uint16_t objPalOverride[8][16] = {};
  // Per-sprite colours (SMB1 co-op's Luigi): OAM entries set here (reset each
  // line) take objAltColors instead of their palette.
  uint64_t objAltMask[2] = {};
  uint16_t objAltColors[16] = {};
  // ... and their tiles in VRAM words [objAltTileBase, + objAltTileWords) from
  // here instead (Luigi's animation frame; the player's tiles in VRAM are Mario's).
  uint16_t objAltTiles[256] = {};
  uint16_t objAltTileBase = 0, objAltTileWords = 0;
  // ... and others with colours of their own only (SMB1 co-op: Luigi as the
  // game's player); reset each line too.
  uint64_t objAlt2Mask[2] = {};
  uint16_t objAlt2Colors[16] = {};
  // ... and more players' (SMB1 co-op's third to fifth), each group with
  // colours and tiles of its own as objAlt's (masks reset each line too).
  static constexpr int kObjExtra = 3;
  struct ObjExtra {
    uint64_t mask[2] = {};
    uint16_t colors[16] = {};
    uint16_t tiles[256] = {};
    uint16_t tileBase = 0, tileWords = 0;
  };
  ObjExtra objExtra[kObjExtra];
  int objExtraOf(int idx) const {
    for (int g = 0; g < kObjExtra; g++)
      if ((objExtra[g].mask[idx >> 6] >> (idx & 63)) & 1) return g;
    return -1;
  }
  bool hideWideBg[4] = {};   // hide BG layer pixels in the margins
  int16_t bgHofsAdj[4] = {};  // extra horizontal scroll for this line (per BG layer)
  bool wideFetchAll[4] = {};  // ask fetchWideTile for every pixel of the layer, not only margins
  uint8_t debugLayers = 0x1f;  // debugging: layers to draw (bit 0-3 BG1-4, bit 4 OBJ)
  bool hideWideObj = false;
  bool objGroupMoves = true;  // high frame rate: a character's sprites move as one (off: each on its own; testing)
  bool maskWide = false;     // black out margins for this line
  bool skipRender = false;  // no pixels drawn (the lines' game hooks still run): a launcher preview booting out of sight
  bool subStill = false;     // high frame rate: this line's BG layers not moved on the in-between pictures (per line)
  int dimLeft = 0, dimRight = 0;  // darken the left / right margin for this line (0 none .. 256 black)
  // Margin pixels outside [wideShowLo, wideShowHi) are black on this line (a
  // screen wipe the game draws only on its 256 px, continued by a hook).
  int wideShowLo = -kMaxExt, wideShowHi = 256 + kMaxExt;
  // Sprite pixels outside [objShowLo, objShowHi) are not drawn on this line.
  int objShowLo = -kMaxExt, objShowHi = 256 + kMaxExt;
  int winShift = 0;         // windows moved right by this much on this line (a hook's overlay)
  // Per-sprite X relative to the 4:3 screen, set by the game layer for
  // sprites whose 9-bit OAM X is ambiguous in a wide view (it repeats every
  // 512 px: at 21:9, objects exist further out than that); used when set.
  int16_t objTrueX[128] = {};
  bool objTrueXSet[128] = {};
  void clearObjTrueX() {
    for (bool& b : objTrueXSet) b = false;
  }

  // Framebuffer: kLineW x kFrameH, 0x00RRGGBB, centered at kMaxExt
  uint32_t frame[kLineW * kFrameH];
  // High frame rate: in-between pictures drawn with each line, at t = k /
  // subFrames of the way from the last frame to this one (k = 1 .. subFrames
  // - 1; 1 = none): the layers' scroll and the sprites part of the way back
  // to where they were (see renderSubLine).
  // With subTimed, in-between k is drawn at t = subAt[k - 1] instead (a display
  // whose refreshes don't divide the frame evenly: the refreshes' own moments).
  static constexpr int kMaxSubFrames = 5;
  int subFrames = 1;
  bool subTimed = false;
  float subAt[kMaxSubFrames - 1] = {};
  uint32_t subFrame[kMaxSubFrames - 1][kLineW * kFrameH];
  void objMove(int idx, int* dx, int* dy) const { *dx = objDx_[idx & 127], *dy = objDy_[idx & 127]; }  // (debugging)
  void lineScroll(int prev, int l, int row, int* h, int* v) const {  // (debugging: a line's scroll, this frame or the last)
    const int c = prev ? scrollCur_ ^ 1 : scrollCur_;
    *h = lineScroll_[c][l & 3][row % kFrameH][0], *v = lineScroll_[c][l & 3][row % kFrameH][1];
  }
  int frameHeight() const { return overscan_ ? 239 : 224; }

  // Memory (public for debugging and game hooks)
  uint16_t vram[0x8000];
  uint16_t cgram[256];
  uint8_t oam[544];

  // Selected registers exposed for hooks
  uint8_t bgMode() const { return mode_; }
  uint16_t bgHofs(int l) const { return bg_[l].hofs; }
  uint16_t bgVofs(int l) const { return bg_[l].vofs; }
  uint16_t bgMapBase(int l) const { return bg_[l].mapBase; }
  uint8_t bgMapSize(int l) const { return bg_[l].mapSize; }
  uint8_t mainScreen() const { return tm_; }
  uint8_t subScreen() const { return ts_; }
  uint8_t brightness() const { return brightness_; }
  uint8_t windowLeft(int w) const { return winL_[w]; }
  uint8_t windowRight(int w) const { return winR_[w]; }
  uint8_t mainScreenWindow() const { return tmw_; }
  uint16_t bgCharBase(int l) const { return bg_[l].charBase; }
  uint16_t tileEntry(int l, int tx, int ty) const { return mapEntry(l, tx, ty); }
  bool bgBigTiles(int l) const { return bg_[l].bigTiles; }
  bool bg3Priority() const { return bg3Priority_; }
  int bgBpp(int l) const;
  uint16_t objBase() const { return objBase_; }
  uint8_t objSizeSel() const { return objSize_; }
  std::string debugString() const;
  // Render a whole BG tilemap (debug). out must hold w*h pixels.
  void debugRenderMap(int l, uint32_t* out, int* w, int* h) const;

  // Serialization
  static constexpr int kStateVersion = 1;
  size_t stateSize() const;
  void saveState(uint8_t* dst) const;
  void loadState(const uint8_t* src);

  // Debug
  uint16_t vramAddr() const { return vramAdr_; }
  using VramWatch = void (*)(void* ctx, uint16_t adr, uint16_t val);
  VramWatch vramWatch = nullptr;
  void* vramWatchCtx = nullptr;

private:
  struct Bg {
    uint16_t hofs = 0, vofs = 0;
    uint16_t mapBase = 0;   // word address
    uint8_t mapSize = 0;
    uint16_t charBase = 0;  // word address
    bool bigTiles = false;
    bool mosaic = false;
  };

  // ---- registers
  bool forcedBlank_ = true;
  uint8_t brightness_ = 0;
  uint8_t objSize_ = 0;
  uint16_t objBase_ = 0, objNameSel_ = 0;
  uint16_t oamAdr_ = 0, oamAdrReload_ = 0;
  bool oamPriority_ = false;
  uint8_t oamLatch_ = 0;
  bool oamSecondWrite_ = false;
  uint8_t mode_ = 0;
  bool bg3Priority_ = false;
  uint8_t mosaicSize_ = 1;
  uint8_t mosaicStart_ = 1;
  Bg bg_[4];
  uint8_t scrollPrev_ = 0, scrollPrev2_ = 0;
  uint8_t m7Prev_ = 0;
  uint16_t vramAdr_ = 0;
  uint8_t vramInc_ = 1;
  uint8_t vramRemap_ = 0;
  bool vramIncOnHigh_ = false;
  uint16_t vramReadBuf_ = 0;
  uint8_t m7sel_ = 0;
  int16_t m7a_ = 0, m7b_ = 0, m7c_ = 0, m7d_ = 0, m7x_ = 0, m7y_ = 0, m7hofs_ = 0, m7vofs_ = 0;
  // Last byte written to any BGnHOFS. Not saved in states: games always write
  // both bytes, so it is set again before it is used.
  uint8_t hofsPrev_ = 0;
  uint8_t cgAdr_ = 0;
  bool cgSecond_ = false;
  uint8_t cgLatch_ = 0;
  uint8_t winSel_[6] = {};  // per layer (BG1-4, OBJ, COL) 4-bit window settings
  uint8_t winL_[2] = {}, winR_[2] = {};
  uint8_t winLogic_[6] = {};
  uint8_t tm_ = 0, ts_ = 0, tmw_ = 0, tsw_ = 0;
  uint8_t cgwsel_ = 0, cgadsub_ = 0;
  uint8_t fixedR_ = 0, fixedG_ = 0, fixedB_ = 0;
  uint8_t setini_ = 0;
  bool overscan_ = false;
  bool interlace_ = false;
  uint16_t hCount_ = 0, vCount_ = 0;
  bool hFlip_ = false, vFlip_ = false;
  bool countersLatched_ = false;
  uint8_t ppu1Bus_ = 0, ppu2Bus_ = 0;
  bool rangeOver_ = false, timeOver_ = false;
  bool frameOdd_ = false;

  // ---- per-line render buffers (index = x + kMaxExt)
  struct Px {
    uint16_t col;
    uint8_t pri;  // 0 = transparent, else priority+1
    uint8_t pal;  // OBJ palette (0-7) or BG palette
  };
  Px layer_[5][kLineW];
  Px layerMain_[4][kLineW];  // (high frame rate: the line's BG layers as the frame drew them, for its in-betweens)
  uint8_t winMask_[6][kLineW];  // 1 = inside effective window for layer
  uint8_t colorLut_[16][32];    // brightness scaled 5-bit -> 8-bit

  // ---- high frame rate (see subFrames): each line's scroll, this frame's
  // and the last one's; the sprites' positions last frame and their moves
  int16_t lineScroll_[2][4][kFrameH][2] = {};
  int scrollCur_ = 0;
  int16_t objPrevX_[128] = {}, objPrevY_[128] = {};
  uint16_t objPrevKey_[128] = {};
  bool objPrevOk_[128] = {}, objPrevBig_[128] = {};
  int16_t objPrev2X_[128] = {}, objPrev2Y_[128] = {};  // (the frame before: sprites flickered every other frame)
  uint16_t objPrev2Key_[128] = {};
  bool objPrev2Ok_[128] = {}, objPrev2Big_[128] = {};
  int16_t objDx_[128] = {}, objDy_[128] = {};
  bool sub_ = false;  // an in-between picture's line is being drawn
  int16_t lineExt_[2][kFrameH] = {};  // each line's left margin, this frame's and the last (the view sliding: see renderSubLine)
  bool curLit_ = false, prevLit_ = false;  // a line of this frame / the last was lit (not all black)
  int16_t subVofs_[4] = {};
  int16_t objSubDx_[128] = {}, objSubDy_[128] = {};
  void objMoves();
  void renderSubLine(int row, int k, int x0, int x1, int eL, int eR);
  void composeLine(uint32_t* fb, int x0, int x1, int eL, int eR);

  uint16_t readVramWord(uint16_t adr) const { return vram[adr & 0x7fff]; }
  uint16_t remapVramAdr(uint16_t adr) const;
  void renderBg(int l, int row, int x0, int x1);
  void renderMode7(int l, int row, int x0, int x1);
  void renderObj(int row, int x0, int x1);
  void computeWindows(int x0, int x1);
  void applyRemap(int l, int x0, int x1);
  void edgeRemap(int l, PpuLayerRemap* rm);
  uint16_t mapEntry(int l, int tx, int ty) const;
  uint16_t directColor(uint8_t idx, uint8_t pal) const;
  void buildLut();
};
