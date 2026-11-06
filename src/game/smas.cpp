#include "smas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include "game/smb1.h"
#include "game/palette.h"
#include "game/smb2.h"
#include "game/smb3.h"

namespace {
constexpr uint32_t kExtMagic = 0x58474d53;  // 'SMGX'
constexpr uint8_t kExtVersion = 7;          // (6 is read too; older blocks are ignored)
}  // namespace

SmasGame::SmasGame(Snes& snes) : snes_(snes) {}
SmasGame::~SmasGame() {
  if (snes_.stateExt == this) snes_.stateExt = nullptr;
}

void SmasGame::install() {
  snes_.ppu.hooks = this;
  smb1_ = std::make_unique<Smb1Engine>(snes_, 0x03, 0x05);
  smbll_ = std::make_unique<Smb1Engine>(snes_, 0x0d, 0x10);
  smb2_ = std::make_unique<Smb2Engine>(snes_);
  smb3_ = std::make_unique<Smb3Engine>(snes_);
  snes_.clearExecMarks();
  smb1_->markExec();
  smbll_->markExec();
  smb2_->markExec();
  smb3_->markExec();
  snes_.setExecHook(execThunk, this);
  snes_.stateExt = this;
  snes_.clearWriteHooks();
  snes_.addWriteHook(0x7e03ad, 0x7e03b3, writeThunk, this);
  snes_.addWriteHook(0x7e0800, 0x7e09ff, writeThunk, this);
  snes_.addWriteHook(0x7e0c00, 0x7e0dff, writeThunk, this);  // SMB1 OAM high bits
  snes_.addWriteHook(0x7e0a20, 0x7e0a9f, writeThunk, this);  // SMB3 OAM high bits
}

const char* SmasGame::gameName(GameId g) {
  switch (g) {
    case GameId::Boot: return "Starting";
    case GameId::Menu: return "Game Select";
    case GameId::SMB1: return "Super Mario Bros.";
    case GameId::SMBLL: return "Super Mario Bros.: The Lost Levels";
    case GameId::SMB2: return "Super Mario Bros. 2";
    case GameId::SMB3: return "Super Mario Bros. 3";
  }
  return "?";
}

std::string SmasGame::describe() const {
  char buf[160];
  snprintf(buf, sizeof buf, "%s gameplay=%d ext=%d+%d (req %d)%s", gameName(game_), gameplay_, extL_, extR_,
           reqExt_, loading_ ? " loading" : "");
  std::string s = buf;
  if (game_ == GameId::SMB1 && smb1_) s += "\n  " + smb1_->debug();
  if (game_ == GameId::SMBLL && smbll_) s += "\n  " + smbll_->debug();
  if (game_ == GameId::SMB2 && smb2_) s += "\n  " + smb2_->debug();
  if (game_ == GameId::SMB3 && smb3_) s += "\n  " + smb3_->debug();
  return s;
}

void SmasGame::detectGame() {
  // $7FFF00 selects the NMI handler: identifies which title is running.
  switch (snes_.peek(0x7fff00)) {
    case 0x02: game_ = GameId::SMB1; break;
    case 0x04: game_ = GameId::SMBLL; break;
    case 0x06: game_ = GameId::SMB2; break;
    case 0x08: game_ = GameId::SMB3; break;
    default: game_ = GameId::Menu; break;
  }
}

Smb1Engine* SmasGame::engine() {
  if (game_ == GameId::SMB1) return smb1_.get();
  if (game_ == GameId::SMBLL) return smbll_.get();
  return nullptr;
}

void SmasGame::beforeFrame() {
  detectGame();
  if (game_ != lastGame_) {
    if (smb1_) smb1_->reset();
    if (smbll_) smbll_->reset();
    snes_.ppu.clearObjTrueX();  // (SMB3's sprite positions)
    lastGame_ = game_;
  }
  extL_ = extR_ = 0;
  gameplay_ = false;
  if (smb1_) smb1_->setCoop(coop && game_ == GameId::SMB1);  // (experimental: see smb1coop.cpp)
  if (smbll_) smbll_->setCoop(coop && game_ == GameId::SMBLL);
  if (smb3_) smb3_->setCoop(coop && game_ == GameId::SMB3);
  // (co-op: the CPU at twice its speed, so the second player's turn doesn't
  // slow busy scenes down; one player keeps the console's own timing)
  // (... and more for more players: each one's turn is the player's code again)
  int speed = coop && (game_ == GameId::SMB1 || game_ == GameId::SMBLL || game_ == GameId::SMB3) ? 2 : 1;
  if (Smb1Engine* e = engine(); e && speed > 1) speed = std::max(speed, std::min(e->coopPlayers(), 4));
  snes_.setCpuSpeed(speed);
  if (Smb1Engine* e = engine()) {
    e->setPads(pads, padsPresent);
    e->frameStart(reqExt_, enemyRange, hudAnchor);
    if (std::string m = e->takeCoopMessage(); !m.empty()) message_ = m;
    extL_ = e->extL();
    extR_ = e->extR();
    gameplay_ = e->gameplay();
  } else if (game_ == GameId::SMB2) {
    smb2_->dimWrap_ = dimWrap;
    smb2_->frameStart(reqExt_, enemyRange, hudAnchor);
    extL_ = smb2_->extL();
    extR_ = smb2_->extR();
    gameplay_ = smb2_->gameplay();
    std::string m = smb2_->takeMessage();
    if (!m.empty()) message_ = m;
  } else if (game_ == GameId::SMB3) {
    smb3_->dimWrap_ = dimWrap;
    smb3_->frameStart(reqExt_, enemyRange, hudAnchor);
    extL_ = smb3_->extL();
    extR_ = smb3_->extR();
    gameplay_ = smb3_->gameplay();
    std::string m = smb3_->takeMessage();
    if (!m.empty()) message_ = m;
  }
  menuDissolve_ = false;
  if (game_ == GameId::Menu && reqExt_ > 0) {
    // The game-select carousel uses a 512px wide tilemap, so its neighbours
    // fill the margins naturally. Other menu screens stay 4:3. (It appears
    // through a dissolve: BG2 is a cover over it, a 256 px pattern, whose
    // edge columns are carried on over the margins.)
    const Ppu& p = snes_.ppu;
    bool carousel = ((p.mainScreen() | p.subScreen()) & 0x01) && p.bgMapBase(0) == 0x7000 && p.bgMapSize(0) == 3;
    if (carousel) extL_ = extR_ = std::min(reqExt_, 112);
    menuDissolve_ = carousel && p.mainScreen() == 0x02;
    // The All-Stars title (BG mode 3, its picture on BG1 at $7800): wide, its
    // margins painted on after the frame (see paintTitleMargins).
    smasTitle_ = !carousel && p.bgMode() == 3 && p.bgMapBase(0) == 0x7800;
    if (smasTitle_) extL_ = extR_ = std::min(reqExt_, 112);
    // (Going on to the game select it dissolves away in triangles from the
    // screen's edges in: BG2's cover, a fixed diamond of tiles numbered by
    // the distance from the middle, $0F at the edges, their graphics drawn in
    // ring by ring. The margins black meanwhile, gone first.)
    titleDissolve_ = false;
    for (int i = 0; i < 16 && smasTitle_ && !titleDissolve_; i++)
      titleDissolve_ = p.vram[(p.bgCharBase(1) + 0x0f * 16 + i) & 0x7fff] != 0;
  } else {
    smasTitle_ = false;
  }
  // Screens drawn as one 256 px picture (SMB2's and SMB3's titles) are shown
  // centred, each layer's pattern at the screen's edges carried on over the
  // margins (see pictureRemap). (Not the All-Stars title: its cast and
  // perspective floor can't be carried on in keeping with it.)
  picture_ = select_ = card2_ = blackWide_ = throne_ = parade_ = battle_ = false;
  // (SMB3's map; still while a level is being entered from it, in NMI mode
  // $00: the map until it fades, then "MARIO START!", until the level's view)
  onMap_ = game_ == GameId::SMB3 && (smb3_->mapScreen() || (onMap_ && smb3_->mapLeaving()));
  if (reqExt_ > 0 && extL_ == 0 && extR_ == 0) {
    if (game_ == GameId::SMB2) {
      picture_ = smb2_->pictureScreen();
      throne_ = picture_ && smb2_->throneScreen();
      // (the contributors' parade: its crowd a strip 256 px wide, scrolled
      // round on BG1's one-screen map, the sign on the same layer)
      parade_ = picture_ && smb2_->endingScreen() && !throne_ && snes_.ppu.bgMapSize(0) == 0;
      smb2_->paradeStart(picture_ && smb2_->endingScreen() ? std::min(reqExt_, 112) : 0);
      if (parade_) {
        // (its background: BG1's commonest tile; rows without it are full)
        const Ppu& p = snes_.ppu;
        std::map<uint16_t, int> count;
        for (int r = 0; r < 32; r++)
          for (int c = 0; c < 32; c++) count[p.tileEntry(0, c, r)]++;
        int n = -1;
        for (auto& kv : count)
          if (kv.second > n) paradeBg_ = kv.first, n = kv.second;
        for (int r = 0; r < 32; r++) {
          paradeFull_[r] = true;
          for (int c = 0; c < 32; c++) paradeFull_[r] &= p.tileEntry(0, c, r) != paradeBg_;
        }
      }
      select_ = smb2_->selectScreen();
      card2_ = smb2_->cardScreen();
      // (only what reads as a card: see learnCard)
      if (card2_ && !cardPat_.valid) learnCard();
      card2_ = card2_ && cardPat_.card;
    } else if (game_ == GameId::SMB3) {
      picture_ = smb3_->pictureScreen() || smb3_->spadeScreen();
      // (The world map is kept exactly as it is, black either side: a frame
      // drawn around one screen, which nothing carries on. The picture keeps
      // the wide shape, as the levels it leads to.)
      blackWide_ = onMap_;
      battle_ = smb3_->battleScreen();
    }
    if (picture_ || select_ || card2_ || blackWide_ || battle_) extL_ = extR_ = std::min(reqExt_, 112);
  }
  // A picture wiped in over another (the warp card to the character select:
  // BG mode 2's offset-per-tile, the offsets in BG3's map, every column $100
  // for the card (it is in the map's other half), turned to 0 a column at a
  // time): half one and half the other, the margins carried on stripes of
  // both. Black margins while it runs (columns offset differently, and a
  // couple of frames after).
  if (game_ == GameId::SMB2 && (picture_ || select_)) {
    const Ppu& p = snes_.ppu;
    bool slid = false;
    if (p.bgMode() == 2)
      for (int r = 0; r < 2 && !slid; r++) {
        // (the offset BG1 gets, modulo its 512 px map: after a death the
        // select's right half keeps $200, the same picture)
        auto at = [&](int c) {
          uint16_t e = p.tileEntry(2, (p.bgHofs(2) >> 3) + c, (p.bgVofs(2) >> 3) + r);
          return (e & 0x2000) ? (e & 0x1ff) : -1;
        };
        for (int c = 1; c < 31 && !slid; c++) slid = at(c) != at(0);  // (31 offsets none shown: the card's 0)
      }
    if (slid) wipeFrames_ = 3;
    else if (wipeFrames_ > 0) wipeFrames_--;
  } else {
    wipeFrames_ = 0;
  }
  // (The character select's frame is taken as it appears: as it wipes out,
  // the card is already in those columns of the map; and once wiped in.)
  if (select_ && wipeFrames_ > 0) latticeValid_ = false;
  if (select_ && !latticeValid_ && wipeFrames_ == 0) {
    const Ppu& p = snes_.ppu;
    for (int ty = 0; ty < 32; ty++) {
      latticeL_[ty] = p.tileEntry(0, (p.bgHofs(0) >> 3) + 1, (p.bgVofs(0) >> 3) + ty);
      latticeR_[ty] = p.tileEntry(0, (p.bgHofs(0) >> 3) + 30, (p.bgVofs(0) >> 3) + ty);
    }
  }
  latticeValid_ = select_ && wipeFrames_ == 0;
  if (game_ != GameId::SMB2 || !smb2_->cardScreen()) cardPat_.valid = false;
  snes_.ppu.setExt(extL_, extR_);
  applyCheats();
  updateWarp();
}

void SmasGame::afterFrame() {
  int pick = -1;
  if (Smb1Engine* e = engine()) {
    e->titleFrame();
    pick = e->takeTitlePick();
  } else if (game_ == GameId::SMB3 && smb3_) {
    smb3_->titleFrame();
    pick = smb3_->takeTitlePick();
  }
  // (A title screen's co-op choice; not a level select's way to its level:
  // it starts the game from the title's 1 PLAYER GAME, and that turned off
  // the co-op the level select had just chosen.)
  if (pick >= 0 && !loading_) {
    coop = pick != 0;
    coopPick = pick;
  }
  if (Smb1Engine* e = engine()) ach_.setCoop(e->coopLuigiLive(), e->coopLuigiPoles(), e->coopP2Hurts());
  else ach_.setCoop(false, 0, 0);
  ach_.frame(snes_.wram, game_);
  // (and the in-between pictures of a high frame rate: see Ppu::subFrames)
  Ppu& p = snes_.ppu;
  if (!smasTitle_ || !titleDissolve_) titleEdgesGone_ = false;
  for (int k = 0; k <= p.subFrames - 1 && k < Ppu::kMaxSubFrames; k++) {
    uint32_t* frame = k ? p.subFrame[k - 1] : p.frame;
    if (smasTitle_ && titleDissolve_) titleDissolveMargins(frame);
    else if (smasTitle_) paintTitleMargins(frame, k == 0);
  }
}

// The All-Stars title is one 256 px picture: the sky, a pale band, then a
// checkerboard floor in perspective, the cast standing on it (clear of the
// screen's edge columns). Its margins are painted into the finished frame
// (so they fade with it), row by row: the floor's checkers carried on, their
// width and colours read off the tiles at both screen edges (the floor is
// symmetric about the middle, the same tiles' edges at 128 +/- d; hand-drawn,
// the widths vary a pixel or so, so no span repeats exactly); other rows go
// on in their edge colour (the sky, the band).
void SmasGame::paintTitleMargins(uint32_t* frame, bool keep) {
  Ppu& p = snes_.ppu;
  const int L = extL_, R = extR_;
  if (L <= 0 && R <= 0) return;
  const int H = p.frameHeight();
  struct Row {
    uint32_t a, b, aR, bR;
    bool floor;
    int e0;
    double w;
  };
  std::vector<Row> rows(H);
  for (int y = 0; y < H; y++) {
    const uint32_t* row = frame + y * kLineW + kMaxExt;  // (row[0] is the picture's first pixel)
    Row& r = rows[y];
    // The checkers' colours: each edge's, and the tile beside it.
    r.a = row[0];
    r.aR = row[255];
    int b0 = 1;
    while (b0 < 128 && row[b0] == r.a) b0++;
    r.b = b0 < 128 ? row[b0] : r.a;
    int b0R = 254;
    while (b0R > 128 && row[b0R] == r.aR) b0R--;
    r.bR = b0R > 128 ? row[b0R] : r.aR;
    // Tile edges near both screen edges, in the left half's terms (the right
    // half's at 256 - x), up to where something else stands on the floor.
    std::vector<int> edges;
    auto collect = [&](bool right) {
      uint32_t c0 = right ? r.aR : r.a, c1 = right ? r.bR : r.b;
      uint32_t prev = c0;
      for (int k = 1; k < 128; k++) {
        uint32_t c = row[right ? 255 - k : k];
        if (c != c0 && c != c1) break;
        if (c != prev) edges.push_back(k);  // (pixel k starts a tile)
        prev = c;
      }
    };
    collect(false);
    collect(true);
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
    // A floor row: the same two colours at both edges, the first tile's edge
    // as far in on both sides (the floor is symmetric; Bowser and Birdo, the
    // logo and Lakitu beside the sky and the band are not).
    const int k1L = b0, k1R = 255 - b0R;
    r.floor = r.a == r.aR && r.b == r.bR && r.a != r.b && std::abs(k1L - k1R) <= 1 && k1L < 64;
    r.e0 = std::min(k1L, k1R);
    r.w = 0;
    if (r.floor && edges.size() >= 2) {
      std::vector<int> d;
      for (size_t i = 1; i < edges.size(); i++) d.push_back(edges[i] - edges[i - 1]);
      std::sort(d.begin(), d.end());
      int med = d[d.size() / 2];
      int span = edges.back() - edges.front(), n = (int)std::lround((double)span / std::max(1, med));
      // (tiles of a like width, 2 px or more: not a character's outline,
      // Bowser's and Birdo's as far in by chance on a row of the band)
      if (d.front() >= 2 && d.back() <= 2 * d.front() + 3 && n >= 1) r.w = (double)span / n;
    }
  }
  // (A floor row with too few clear tile edges, by the horizon where the
  // tiles are a few pixels wide: the nearest measured row's width.)
  for (int y = 0; y < H; y++) {
    if (!rows[y].floor || rows[y].w > 0) continue;
    for (int d = 1; d <= 16; d++) {
      if (y - d >= 0 && rows[y - d].floor && rows[y - d].w > 0) { rows[y].w = rows[y - d].w; break; }
      if (y + d < H && rows[y + d].floor && rows[y + d].w > 0) { rows[y].w = rows[y + d].w; break; }
    }
  }
  if (keep) titleCache_.assign((size_t)H * 2 * kTitleCacheW, 0);
  for (int y = 0; y < H; y++) {
    uint32_t* row = frame + y * kLineW + kMaxExt;
    const Row& r = rows[y];
    if (r.w > 0) {
      // x < 0: tile j = floor((x - e0) / w) from the first edge e0; j = -1 is
      // the edge's own tile. (The right: the same, mirrored, pixel x the twin
      // of 255 - x.)
      for (int x = -L; x < 0; x++) row[x] = ((long)std::floor((x - r.e0) / r.w) & 1) ? r.a : r.b;
      for (int x = 256; x < 256 + R; x++) row[x] = ((long)std::floor(((255 - x) - r.e0) / r.w) & 1) ? r.aR : r.bR;
    } else {
      for (int x = -L; x < 0; x++) row[x] = r.a;
      for (int x = 256; x < 256 + R; x++) row[x] = r.aR;
    }
    // (kept for the dissolve: see titleDissolveMargins)
    if (!keep) continue;
    uint32_t* c = &titleCache_[(size_t)y * 2 * kTitleCacheW];
    for (int k = 0; k < kTitleCacheW && k < L; k++) c[k] = row[-1 - k];
    for (int k = 0; k < kTitleCacheW && k < R; k++) c[kTitleCacheW + k] = row[256 + k];
  }
}

// The title dissolving into the game select: BG2's cover, a diamond of
// tiles numbered by the distance from the middle (the edge columns' all the
// outermost, $0F), drawn in ring by ring as black triangles. The margins go
// with the outermost ring: the last whole frame's margins (see
// paintTitleMargins), covered where the edge column's tile is, repeated
// across them (each side its own column's tile, with its flips).
void SmasGame::titleDissolveMargins(uint32_t* frame) {
  Ppu& p = snes_.ppu;
  const int L = extL_, R = extR_, H = p.frameHeight();
  const bool cached = titleCache_.size() == (size_t)H * 2 * kTitleCacheW;
  // (Once the edge columns are all covered, black, the margins stay black to
  // the end: on its last frame the select's graphics are already loaded over
  // the cover's tiles, and the title showed through the holes.)
  if (frame == p.frame && !titleEdgesGone_) {
    bool gone = true;
    for (int y = 0; y < H && gone; y++) {
      const uint32_t* row = frame + y * kLineW + kMaxExt;
      gone = row[0] == 0 && row[255] == 0;
    }
    titleEdgesGone_ = gone;
  }
  if (titleEdgesGone_) {
    for (int y = 0; y < H; y++) {
      uint32_t* row = frame + y * kLineW + kMaxExt;
      for (int x = -L; x < 0; x++) row[x] = 0;
      for (int x = 256; x < 256 + R; x++) row[x] = 0;
    }
    return;
  }
  auto covered = [&](int col, int y, int x) {
    uint16_t e = p.tileEntry(1, col, (p.bgVofs(1) + y) >> 3);
    int px = x & 7, py = (p.bgVofs(1) + y) & 7;
    if (e & 0x4000) px = 7 - px;  // (H flip)
    if (e & 0x8000) py = 7 - py;  // (V flip)
    uint16_t base = (uint16_t)(p.bgCharBase(1) + (e & 0x3ff) * 16);  // (4 bpp: 16 words a tile)
    uint16_t w0 = p.vram[(base + py) & 0x7fff], w1 = p.vram[(base + 8 + py) & 0x7fff];
    int bit = 7 - px;
    return (((w0 >> bit) | (w0 >> (8 + bit)) | (w1 >> bit) | (w1 >> (8 + bit))) & 1) != 0;
  };
  for (int y = 0; y < H; y++) {
    uint32_t* row = frame + y * kLineW + kMaxExt;
    const uint32_t* c = cached ? &titleCache_[(size_t)y * 2 * kTitleCacheW] : nullptr;
    for (int x = -L; x < 0; x++)
      row[x] = !c || -1 - x >= kTitleCacheW || covered(0, y, x) ? 0 : c[-1 - x];
    for (int x = 256; x < 256 + R; x++)
      row[x] = !c || x - 256 >= kTitleCacheW || covered(31, y, x) ? 0 : c[kTitleCacheW + x - 256];
  }
}

// Character colour variants: recolour the sprite palette the player uses.
void SmasGame::applyPlayerPalette(Ppu& ppu) {
  ppu.objPalOverrideMask = 0;
  Smb1Engine* e = engine();
  if (e) e->setCharacterColours(playerPalette, luigiPalette);  // (co-op's other brother: see coopLine)
  // (the game's player: Mario's or Luigi's, whichever he is)
  const int variant = e ? e->liveVariant() : playerPalette;
  if (variant <= 0 || !gameplay_) return;
  int pal = -1;
  if (e) {
    // SMB1 engine: player OAM slot offset at $0B45 (OAM buffer at $0800)
    uint8_t off = snes_.wram[0x0b45];
    pal = (snes_.wram[0x0800 + off + 3] >> 1) & 7;
  }
  if (pal < 0) return;
  recolorCharacterPalette(&ppu.cgram[128 + pal * 16], ppu.objPalOverride[pal], variant);
  ppu.objPalOverrideMask |= 1 << pal;
}

void SmasGame::onStateLoaded() {
  detectGame();
  lastGame_ = game_;
  cancelWarps();
  snes_.ppu.clearObjTrueX();  // (SMB3's sprite positions: restored below if saved)
  if (smb1_) smb1_->reset();
  if (smbll_) smbll_->reset();
  // (co-op as it will be in this game before its second player is read back:
  // turned on at the next frame instead, it was wiped, Luigi back as Mario)
  if (smb1_) smb1_->setCoop(coop && game_ == GameId::SMB1);
  if (smbll_) smbll_->setCoop(coop && game_ == GameId::SMBLL);
  if (smb3_) smb3_->setCoop(coop && game_ == GameId::SMB3);
  if (smb3_) smb3_->forgetRoom();
  if (Smb1Engine* e = engine()) e->seedColumns();
  // Restore the widescreen state saved with the state. Without it (states
  // from older versions) an engine takes the state as saved at 4:3, so the
  // enemies a wider view shows are caught up rather than skipped.
  std::vector<uint8_t> x;
  x.swap(pendingExt_);
  bool loaded[2] = {};
  bool achLoaded = false, msuLoaded = false;
  [&] {
    const uint8_t* p = x.data();
    const uint8_t* end = p + x.size();
    uint32_t magic = 0;
    if (x.size() < 5) return;
    std::memcpy(&magic, p, 4);
    int version = p[4];
    if (magic != kExtMagic || version < 6 || version > kExtVersion) return;
    p += 5;
    // (SMB2, SMB3, then SMB1 and the Lost Levels: later additions, absent
    // from older states.)
    // (... and the achievement run: absent from older states, which then
    // don't count.)
    for (int g = 0; g < 6; g++) {
      uint32_t n = 0;
      if (end - p < 4) return;
      std::memcpy(&n, p, 4);
      p += 4;
      if ((size_t)(end - p) < n) return;
      if (g < 2) loaded[g] = g == 0 ? smb2_->loadExt(p, n) : smb3_->loadExt(p, n, version);
      else if (g == 4) achLoaded = (ach_.load(p, n), true);
      else if (g == 5) {  // (MSU-1's track and place: a later addition)
        if (snes_.msu) snes_.msu->loadState(p, n), msuLoaded = true;
      } else if (Smb1Engine* e = g == 2 ? smb1_.get() : smbll_.get()) e->loadExt(p, n, e == engine());
      p += n;
    }
  }();
  if (!achLoaded) ach_.load(nullptr, 0);
  if (!msuLoaded && snes_.msu) snes_.msu->loadState(nullptr, 0);  // (a state from before: its own music, not the pack's)
  if (!loaded[0]) smb2_->assumeNarrowState();
  if (!loaded[1]) smb3_->assumeNarrowState();
}

std::vector<uint8_t> SmasGame::saveExt() const {
  std::vector<uint8_t> out(5);
  std::memcpy(out.data(), &kExtMagic, 4);
  out[4] = kExtVersion;
  for (int g = 0; g < 6; g++) {
    std::vector<uint8_t> b;
    if (g == 0) smb2_->saveExt(b);
    else if (g == 1) smb3_->saveExt(b);
    else if (g == 4) b = ach_.save();
    else if (g == 5) {
      if (snes_.msu) b = snes_.msu->saveState();
    }
    else if (Smb1Engine* e = g == 2 ? smb1_.get() : smbll_.get()) e->saveExt(b);
    uint32_t n = (uint32_t)b.size();
    const uint8_t* np = (const uint8_t*)&n;
    out.insert(out.end(), np, np + 4);
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

void SmasGame::loadExt(const uint8_t* data, size_t size) {
  pendingExt_.assign(data, data + (data ? size : 0));
}

// Drop any level-select launch in progress and per-game view state: after a
// state load or a console reset they would act on the wrong game state.
void SmasGame::setNesBrickBounce(bool on) {
  // (unheadered ROM offsets: a routine in free space at $00:FFA0 - fall speed
  // 1, the bump sound - called in place of each game's "LDA #$FE / STA $A0")
  static const uint8_t kRoutine[8] = {0xA9, 0x01, 0x85, 0xA0, 0x8D, 0x00, 0x16, 0x6B};  // LDA #1, STA $A0, STA $1600, RTL
  static const uint8_t kCall[4] = {0x22, 0xA0, 0xFF, 0x00};                             // JSL $00FFA0
  static const uint8_t kOrig[4] = {0xA9, 0xFE, 0x85, 0xA0};                             // LDA #$FE, STA $A0
  static const size_t kSites[2] = {0x01BFB6, 0x06BDA4};                                 // $03:BFB6, $0D:BDA4
  std::vector<uint8_t>& rom = snes_.rom;
  if (rom.size() < 0x80000) return;
  // (only over the bytes it expects: another version of the ROM, or a patch there, is left alone)
  for (size_t at : kSites)
    if (std::memcmp(&rom[at], kOrig, 4) != 0 && std::memcmp(&rom[at], kCall, 4) != 0) return;
  for (size_t i = 0; i < 8; i++)
    if (rom[0x7FA0 + i] != 0xFF && rom[0x7FA0 + i] != kRoutine[i]) return;
  for (size_t i = 0; i < 8; i++) rom[0x7FA0 + i] = on ? kRoutine[i] : 0xFF;
  for (size_t at : kSites) std::memcpy(&rom[at], on ? kCall : kOrig, 4);
}

void SmasGame::cancelWarps() {
  warp_ = Warp();
  warpOverride_ = false;
  loading_ = false;
  if (smb1_) smb1_->cancelWarp();
  if (smbll_) smbll_->cancelWarp();
  if (smb2_) smb2_->reset();
  if (smb3_) smb3_->reset();
}

std::string SmasGame::verifyWide(int layer) {
  if (game_ == GameId::SMB2) return smb2_->verify(layer);
  if (game_ == GameId::SMB3) return layer == 0 ? smb3_->verifyLines() : "layer " + std::to_string(layer + 1) + ": n/a";
  return "verify is only implemented for SMB2 and SMB3";
}

void SmasGame::setWidescreen(int ext) { reqExt_ = std::clamp(ext, 0, kMaxSupportedExt); }

bool SmasGame::coopMidTurn() const {
  if (game_ == GameId::SMB1 && smb1_) return smb1_->coopMidTurn();
  if (game_ == GameId::SMBLL && smbll_) return smbll_->coopMidTurn();
  if (game_ == GameId::SMB3 && smb3_) return smb3_->coopMidTurn();
  return false;
}

bool SmasGame::execThunk(void* ctx, uint32_t pc) {
  SmasGame* g = (SmasGame*)ctx;
  if (Smb1Engine* e = g->engine()) return e->onExec(pc);
  if (g->game_ == GameId::SMB3) return g->smb3_->onExec(pc);
  if (g->game_ == GameId::SMB2) return g->smb2_->onExec(pc);
  return false;
}

void SmasGame::writeThunk(void* ctx, uint32_t adr, uint8_t v, uint32_t pc) {
  SmasGame* g = (SmasGame*)ctx;
  if (Smb1Engine* e = g->engine()) e->onWrite(adr, v, pc);
  else if (g->game_ == GameId::SMB3) g->smb3_->onWrite(adr, v);
}

// Cheat ids: bits 0-7 SMB1, 8-15 Lost Levels, 16-23 SMB2, 24-31 SMB3.
const std::vector<CheatDef>& SmasGame::cheatList() {
  static const std::vector<CheatDef> list = [] {
    std::vector<CheatDef> l;
    for (int g = 0; g < 2; g++) {
      GameId id = g == 0 ? GameId::SMB1 : GameId::SMBLL;
      int b = g * 8;
      l.push_back({b + 0, id, "Infinite Lives", "Lives never drop below 10."});
      l.push_back({b + 1, id, "Star Power", "Permanent starman invincibility: touch enemies to defeat them."});
      l.push_back({b + 2, id, "Invincibility", "Enemies cannot hurt you, without the star's music or touch (pits and lava still can)."});
      l.push_back({b + 3, id, "Always Fire Mario", "Keep the fire flower power-up at all times."});
      l.push_back({b + 4, id, "Infinite Time", "The level timer stays at 400."});
      l.push_back({b + 5, id, "Moon Jump", "Hold the jump button to float slowly back down."});
    }
    GameId s2 = GameId::SMB2;
    l.push_back({16, s2, "Infinite Lives", "Extra lives never drop below 9."});
    l.push_back({17, s2, "Full Health", "Your life meter is always full."});
    l.push_back({18, s2, "Star Power", "Permanent invincibility: touching enemies defeats them."});
    l.push_back({19, s2, "Four Hearts", "Start every area with the maximum four-heart life meter."});
    l.push_back({20, s2, "Super Jump", "Every character jumps as high as a charged jump."});
    l.push_back({21, s2, "Float Like Peach", "Any character can hover by holding the jump button."});
    GameId s3 = GameId::SMB3;
    l.push_back({24, s3, "Infinite Lives", "Lives never drop below 10."});
    l.push_back({25, s3, "Star Power", "Permanent starman invincibility inside levels."});
    l.push_back({26, s3, "Infinite Flight", "With the raccoon or tanooki suit you can fly for as long as you like."});
    l.push_back({27, s3, "Full P-Meter", "The P-meter is always charged."});
    l.push_back({28, s3, "Infinite Time", "The level timer stays at 300."});
    l.push_back({29, s3, "Always Raccoon", "Enter every level with at least the raccoon suit."});
    l.push_back({30, s3, "Stocked Item Box", "Empty item slots on the world map are refilled with power-ups."});
    l.push_back({31, s3, "Invincibility", "Enemies cannot hurt you, without the star's music or touch (pits and lava still can)."});
    return l;
  }();
  return list;
}

void SmasGame::applyCheats() {
  if (Smb1Engine* e = engine()) {
    int shift = game_ == GameId::SMB1 ? 0 : 8;
    e->applyCheats((uint32_t)((cheats_ >> shift) & 0xff));
  } else if (game_ == GameId::SMB2) {
    smb2_->applyCheats((uint32_t)((cheats_ >> 16) & 0xff));
  } else if (game_ == GameId::SMB3) {
    smb3_->applyCheats((uint32_t)((cheats_ >> 24) & 0xff));
  }
}

const std::vector<LevelSelectDef>& SmasGame::levelSelectList() {
  static const std::vector<LevelSelectDef> list = [] {
    std::vector<LevelSelectDef> l;
    auto smb1Style = [](GameId g, const std::vector<std::string>& worldNames) {
      LevelSelectDef d;
      d.game = g;
      for (auto& w : worldNames) {
        d.worlds.push_back("World " + w);
        std::vector<std::string> lv;
        for (int i = 1; i <= 4; i++) lv.push_back(w + "-" + std::to_string(i));
        d.levels.push_back(lv);
      }
      return d;
    };
    l.push_back(smb1Style(GameId::SMB1, {"1", "2", "3", "4", "5", "6", "7", "8"}));
    l.push_back(smb1Style(GameId::SMBLL, {"1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D"}));
    // SMB2: seven worlds of three levels (world 7 has two).
    LevelSelectDef d2;
    d2.game = GameId::SMB2;
    for (int w = 0; w < 7; w++) {
      d2.worlds.push_back("World " + std::to_string(w + 1));
      std::vector<std::string> lv;
      for (int i = 1; i <= (w == 6 ? 2 : 3); i++) lv.push_back(std::to_string(w + 1) + "-" + std::to_string(i));
      d2.levels.push_back(lv);
    }
    l.push_back(d2);
    // SMB3: numbered levels and fortresses found on each world map.
    LevelSelectDef d3;
    d3.game = GameId::SMB3;
    static const int kLevels[8] = {6, 5, 9, 6, 9, 10, 9, 2};
    // (World 5's second fortress is on the sky map, which isn't loaded at the
    // start: it's among the extra levels.)
    static const int kForts[8] = {1, 1, 2, 2, 1, 3, 2, 1};  // (World 5's second: see Smb3Engine::kExtraLevels)
    for (int w = 0; w < 8; w++) {
      d3.worlds.push_back("World " + std::to_string(w + 1));
      std::vector<std::string> names{"World Map"};
      std::vector<int> codes{0};
      for (int i = 1; i <= kLevels[w]; i++) {
        names.push_back(std::to_string(w + 1) + "-" + std::to_string(i));
        codes.push_back(i);
      }
      for (int k = 0; k < kForts[w]; k++) {
        names.push_back(kForts[w] > 1 || w == 4 ? "Fortress " + std::to_string(k + 1) : std::string("Fortress"));
        codes.push_back(11 + k);
      }
      for (int i = 0; i < Smb3Engine::kExtraCount; i++) {
        const auto& e = Smb3Engine::kExtraLevels[i];
        if (e.world != w || std::string(e.name) == "Bowser's Castle") continue;
        names.push_back(e.name);
        codes.push_back(Smb3Engine::kWarpExtra + i);
      }
      names.push_back("Airship");
      codes.push_back(Smb3Engine::kWarpAirship);
      for (int i = 0; i < Smb3Engine::kExtraCount; i++)  // (the last: after the airship)
        if (Smb3Engine::kExtraLevels[i].world == w && std::string(Smb3Engine::kExtraLevels[i].name) == "Bowser's Castle") {
          names.push_back(Smb3Engine::kExtraLevels[i].name);
          codes.push_back(Smb3Engine::kWarpExtra + i);
        }
      d3.levels.push_back(names);
      d3.codes.push_back(codes);
    }
    l.push_back(d3);
    return l;
  }();
  return list;
}

namespace {
int carouselIndex(GameId g) {
  switch (g) {
    case GameId::SMB1: return 0;
    case GameId::SMBLL: return 1;
    case GameId::SMB2: return 2;
    case GameId::SMB3: return 3;
    default: return -1;
  }
}
}  // namespace

bool SmasGame::requestLevel(GameId g, int world, int level, std::string* err, int character) {
  if (carouselIndex(g) < 0) {
    if (err) *err = "Unknown game";
    return false;
  }
  // Translate the menu's level index to the engine's level code.
  for (const auto& d : levelSelectList()) {
    if (d.game == g && world >= 0 && world < (int)d.codes.size() && level >= 0 &&
        level < (int)d.codes[world].size())
      level = d.codes[world][level];
  }
  warp_ = Warp();
  warp_.game = g;
  warp_.world = world;
  warp_.level = level;
  warp_.character = character;
  loading_ = true;
  loadFrames_ = loadIdle_ = 0;
  if (g == game_) {
    // SMB3 can only warp from its title screen: relaunch it otherwise.
    bool needTitle = (g == GameId::SMB3 && !smb3_->atTitle()) ||
                     (g == GameId::SMB2 && !smb2_->atTitle() && !smb2_->inGame());
    if (!needTitle) {
      if (!startTitleWarp()) {
        loading_ = false;
        if (err) *err = "Level select is not available for this game";
        return false;
      }
      return true;
    }
    warp_.forceReset = true;
  }
  // Launch the title from the All-Stars menu first (soft-resetting if another
  // game is running), then warp.
  warp_.pending = true;
  return true;
}

// A game started from the launcher: the All-Stars menu driven to it (reset
// first, whatever is running), as far as its file panel, where the player
// picks the save file; or (startFile, the launcher's previews) on through it
// with the highlighted file, into the game's title.
bool SmasGame::requestTitle(GameId g, bool startFile) {
  if (carouselIndex(g) < 0) return false;
  cancelWarps();
  warp_.game = g;
  warp_.titleOnly = true;
  warp_.startFile = startFile;
  warp_.forceReset = true;
  warp_.pending = true;
  loading_ = true;
  loadFrames_ = loadIdle_ = 0;
  return true;
}

// The level select's run to the level is over once the chosen game is being
// played (the level's own opening is shown), or a while after the warps
// have let go without that (an SMB3 world map; a warp that gave up).
void SmasGame::updateLoading() {
  if (!loading_) return;
  if (warp_.titleOnly) {  // (see requestTitle)
    loading_ = warp_.pending && ++loadFrames_ <= 5400;
    return;
  }
  Smb1Engine* e = engine();
  bool busy = warp_.pending || (e && e->warpPending()) || (game_ == GameId::SMB2 && smb2_->overridesInput()) ||
              (game_ == GameId::SMB3 && smb3_->overridesInput());
  loadIdle_ = busy ? 0 : loadIdle_ + 1;
  // (Not SMB1's title; nor the frame an SMB1 warp is applied on, still the
  // old level's.)
  bool playing = game_ == warp_.game && gameplay_ && (!e || snes_.wram[0x0770] == 1);
  if (++loadFrames_ > 5400 || (loadIdle_ > 1 && playing) || loadIdle_ > 300) loading_ = false;
}

std::string SmasGame::takeMessage() {
  std::string m;
  m.swap(message_);
  return m;
}

bool SmasGame::startTitleWarp() {
  if (warp_.game == GameId::SMB3) {
    smb3_->requestWarp(warp_.world, warp_.level);
    return true;
  }
  if (warp_.game == GameId::SMB2) {
    smb2_->requestWarp(warp_.world, warp_.level, warp_.character);
    return true;
  }
  Smb1Engine* e = warp_.game == GameId::SMB1 ? smb1_.get() : warp_.game == GameId::SMBLL ? smbll_.get() : nullptr;
  if (e && e->ok() && warp_.world < e->worldCount()) {
    e->requestWarp(warp_.world, warp_.level);
    return true;
  }
  return false;
}

// Drives the All-Stars menu like a player would. Screens are recognised from
// the PPU setup: the title shows only BG2; the game carousel uses a 64x64 BG1
// map at $7000. Start is only pressed on the carousel once its cursor ($0218)
// is on the target, so the file panel always opens for the right game.
void SmasGame::driveLaunch() {
  if (game_ == GameId::Menu) warp_.sawMenu = true;
  if (game_ == warp_.game && (!warp_.forceReset || warp_.sawMenu)) {
    warp_.pending = false;
    if (!warp_.titleOnly && !startTitleWarp()) message_ = "That level can't be started in this version of the game.";
    return;
  }
  warpOverride_ = true;
  warpInput_ = 0;
  if (++warp_.attempts > 3600) {  // ~60 s safety net
    warp_.pending = false;
    message_ = "Level select couldn't reach the game. Try again from the game select screen.";
    return;
  }
  const Ppu& p = snes_.ppu;
  bool lit = !p.forcedBlank() && p.brightness() == 15;
  bool title = game_ == GameId::Menu && lit && p.mainScreen() == 0x02;
  bool carousel = game_ == GameId::Menu && lit && p.mainScreen() == 0x01 && p.bgMapBase(0) == 0x7000 &&
                  p.bgMapSize(0) == 3;
  int target = carouselIndex(warp_.game);
  int cur = snes_.wram[0x0218] & 3;
  int t = warp_.timer++;
  auto next = [&](int s) {
    warp_.stage = s;
    warp_.timer = 0;
    warp_.pressT = -1000;
  };
  switch (warp_.stage) {
    case 0:
      if (game_ != GameId::Menu) snes_.reset(false);
      next(1);
      break;
    case 1:  // wait for the title or the carousel to settle
      if (carousel) {
        if (t > 30) next(2);
      } else if (title) {
        if (t > 20) {
          warpInput_ = kPadStart;
          if (t > 24) next(1);
        }
      } else {
        warp_.timer = 0;
      }
      break;
    case 2:  // move the cursor
      if (!carousel) {
        next(1);
      } else if (cur != target) {
        int diff = (target - cur + 4) % 4;
        if (t < 4) warpInput_ = diff > 2 ? kPadLeft : kPadRight;
        if (t > 40) next(2);
      } else if (t > 20) {
        next(3);
      }
      break;
    case 3:  // open the file panel, then start with the highlighted file
      if (warp_.titleOnly) {
        // (Start is ignored while the carousel turns: pressed, one frame
        // long, once it is still. The box then zooms into the file panel,
        // some 80 frames; a press meanwhile picked a file. Then the player
        // picks the file; for the launcher's previews, on with the
        // highlighted one, pressed again until the game is running.)
        int hofs = p.bgHofs(0);
        warp_.still = hofs == warp_.lastHofs ? warp_.still + 1 : 0;
        warp_.lastHofs = hofs;
        if (warp_.pressT < -500 && warp_.still > 20) {
          warp_.pressT = t;
          warpInput_ = kPadStart;
        } else if (warp_.pressT >= 0) {
          int since = t - warp_.pressT;
          if (!warp_.startFile && since > 120) {
            warp_.pending = false;
            warpOverride_ = false;
          } else if (warp_.startFile && since >= 130 && (since - 130) % 150 == 0) {
            warpInput_ = kPadStart;
          }
        }
        if (t > 1500) next(1);
        break;
      }
      if (t < 4 || (t >= 70 && t < 74)) warpInput_ = kPadStart;
      if (t > 300) next(3);
      break;
  }
}
void SmasGame::updateWarp() {
  warpOverride_ = false;
  if (warp_.pending) driveLaunch();
  updateLoading();
  if (Smb1Engine* e = engine()) {
    if (e->overridesInput()) {
      snes_.input[0] = e->inputOverride();
      return;
    }
  }
  if (game_ == GameId::SMB2 && smb2_->overridesInput()) {
    snes_.input[0] = smb2_->inputOverride();
    return;
  }
  if (game_ == GameId::SMB3 && smb3_->overridesInput()) {
    snes_.input[0] = smb3_->inputOverride();
    return;
  }
  if (warpOverride_) snes_.input[0] = warpInput_;
}

const std::vector<TrackDef>& SmasGame::trackList() {
  static const std::vector<TrackDef> list;
  return list;
}

void SmasGame::beginLine(Ppu& ppu, int row) {
  if (row == 0) applyPlayerPalette(ppu);
  if (Smb1Engine* e = engine()) {
    e->coopLine(ppu, row);
    e->beginLine(ppu, row);
  } else if (game_ == GameId::SMB3) smb3_->beginLine(ppu, row);
  else if (game_ == GameId::SMB2) smb2_->beginLine(ppu, row);
  if (picture_ && !throne_) pictureRemap(ppu, row);
  if (row == 0 && game_ == GameId::SMB2 && (parade_ || wasParade_)) {
    smb2_->paradeSprites(ppu, parade_ ? extR_ : 0);
    wasParade_ = parade_;
  }
  if (blackWide_ || wipeFrames_ > 0) ppu.maskWide = true;
  // (The arena round: no one can be there, dimmed as SMB2's wrapping areas.
  // Not the round's panel, the map's other half, between rounds.)
  if (battle_ && dimWrap && !(ppu.bgHofs(0) & 0x100)) ppu.dimLeft = ppu.dimRight = 96;
  // (The panel's left 8 px are covered by a column of black sprites (tile
  // $9F), as the NES game hid its screen's left edge: with its gold carried
  // on there, they cut a black strip down it.)
  if (battle_ && extL_ > 0 && (ppu.bgHofs(0) & 0x100)) ppu.objShowLo = std::max(ppu.objShowLo, 8);
  if (select_ || card2_) ppu.wideFetchAll[0] = true;  // (their blank edge columns too)
  if (menuDissolve_ && (ppu.bgHofs(1) & 7) == 0) {
    // (the cover's first and last tile columns, repeated tile-aligned)
    PpuLayerRemap& rm = ppu.layerRemap[1];
    rm.count = 3;
    rm.seg[0] = {(int16_t)-((extL_ + 7) & ~7), 0, 0, 8};
    rm.seg[1] = {0, 256, 0, 0};
    rm.seg[2] = {256, (int16_t)(256 + extR_), 248, 8};
  }
  if (row == 0 && gameplay_) {
    // SMB2/SMB3 place the view window from the camera set during vblank.
    if (game_ == GameId::SMB3) {
      extL_ = smb3_->extL();
      extR_ = smb3_->extR();
    } else if (game_ == GameId::SMB2) {
      extL_ = smb2_->extL();
      extR_ = smb2_->extR();
    }
  }
}

// A picture screen's margins: each layer's line carried on from the screen's
// edges (see Ppu::edgeRemap). Where nothing repeats there, SMB3's hills'
// layer is mirrored at its edge as far as what touches the edge reaches on
// each line: the half-hills there become whole, ending in their own outline
// (a fixed 48 px cut through the next hill), and the logo and the menu text
// on the same layer, clear of the edges, stay out. Elsewhere the edge pixel
// goes on (mirrored, SMB2's frame showed reversed; SMB3's background layer
// left lines of its mountains' foot in the margins).
void SmasGame::pictureRemap(Ppu& ppu, int row) {
  for (int l = 0; l < 3; l++) {
    PpuLayerRemap& rm = ppu.layerRemap[l];
    rm.edge = game_ == GameId::SMB3 && l == 0 && !smb3_->spadeScreen() ? 2 : 1;
    if (rm.edge == 2) rm.mirror = 0xff;
  }
  if (parade_) ppu.layerRemap[0].edge = 0;  // (see fetchWideTile)
}

// SMB2's character select: a frame of 16 px diamonds, orange and blue
// alternating both ways, inside a blank 8 px column at each screen edge. Its
// lattice is carried on over the margins and those columns: two tile columns
// per diamond, the edge diamond's half without the frame's inner outline (the
// left one's left half, the right one's right half) and its mirror, every
// other diamond column two tile rows (half a diamond) lower, so the colours
// alternate as in the frame. The rest of the picture is left as it is.
namespace {
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

int SmasGame::selectLattice(Ppu& ppu, int ty, int x, uint16_t* entry) {
  bool left = x < 128;
  if (left ? x >= 8 : x < 248) return kWideVram;
  int rel = x - (left ? 8 : 232), k = floorDiv(rel, 16), within = rel - k * 16;
  // (the clean half's tiles, see latticeL_; half a diamond up instead near
  // the picture's foot: below it is no frame)
  int row = (ty - (ppu.bgVofs(0) >> 3)) & 31, shift = (k & 1) ? (row < 26 ? 2 : -2) : 0;
  uint16_t e = (left ? latticeL_ : latticeR_)[(row + shift) & 31];
  // (the left clean half is a diamond's left half, the right one its right half)
  bool flip = left ? within >= 8 : within < 8;
  *entry = flip ? e ^ 0x4000 : e;
  return kWideEntry;
}

// SMB2's card between levels: a plain background tile with 2x2-tile motifs
// scattered over it, a blank 8 px column at each screen edge. Its motifs are
// those that stand clear in the background (not the level's picture or the
// logo): read off the card as it appears. A screen in the same mode that
// doesn't read as one (the background three quarters of its edge columns or
// more, two motifs or more: the ending uses that mode too) stays 4:3.
void SmasGame::learnCard() {
  const Ppu& p = snes_.ppu;
  int c0 = p.bgHofs(0) >> 3, r0 = p.bgVofs(0) >> 3;
  auto at = [&](int c, int r) { return p.tileEntry(0, c0 + c, r0 + r); };
  // (the background: the commonest entry of the columns inside the blank ones)
  std::map<uint16_t, int> count;
  for (int r = 0; r < 28; r++)
    for (int c : {1, 2, 3, 4, 29, 30}) count[at(c, r)]++;
  CardPattern& cp = cardPat_;
  cp = CardPattern{};
  for (auto& kv : count)
    if (kv.second > count[cp.bg]) cp.bg = kv.first;
  for (int r = 0; r < 27; r++)
    for (int c = 1; c < 30; c++) {
      uint16_t e = at(c, r);
      if (e == cp.bg || at(c + 1, r) != e + 1 || at(c, r + 1) != e + 16 || at(c + 1, r + 1) != e + 17) continue;
      bool clear = true;  // (the background all round it)
      for (int dr = -1; dr <= 2 && clear; dr++)
        for (int dc = -1; dc <= 2 && clear; dc++) {
          if (dr >= 0 && dr <= 1 && dc >= 0 && dc <= 1) continue;
          int rr = r + dr, cc = c + dc;
          if (rr >= 0 && rr < 28 && cc >= 1 && cc <= 30) clear = at(cc, rr) == cp.bg;
        }
      if (clear && cp.count < 8 && std::find(cp.motif, cp.motif + cp.count, e) == cp.motif + cp.count)
        cp.motif[cp.count++] = e;
    }
  cp.valid = true;
  cp.card = count[cp.bg] * 4 >= 28 * 6 * 3 && cp.count >= 2;
}

// The card's margins (and blank edge columns): its background, with one of
// its motifs in most 6x6-tile cells (as thickly as on the card), the place
// and kind picked by a hash of the cell, whole within the margin. A motif of
// the card's own that its blank edge column cut in half is made whole there.
int SmasGame::cardTile(Ppu& ppu, int ty, int x, uint16_t* entry) {
  int c = floorDiv(x, 8);  // (the screen's tile column)
  if (c >= 1 && c <= 30) return kWideVram;
  if (!cardPat_.valid) return kWideBlank;
  *entry = cardPat_.bg;
  if (!cardPat_.count) return kWideEntry;
  if (c == 0 || c == 31) {
    uint16_t n = ppu.tileEntry(0, (ppu.bgHofs(0) >> 3) + (c ? 30 : 1), (ppu.bgVofs(0) >> 3) + ty);
    for (int i = 0; i < cardPat_.count; i++) {
      uint16_t m = cardPat_.motif[i];
      for (int dy = 0; dy <= 1; dy++)
        if (n == m + (c ? 0 : 1) + 16 * dy) {
          *entry = m + (c ? 1 : 0) + 16 * dy;
          return kWideEntry;
        }
    }
  }
  int cx = floorDiv(c, 6), cy = floorDiv(ty, 6);
  uint32_t h = (uint32_t)(cx * 73856093) ^ (uint32_t)(cy * 19349663);
  h = (h ^ (h >> 13)) * 0x5bd1e995u;
  h ^= h >> 15;
  int ox = h % 5, oy = (h >> 3) % 5, kind = (h >> 6) % cardPat_.count;
  bool present = (h >> 10) % 6 != 0;
  int mc = cx * 6 + ox, dx = c - mc, dy = ty - (cy * 6 + oy);
  bool whole = mc + 1 <= 0 || mc >= 31;
  if (present && whole && dx >= 0 && dx <= 1 && dy >= 0 && dy <= 1)
    *entry = cardPat_.motif[kind] + dx + 16 * dy;
  return kWideEntry;
}

int SmasGame::fetchWideTile(Ppu& ppu, int layer, int mapX, int mapY, int screenX, uint16_t* entry) {
  if (parade_ && layer == 0) {
    // (Rows the strip fills right across, the crowd and the borders: round
    // the map's 256 px, the crowd across the whole bottom. Elsewhere (the
    // sign, the numbers) the background, as at the screen's edges; round,
    // a piece of the sign stood in the margin.)
    if (paradeFull_[mapY & 31]) return kWideVram;
    *entry = paradeBg_;
    return kWideEntry;
  }
  if (throne_) {
    // (BG1: the room's square gold blocks, 4x4 tiles, carried on in step
    // with its own: the left edge shows a block's left half, the right edge
    // its right half; rows in step down the screen. Its narrow bars above
    // them, repeated, read as a grille. Behind, the edge tile columns.)
    int row = mapY - (ppu.bgVofs(layer) >> 3);
    if (layer == 0) {
      static const uint8_t kBlock[4][4] = {
          {0x0a, 0x0b, 0x0c, 0x0d}, {0x1a, 0x1b, 0x1c, 0x1d}, {0x22, 0x23, 0x24, 0x25}, {0x32, 0x33, 0x34, 0x35}};
      int col = floorDiv(screenX, 8) & 3;  // (x 0 the left half's first column)
      uint16_t edge = ppu.tileEntry(0, ppu.bgHofs(0) >> 3, (ppu.bgVofs(0) >> 3) + 16);  // (a block's tile there)
      *entry = (edge & 0xff00) | kBlock[((row % 4) + 4) % 4][col];
      return kWideEntry;
    }
    int c = screenX < 0 ? (((screenX >> 3) % 2) + 2) % 2 : 30 + ((screenX - 256) >> 3) % 2;
    *entry = ppu.tileEntry(layer, (ppu.bgHofs(layer) >> 3) + c, mapY);
    return kWideEntry;
  }
  if (battle_) {
    // (The arena (BG1: its pipes, platforms, floor) carried on past the
    // screen's edges, its two edge tile columns repeated: the pipes and the
    // blocks, 16 px each, go on off the screen (round its 256 px, the other
    // side's pipe mouth met this side's, and the score boxes came round
    // again). The backdrop pattern round its 256 px, seamless.)
    const int x8 = floorDiv(screenX, 8);
    int c = ((x8 % 32) + 32) % 32;
    if (layer == 0) c = x8 < 0 ? ((x8 % 2) + 2) % 2 : 30 + ((((x8 - 32) % 2) + 2) % 2);
    *entry = ppu.tileEntry(layer, (ppu.bgHofs(layer) >> 3) + c, mapY);
    return kWideEntry;
  }
  if (select_) return layer == 0 ? selectLattice(ppu, mapY, screenX, entry) : kWideBlank;
  if (card2_) return layer == 0 ? cardTile(ppu, mapY - (ppu.bgVofs(0) >> 3), screenX, entry) : kWideBlank;
  if (Smb1Engine* e = engine()) return e->fetchWideTile(ppu, layer, mapX, mapY, screenX, entry);
  if (game_ == GameId::SMB2) return smb2_->fetchWideTile(ppu, layer, mapX, mapY, screenX, entry);
  if (game_ == GameId::SMB3) return smb3_->fetchWideTile(ppu, layer, mapX, mapY, screenX, entry);
  return kWideVram;
}

void SmasGame::onVramWrite(Ppu& ppu, uint16_t adr) {
  if (Smb1Engine* e = engine()) e->onVramWrite(ppu, adr);
}
