#include "game/smb1.h"
#include <algorithm>
#include <cstring>

namespace {
// GetXOffscreenBits prologue: STX $04; LDY #1; LDA $071C,Y; SEC; SBC $0219,X;
// STA $07; LDA $071A,Y; SBC $78,X
const uint8_t kSigXOffscreen[] = {0x86, 0x04, 0xA0, 0x01, 0xB9, 0x1C, 0x07, 0x38, 0xFD,
                                  0x19, 0x02, 0x85, 0x07, 0xB9, 0x1A, 0x07, 0xF5, 0x78};
// CheckRightBounds: LDA $071D; CLC; ADC #$30; AND #$F0
const uint8_t kSigSpawn[] = {0xAD, 0x1D, 0x07, 0x18, 0x69, 0x30, 0x29, 0xF0};
// OffscreenBoundsCheck: ADC #$38; SBC #$48; STA $01; LDA $071A; SBC #0; STA $00;
// LDA $071D; CLC; ADC #$48
const uint8_t kSigDespawn[] = {0x69, 0x38, 0xE9, 0x48, 0x85, 0x01, 0xAD, 0x1A, 0x07, 0xE9,
                               0x00, 0x85, 0x00, 0xAD, 0x1D, 0x07, 0x18, 0x69, 0x48};

// PlayerGfxHandler: LDA InjuryTimer; BEQ; LDA FrameCounter; LSR; BCS (blink)
const uint16_t kSigBlink[] = {0xAD, 0xAE, 0x07, 0xF0, 0x100, 0xA5, 0x09, 0x4A, 0xB0};
// ProcFirebar: LDA Enemy_OffscreenBits; AND #$08; BNE SkipFBar; LDA TimerControl
// (the bar, all its flames, is skipped when the pivot is off the 4:3 screen).
// ChkPOffscr: LDX #0; JSR GetXOffscreenBits; STA $00; LDY #0; ASL; BCS KeepOnscr;
// INY; LDA $00; AND #$20; BEQ: holds the player inside the 256 px screen.
const uint16_t kSigChkPOffscr[] = {0xA2, 0x00, 0x20, 0x100, 0x100, 0x85, 0x00, 0xA0, 0x00, 0x0A,
                                   0xB0, 0x100, 0xC8, 0xA5, 0x00, 0x29, 0x20, 0xF0};
// LDA Player_Rel_XPos; STA Player_Pos_ForScroll; STA $05: the position the
// scroll code compares with the screen's middle.
const uint16_t kSigPosForScroll[] = {0xAD, 0xAD, 0x03, 0x8D, 0x55, 0x07, 0x85, 0x05};
// Collisions. Bounding boxes are 8-bit, relative to the game's screen
// (BoundingBox_UL/DR at $0F9C/$0F9E + 4 x object), clamped where they leave
// it; beyond it (the view's margins) they are recorded in 16 bits as the
// game works them out, and compared in 16 bits (see onExec).
// BoundingBoxCore: STX $00; LDA SprObject_Rel_YPos,Y; STA $02; LDA Rel_XPos,Y;
// STA $01; TXA; ASL; ASL; PHA; TAY; LDA SprObj_BoundBoxCtrl,X; ASL; ASL; TAX;
// LDA $01; CLC; ADC BoundBoxCtrlData,X
const uint16_t kSigBoundBox[] = {0x86, 0x00, 0xB9, 0xB8, 0x03, 0x85, 0x02, 0xB9, 0xAD, 0x03, 0x85, 0x01, 0x8A, 0x0A,
                                 0x0A, 0x48, 0xA8, 0xBD, 0x8F, 0x04, 0x0A, 0x0A, 0xAA, 0xA5, 0x01, 0x18, 0x7D};
// SprObjectCollisionCore: STY $06; LDA #1; STA $07; LDA BoundingBox_UL,Y; CMP BoundingBox_UL,X; BCS
const uint16_t kSigCollide[] = {0x84, 0x06, 0xA9, 0x01, 0x85, 0x07, 0xB9, 0x9C, 0x0F, 0xDD, 0x9C, 0x0F, 0xB0};
// GetEnemyBoundBox: TYA; AND Enemy_OffscreenBits; STA $03D9,X (non-zero: no box)
const uint16_t kSigEnemyBoxOff[] = {0x98, 0x2D, 0xD1, 0x03, 0x9D, 0xD9, 0x03, 0xE0, 0x09};
// FireballObjCore: LDA FBall_OffscreenBits; AND #$FC; BEQ (on screen: collide and
// draw); AND #$F0; BNE (erase) ...
const uint16_t kSigFireballOff[] = {0xAD, 0xD2, 0x03, 0x29, 0xFC, 0xF0, 0x100, 0x29, 0xF0, 0xD0};
// Hammer Bros and their hammers (SprObject 17 + misc slot $9E, state $39,X):
// a Bro off the 4:3 screen never throws (SMB1 03:CE78, LL 0D:CD80: LDA
// Enemy_OffscreenBits; AND #$0C; BNE); a hammer off it is erased as it's drawn
// (05:DD62, LL 0F:EBAE: LDA Misc_OffscreenBits; AND #$0F; CMP #$0F; BEQ erase;
// LDA Misc_OffscreenBits; AND #$F0; BEQ keep) and never hits the player
// (03:DDC6, LL 0D:DCE4: ... ORA Misc_OffscreenBits; BNE skip).
const uint8_t kSigHammerThrow[] = {0xAD, 0xD1, 0x03, 0x29, 0x0C, 0xD0};
const uint16_t kSigHammerGone[] = {0xAD, 0xD6, 0x03, 0x29, 0x0F, 0xC9, 0x0F, 0xF0, 0x100,
                                   0xAD, 0xD6, 0x03, 0x29, 0xF0, 0xF0};
const uint8_t kSigHammerColl[] = {0x0D, 0xD6, 0x03, 0xD0};
constexpr int kMiscObj = 17;  // the misc objects (hammers, coins from blocks) in the SprObject arrays
const uint16_t kSigFirebar[] = {0xAD, 0xD1, 0x03, 0x29, 0x08, 0xD0, 0x100, 0xAD, 0x47, 0x07};
// DrawFirebar_Collision: SEC; SBC Enemy_Rel_XPos; CMP #$59; BCC: a flame
// further than $59 px from the pivot in 8 bits has wrapped around the 4:3
// screen's edge and is hidden.
const uint16_t kSigFirebarWrap[] = {0x38, 0xED, 0xAE, 0x03, 0xC9, 0x59, 0x90};
// Enemy groups (2-3 Goombas or Koopas) start at the screen's right edge, 24 px
// apart: LDA ScreenRight_PageLoc; STA $02; LDA ScreenRight_X_Pos; STA $03
const uint8_t kSigGroup[] = {0xAD, 0x1B, 0x07, 0x85, 0x02, 0xAD, 0x1D, 0x07, 0x85, 0x03};
// The jumpspring area object (SMB1 03:ABD5, LL 0D:A967) takes the first free
// object slot without checking one was found, then sets up the spring:
// JSR GetAreaObjXPosition; STA Enemy_X_Position,X; LDA CurrentPageLoc;
// STA $79,X; JSR GetAreaObjYPosition; STA $0238,X; STA $5E,X; LDA #$32;
// STA Enemy_ID,X (the hook is at that first JSR, X = the slot found).
const uint16_t kSigSpring[] = {0x20,  0x100, 0x100, 0x9D, 0x1A, 0x02, 0xAD, 0x25, 0x07, 0x95, 0x79,
                               0x20,  0x100, 0x100, 0x9D, 0x38, 0x02, 0x95, 0x5E, 0xA9, 0x32, 0x95, 0x1C};
// All-Stars' own Bowser drawing ($05:D14E, LL $0F:D848) sets X bit 8 in the
// high table for each of his three sprite columns past the 256 px screen:
// $03 (16x16) for the first column's two big sprites, but $01 (8x8) for the
// last column's (LDA #$01; STA $0D0C,Y; STA $0D10,Y; STA $0D14,Y). Off the
// 4:3 screen that hides them (an 8x8 sprite just left of it entirely); in
// the margins half his shell was cut off as he came into view. The first two
// stores keep the size bit there (see onWrite).
const uint8_t kSigBowserHi[] = {0xA9, 0x01, 0x99, 0x0C, 0x0D, 0x99, 0x10, 0x0D, 0x99, 0x14, 0x0D};
// Objects placed 32 px past the screen's right edge (Bowser's flames):
// LDA ScreenRight_X_Pos; CLC; ADC #$20; STA Enemy_X_Position,X
const uint8_t kSigEdgeSpawn[] = {0xAD, 0x1D, 0x07, 0x18, 0x69, 0x20, 0x9D, 0x1A, 0x02};

// Sprite drawing code that hides sprite columns off the 4:3 screen, read
// from the object's off-screen bits (SprObject_OffscrBits $03D0+). Every
// other reader is game logic (fireball/hammer lifetimes, enemy behaviour,
// collisions) and keeps the original bits.
struct DrawSite {
  uint8_t sig[4];
  uint16_t relAddr;  // SprObject_Rel_XPos written just before drawing
};
const DrawSite kDrawSites[] = {
    {{0xAD, 0xD1, 0x03, 0x4A}, 0x03ae},  // enemies: LDA Enemy_OffscreenBits; LSR
    {{0xAD, 0xD1, 0x03, 0x48}, 0x03ae},  // enemies: LDA Enemy_OffscreenBits; PHA
    {{0xAD, 0xD4, 0x03, 0x0A}, 0x03b1},  // bumped block: LDA Block_OffscreenBits; ASL
    {{0xAD, 0xD4, 0x03, 0xF0}, 0x03b1},  // bumped block: LDA Block_OffscreenBits; BEQ
    {{0xAD, 0xD5, 0x03, 0xF0}, 0x03b1},  // second block
    {{0xAD, 0xD3, 0x03, 0x29}, 0x03b0},  // air bubble: LDA Bubble_OffscreenBits; AND
};

// The enemy loop (SMB1 03:AD94): STX ObjectOffset; LDA $0E67; BEQ; CMP #$7F;
// BEQ; JSR EnemiesAndLoopsCore (each of the 10 object slots; an empty one
// spawns from the level's enemy data).
const uint16_t kSigEnemyLoop[] = {0x86, 0x9E, 0xAD, 0x67, 0x0E, 0xF0, 0x100, 0xC9, 0x7F, 0xF0, 0x100, 0x20};
// NMI: the sprite buffer ($0800, 544 bytes) to OAM: STZ $00; REP #$20;
// STZ $2102; LDA #$0004; STA $01; LDA #$0008; STA $03
const uint16_t kSigOamDma[] = {0x64, 0x00, 0xC2, 0x20, 0x9C, 0x02, 0x21, 0xA9, 0x04, 0x00,
                               0x85, 0x01, 0xA9, 0x08, 0x00, 0x85, 0x03};

constexpr int kHudLines = 32;  // BG3 status line at the top of the screen
// Widescreen layout: the extra width is shown to the right of the game's 256
// px screen first, as far as the game has built the level. The area parser
// runs 87-127 px past the 4:3 edge and must not be pushed further: level
// objects act when parsed (scroll locks, warp zones, frenzies, castle loop
// checks). An enemy needs the level under it built too (or it falls through
// the old collision map there): with 72 px every enemy in view (16 px wide)
// stands on built level.
constexpr int kMaxRight = 72;
// The rest of the width (16:9 too) reaches back over the level just passed,
// and the player may walk into it: as far as the game's collision map still
// holds it (32 columns behind the parser, see playerLeftLimit).
constexpr int kMaxLeft = 128;
constexpr int kFireballObj = 11;  // the player's two fireballs in the SprObject arrays ($0219 + 11 ...)
constexpr int kBg2Pages = 16;  // WRAM $7E2000-$7E9FFF (page = $0EFE & 15, $04:9B7D), 32x32 tiles each
}  // namespace

Smb1Engine::Smb1Engine(Snes& snes, uint8_t bankLo, uint8_t bankHi) : snes_(snes) {
  uint32_t x = findSig(kSigXOffscreen, sizeof(kSigXOffscreen), bankLo, bankHi);
  uint32_t s = findSig(kSigSpawn, sizeof(kSigSpawn), bankLo, bankHi);
  uint32_t d = findSig(kSigDespawn, sizeof(kSigDespawn), bankLo, bankHi);
  if (x && s && d) {
    spawnOp_ = s + 5;
    despawnLOp_ = d + 3;
    despawnROp_ = d + 18;
    ok_ = true;
  }
  coopInit(bankLo, bankHi);
  blinkLda_ = findSig(kSigBlink, sizeof(kSigBlink) / 2, bankLo, bankHi);
  firebarLda_ = findSig(kSigFirebar, sizeof(kSigFirebar) / 2, bankLo, bankHi);
  if (uint32_t c = findSig(kSigChkPOffscr, sizeof(kSigChkPOffscr) / 2, bankLo, bankHi)) chkPOffscr_ = c + 5;
  if (uint32_t c = findSig(kSigPosForScroll, sizeof(kSigPosForScroll) / 2, bankLo, bankHi)) posForScroll_ = c + 6;
  // ScrollHandler's amount (LDA Player_Pos_ForScroll; CMP #$50; BCC; LDA
  // SideCollisionTimer; BNE; LDY Player_X_Scroll; DEY; BMI): see onExec.
  static const uint16_t kSigScrollAmt[] = {0xAD, 0x55, 0x07, 0xC9, 0x50, 0x90, 0x100, 0xAD, 0x100,
                                           0x100, 0xD0, 0x100, 0xAC, 0xFF, 0x06, 0x88, 0x30};
  if (uint32_t c = findSig(kSigScrollAmt, sizeof(kSigScrollAmt) / 2, bankLo, bankHi)) scrollAmt_ = c + 12;
  if (uint32_t c = findSig(kSigBoundBox, sizeof(kSigBoundBox) / 2, bankLo, bankHi)) {
    boundBox_ = c;
    boxTable_ = (c & 0xff0000) | snes_.peek16(c + 27);
  }
  if (uint32_t c = findSig(kSigCollide, sizeof(kSigCollide) / 2, bankLo, bankHi)) collide_ = c;
  fireballOff_ = findSig(kSigFireballOff, sizeof(kSigFireballOff) / 2, bankLo, bankHi);
  hammerThrow_ = findSig(kSigHammerThrow, sizeof(kSigHammerThrow), bankLo, bankHi);
  if (uint32_t h = findSig(kSigHammerGone, sizeof(kSigHammerGone) / 2, bankLo, bankHi)) {
    hammerGone_ = h;
    hammerKeep_ = (h & 0xff0000) | (uint16_t)(h + 16 + (int8_t)snes_.peek(h + 15));  // (BEQ keep)
  }
  hammerColl_ = findSig(kSigHammerColl, sizeof(kSigHammerColl), bankLo, bankHi);
  if (uint32_t c = findSig(kSigEnemyBoxOff, sizeof(kSigEnemyBoxOff) / 2, bankLo, bankHi)) enemyBoxOff_ = c + 4;
  if (uint32_t w = findSig(kSigFirebarWrap, sizeof(kSigFirebarWrap) / 2, bankLo, bankHi)) firebarWrap_ = w + 4;
  if (firebarLda_) firebarEnd_ = firebarLda_ + 7 + (int8_t)snes_.peek(firebarLda_ + 6);  // (BNE SkipFBar: its RTS)
  // FirebarCollision, after TYA; PHA: LDA StarInvincibleTimer; ORA
  // TimerControl; BNE NoColFB (its PLA: the end)
  static const uint8_t kSigFirebarColl[] = {0x98, 0x48, 0xAD, 0xAF, 0x07, 0x0D, 0x47, 0x07, 0xD0};
  if (uint32_t c = findSig(kSigFirebarColl, sizeof(kSigFirebarColl), bankLo, bankHi)) {
    firebarColl_ = c + 2;
    firebarCollEnd_ = c + 10 + (int8_t)snes_.peek(c + 9);
  }
  if (uint32_t g = findSig(kSigGroup, sizeof(kSigGroup), bankLo, bankHi)) groupPlaced_ = g + sizeof(kSigGroup);
  springSlot_ = findSig(kSigSpring, sizeof(kSigSpring) / 2, bankLo, bankHi);
  if (uint32_t b = findSig(kSigBowserHi, sizeof(kSigBowserHi), bankLo, bankHi)) {
    bowserBig_[0] = b + 2;
    bowserBig_[1] = b + 5;
  }
  for (uint32_t from = bankLo; from <= bankHi && edgeSpawnOps_.size() < 4; from++) {
    const std::vector<uint8_t>& rom = snes_.rom;
    size_t base = (size_t)from * 0x8000;
    if (base + 0x8000 > rom.size()) break;
    for (size_t i = 0; i + sizeof(kSigEdgeSpawn) <= 0x8000; i++)
      if (memcmp(&rom[base + i], kSigEdgeSpawn, sizeof(kSigEdgeSpawn)) == 0)
        edgeSpawnOps_.push_back((from << 16) | (0x8000 + (uint32_t)i + 5));
  }
  for (const DrawSite& ds : kDrawSites) {
    if (uint32_t p = findSig(ds.sig, sizeof(ds.sig), bankLo, bankHi)) drawSites_.push_back({p, ds.relAddr});
  }
  // One reader of the bits that isn't drawing code counts too: All-Stars'
  // own vine handler ($03:BA36, LL $0D:B7F9) erases the vine once it is off
  // the 4:3 screen (LDA Enemy_OffscreenBits; AND #$0F; CMP #$0F; BNE). It
  // vanished in the margin as the screen scrolled on; now off the view.
  static const uint8_t kSigVineGone[] = {0xAD, 0xD1, 0x03, 0x29, 0x0F, 0xC9, 0x0F, 0xD0};
  if (uint32_t p = findSig(kSigVineGone, sizeof(kSigVineGone), bankLo, bankHi)) drawSites_.push_back({p, 0x03ae});
  if (uint32_t e = findSig(kSigEnemyLoop, sizeof(kSigEnemyLoop) / 2, bankLo, bankHi)) {
    enemyCall_ = e + 11;
    enemyRet_ = e + 14;
  }
  oamUpload_ = findSig(kSigOamDma, sizeof(kSigOamDma) / 2, bankLo, bankHi);
  // BlockBufferCollision: LDA BlockBuffer_X_Adder,Y; CLC; ADC SprObject_X,X;
  // STA $05; LDA SprObject_Page,X; ADC #0; AND #1; LSR; ORA $05; ROR; LSR;
  // LSR; LSR; JSR GetBlockBufferAddr; LDY $04; LDA SprObject_Y,X; CLC; ADC
  // BlockBuffer_Y_Adder,Y; AND #$F0; SEC; SBC #$20; STA $02; TAY; LDA ($06),Y
  static const uint8_t kSigBBColl[] = {0x18, 0x7D, 0x19, 0x02, 0x85, 0x05, 0xB5, 0x78, 0x69, 0x00,
                                       0x29, 0x01, 0x4A, 0x05, 0x05, 0x6A, 0x4A, 0x4A, 0x4A, 0x20};
  if (uint32_t b = findSig(kSigBBColl, sizeof(kSigBBColl), bankLo, bankHi)) {
    if (snes_.peek(b - 3) == 0xB9 && snes_.peek(b + 39) == 0xB1 && snes_.peek(b + 40) == 0x06) {
      bbXAdder_ = (b & 0xff0000) | snes_.peek16(b - 2);
      bbRead_ = b + 39;
    }
  }
  // GetMTileAttrib (TAY; AND #$C0; ASL; ROL; ROL; TAX; TYA; RTS); the first
  // JSR to it followed by CMP tbl,X; RTS is CheckForSolidMTiles: a metatile
  // is solid from tbl[its top two bits] up.
  static const uint8_t kSigMTileAttrib[] = {0xA8, 0x29, 0xC0, 0x0A, 0x2A, 0x2A, 0xAA, 0x98, 0x60};
  if (uint32_t g = findSig(kSigMTileAttrib, sizeof(kSigMTileAttrib), bankLo, bankHi)) {
    const uint8_t call[] = {0x20, (uint8_t)g, (uint8_t)(g >> 8), 0xDD};
    if (uint32_t s = findSig(call, sizeof(call), (uint8_t)(g >> 16), (uint8_t)(g >> 16))) {
      uint32_t tbl = (s & 0xff0000) | snes_.peek16(s + 4);
      for (int i = 0; i < 4; i++) solidFrom_[i] = snes_.peek(tbl + i);
    }
  }
  // All-Stars' own drawing code (sparks, Bowser's fire, ...) works out
  // positions in 16 bits: SEC; SBC $42 (the camera), then the sprite X.
  for (uint32_t bank = bankLo; bank <= bankHi; bank++) {
    const std::vector<uint8_t>& rom = snes_.rom;
    size_t base = (size_t)bank * 0x8000;
    if (base + 0x8000 > rom.size()) break;
    for (size_t i = 0; i + 3 <= 0x8000; i++)
      if (rom[base + i] == 0x38 && rom[base + i + 1] == 0xE5 && rom[base + i + 2] == 0x42)
        rel16Sites_.push_back((bank << 16) | (0x8000 + (uint32_t)i + 3));
  }
  // Cloud column builder: LDA tbl,Y; STA $7F2004,X; LDA tbl+2,Y; STA $7F2006,X
  static const uint16_t kSigCloud[] = {0xB9, 0x100, 0x100, 0x9F, 0x04, 0x20, 0x7F,
                                       0xB9, 0x100, 0x100, 0x9F, 0x06, 0x20, 0x7F};
  uint32_t c = findSig(kSigCloud, sizeof(kSigCloud) / 2, bankLo, bankHi);
  if (c) cloudTiles_ = (c & 0xff0000) | snes_.peek16(c + 1);
  // Area pointer lookup: LDA worldTbl,Y; CLC; ADC $0760; TAY; LDA areaTbl,Y
  static const uint16_t kSigArea[] = {0xB9, 0x100, 0x100, 0x18, 0x6D, 0x60, 0x07, 0xA8, 0xB9, 0x100, 0x100};
  uint32_t a = findSig(kSigArea, sizeof(kSigArea) / 2, bankLo, bankHi + 1);
  if (a) {
    worldTbl_ = (a & 0xff0000) | snes_.peek16(a + 1);
    areaTbl_ = (a & 0xff0000) | snes_.peek16(a + 9);
    // The world offset table runs up to the start of the area table.
    worlds_ = (int)((areaTbl_ & 0xffff) - (worldTbl_ & 0xffff));
    if (worlds_ < 1 || worlds_ > 16) worlds_ = 0;
  }
  reset();
}

void Smb1Engine::reset() {
  for (int& c : colWorld_) c = -100000;
  clearShadow();
  curWrites_ = 0;
  bg2Fresh_ = true;
  lockedRoom_ = padRoom_ = openRoom_ = false;
  pipeFade_ = false;
  areaFrames_ = 0;
  loadedMidArea_ = true;
  areaLeftLoaded_ = false;
  fillValid_ = fillMtValid_ = false;
  fresh_ = 0;
  for (uint8_t& b : oamXSet_) b = 0;
  for (CoopPl& p : pl_) p.valid = p.drawn = p.known = p.wait = false;  // (co-op: the others come back beside the game's player)
  live_ = 0;
  baseKnown_ = false;
  cur_ = b0Owner_ = liveDrawn_ = -1;
  coopMove_ = coopDrawPhase_ = 0;
  coopEnemy_ = -1;
  coopObjPhase_ = coopHamPhase_ = vicPhase_ = coopWhirlPhase_ = 0;
  starMusic_ = false;
  coopFirePhase_ = 0;
  lockForced_ = false;
}

namespace {
template <class T> void put(std::vector<uint8_t>& out, const T& v) {
  const uint8_t* p = (const uint8_t*)&v;
  out.insert(out.end(), p, p + sizeof(v));
}
// Reads the next field if the blob has it (older states end earlier).
struct Reader {
  const uint8_t* p;
  size_t n;
  template <class T> bool get(T& v) {
    if (n < sizeof(v)) return false;
    std::memcpy(&v, p, sizeof(v));
    p += sizeof(v);
    n -= sizeof(v);
    return true;
  }
};
}  // namespace

void Smb1Engine::saveExt(std::vector<uint8_t>& out) const {
  put(out, (int32_t)areaLeft_);
  // The fill column (it was chosen from columns a load may no longer have).
  put(out, (uint8_t)fillValid_);
  put(out, fill_);
  // Enemies held until the level under them is built, and the sprites' true
  // positions: those of the buffer being written and those on display.
  put(out, fresh_);
  put(out, oamX_);
  put(out, oamXSet_);
  put(out, snes_.ppu.objTrueX);
  for (bool b : snes_.ppu.objTrueXSet) put(out, (uint8_t)b);
  put(out, (uint8_t)fillMtValid_);
  put(out, fillMt_);
  put(out, (uint8_t)fillWall_);
  // (the objects' boxes, the last frame's used by this one's tests)
  put(out, (uint8_t)'B');
  put(out, boxL_);
  put(out, boxR_);
  put(out, boxFrame_);
  coopSave(out);
}

void Smb1Engine::loadExt(const uint8_t* p, size_t n, bool running) {
  Reader rd{p, n};
  int32_t left;
  if (!rd.get(left)) return;
  areaLeft_ = std::clamp<int32_t>(left, -0x8000, 0x7fff);
  areaLeftLoaded_ = true;
  uint8_t valid;
  if (!rd.get(valid) || !rd.get(fill_)) return;
  fillValid_ = valid != 0;
  if (!rd.get(fresh_) || !rd.get(oamX_) || !rd.get(oamXSet_)) return;
  for (uint8_t& b : oamXSet_) b = b != 0;
  int16_t trueX[128];
  uint8_t trueSet[128];
  if (!rd.get(trueX) || !rd.get(trueSet)) return;
  if (running) {
    std::memcpy(snes_.ppu.objTrueX, trueX, sizeof(trueX));
    for (int i = 0; i < 128; i++) snes_.ppu.objTrueXSet[i] = trueSet[i] != 0;
  }
  uint8_t mtValid;
  if (rd.get(mtValid) && rd.get(fillMt_)) fillMtValid_ = mtValid != 0;
  uint8_t wall;
  if (rd.get(wall)) {
    fillWall_ = wall != 0;
  } else {
    // (Older states: a wall fill is solid from the playfield's top to the floor.)
    fillWall_ = true;
    for (int r = 4; r < 28; r++)
      for (int h = 0; h < 2; h++) fillWall_ = fillWall_ && fill_[h][r] != fill_[0][0];
  }
  if (rd.n > 0 && rd.p[0] == 'B') {
    uint8_t tag;
    rd.get(tag);
    rd.get(boxL_);
    rd.get(boxR_);
    rd.get(boxFrame_);
  } else {
    std::fill(std::begin(boxFrame_), std::end(boxFrame_), 0u);  // (older states: none)
  }
  coopLoad(rd.p, rd.n);
}

void Smb1Engine::clearShadow() {
  for (int& c : shadowCol_) c = -100000;
  lastParser_ = 0;
}

uint32_t Smb1Engine::findSig(const uint16_t* sig, size_t n, uint8_t bankLo, uint8_t bankHi) const {
  const std::vector<uint8_t>& rom = snes_.rom;
  for (uint32_t bank = bankLo; bank <= bankHi; bank++) {
    size_t base = (size_t)bank * 0x8000;
    if (base + 0x8000 > rom.size()) break;
    for (size_t i = 0; i + n <= 0x8000; i++) {
      size_t k = 0;
      while (k < n && (sig[k] == 0x100 || rom[base + i + k] == sig[k])) k++;
      if (k == n) return (bank << 16) | (0x8000 + (uint32_t)i);
    }
  }
  return 0;
}

uint32_t Smb1Engine::findSig(const uint8_t* sig, size_t n, uint8_t bankLo, uint8_t bankHi) const {
  const std::vector<uint8_t>& rom = snes_.rom;
  for (uint32_t bank = bankLo; bank <= bankHi; bank++) {
    size_t base = (size_t)bank * 0x8000;
    if (base + 0x8000 > rom.size()) break;
    for (size_t i = 0; i + n <= 0x8000; i++) {
      if (memcmp(&rom[base + i], sig, n) == 0) return (bank << 16) | (0x8000 + (uint32_t)i);
    }
  }
  return 0;
}

void Smb1Engine::patch(uint32_t adr, uint8_t v) {
  if (!original_.count(adr)) original_[adr] = snes_.peek(adr);
  snes_.poke(adr, v);
}

void Smb1Engine::unpatchAll() {
  for (auto& o : original_) snes_.poke(o.first, o.second);
}

void Smb1Engine::markExec() {
  if (!ok_) return;
  for (const auto& s : drawSites_) snes_.markExec(s.pc, true);
  if (blinkLda_) snes_.markExec(blinkLda_, true);
  if (firebarLda_) snes_.markExec(firebarLda_, true);
  if (chkPOffscr_) snes_.markExec(chkPOffscr_, true);
  if (scrollAmt_) snes_.markExec(scrollAmt_, true);
  if (posForScroll_) snes_.markExec(posForScroll_, true);
  for (uint32_t pc : {boundBox_, collide_, enemyBoxOff_, fireballOff_, hammerThrow_, hammerGone_, hammerColl_})
    if (pc) snes_.markExec(pc, true);
  if (firebarWrap_) snes_.markExec(firebarWrap_, true);
  if (firebarEnd_) snes_.markExec(firebarEnd_, true);
  if (firebarColl_) snes_.markExec(firebarColl_, true);
  if (firebarCollEnd_) snes_.markExec(firebarCollEnd_, true);
  if (groupPlaced_) snes_.markExec(groupPlaced_, true);
  if (springSlot_) snes_.markExec(springSlot_, true);
  for (uint32_t pc : {enemyCall_, enemyRet_, oamUpload_, bbRead_})
    if (pc) snes_.markExec(pc, true);
  for (uint32_t pc : rel16Sites_) snes_.markExec(pc, true);
  if (livesSet_) snes_.markExec(livesSet_, true);
  if (titleInput_) {
    snes_.markExec(titleInput_, true);
    snes_.markExec(titleInput_ + (lostLevels_ ? 0x28 : 0x2A), true);
  }
  if (coopOk_)
    for (uint32_t pc : {coopCall_, coopAfter_, coopDrawEnd_, coopEnemyAt_, coopDeathSmall_, coopLeftHi_, coopFire_, coopPack_, coopHammer_, coopStar_, coopVicDraw_, coopThanks_, coopEndStart_, coopWhirlRet_, coopLoop_, coopMenu_[0], coopMenu_[1], coopMenu_[2]})
      if (pc) snes_.markExec(pc, true);
}

void Smb1Engine::frameStart(int want, bool active, bool hudAnchor) {
  hudAnchor_ = hudAnchor;
  if (coopOk_) coopFrame();  // (co-op: see smb1coop.cpp)
  ref16Writes_ = 0;  // (a game frame draws within one emulated frame)
  uint8_t operMode = ram(0x0770), task = ram(0x0772);
  uint8_t routine = ram(0x000f);  // GameEngineSubroutine: 2/3 pipe entries, $0B dying, $06 losing the life
  bool wasGameplay = gameplay_;
  // The title screen (OperMode 0, task 3: GameMenuRoutine) is 1-1's start
  // with the logo over it, and its demo plays the level: shown as play is
  // (centred while the camera stands at the start, see titleL_), but enemies
  // keep the 4:3 game's range (the demo's inputs are recorded).
  bool title = operMode == 0 && task == 3;
  gameplay_ = (operMode == 1 && task == 3) || title;
  cam16_ = camera();
  trackParser();  // (a castle loop moves the column history before this frame is drawn)
  // Clouds stream only while $0EDC is set ($04:8B51); night levels leave the
  // BG3 mode at 1 with a stale panorama from an earlier level.
  cloudStream_ = cloudTiles_ != 0 && ram(0x005c) == 1 && ram(0x0edc) != 0;
  // Single-screen rooms (the underground coin rooms, LL 8-4's two-pipe room)
  // have the scroll locked from the moment play starts in them, the camera on
  // a page boundary: show them centred (padded or open: see padRoom_ below).
  // A lock engaged later (warp zones:
  // 1-2's stops the camera at 2817) keeps the wide view. (A state load counts
  // as a start.) The death sequence also sets $0723 (a timer), so it keeps
  // the view it had.
  if (!gameplay_) areaFrames_ = 0;
  else if (areaFrames_ < 255) areaFrames_++;
  if (routine != 0x0b && routine != 0x06) {
    if (ram(0x0723) == 0) lockedRoom_ = false;
    else if (gameplay_ && areaFrames_ <= 30 && (cam16_ & 0xff) == 0) lockedRoom_ = true;
  }
  int total = ok_ && want > 0 ? std::min(2 * want, kMaxRight + kMaxLeft) : 0;
  int wide = std::min(total, kMaxRight), back = total - wide;  // (to the right first: see kMaxRight)
  // Where the area began (the game builds nothing to the left of it): the
  // view continues its floor there (see makeFill) and the player stops at it.
  // After a state load the columns kept in VRAM reach about 128 px back.
  if (gameplay_ && areaFrames_ == 1 && !areaLeftLoaded_) {
    areaLeft_ = loadedMidArea_ ? std::max(0, cam16_ - 128) : cam16_;
    fillValid_ = fillMtValid_ = false;
  }
  // (Only the first frame after a load: a state loaded between areas, as a
  // rewind into a pipe can be, comes before a genuine area start.)
  loadedMidArea_ = areaLeftLoaded_ = false;
  if (gameplay_ && !fillValid_) makeFill();
  if (cam16_ < areaLeft_) areaLeft_ = cam16_;  // (LL 8-4's loops take the camera back)
  // Bowser beaten (VictoryMode, $0770 = 2): the walk to Toad (or the
  // princess) and the messages stay wide, centred on the game's screen as the
  // messages are, the 4:3 picture eased there 1 px a frame from where play
  // left it. (They outlasted the 10 s below and dropped to 4:3.)
  bool victory = operMode == 2;
  // Leaving play (a pipe, a death, a level's end, a vine, the end of the
  // victory), the view switches once the screen is black (the 4:3 picture is
  // centred: switching at full brightness jumped it sideways). (A safety net
  // of 10 s.)
  if ((wasGameplay && !gameplay_ && !victory) || (wasVictory_ && !victory)) {
    pipeFade_ = true;
    fadeFrames_ = 0;
  }
  wasVictory_ = victory;
  if (gameplay_ || victory || snes_.ppu.forcedBlank() || snes_.ppu.brightness() == 0 || ++fadeFrames_ > 600)
    pipeFade_ = false;
  if (!victory && !pipeFade_) victoryL_ = -1;
  // Between levels, the card (WORLD 1-2, the lives left, the level's
  // picture) and the TIME UP screen (OperMode_Task 1) are shown wide too,
  // centred: the margins are their backdrop, the status line split as in
  // play. (Once the last view has gone dark: the card comes up from black.)
  card_ = operMode == 1 && task == 1 && !pipeFade_;
  // A single-screen room that opens with a wall (the coin rooms) is shown
  // centred, the wall carried on into both margins. On the right the game's
  // own next column stays where something in the room's last column carries
  // on into it (the exit pipe: its right half), then the wall.
  padRoom_ = lockedRoom_ && fillValid_ && fillWall_;
  // One that opens on open ground (the scenes before x-2's pipe) is shown
  // centred too: the area's floor and scenery carried on left of its start,
  // on the right what the parser built (its last column carried on past that).
  openRoom_ = lockedRoom_ && fillValid_ && !fillWall_;
  if (openRoom_) wide = back = total / 2;
  if (padRoom_) {
    auto mt = [&](int c, int row) { return ram((((c >> 4) & 1) ? 0x05d0 : 0x0500) + row * 16 + (c & 15)); };
    int c = (cam16_ >> 4) + 15;
    bool across = false;
    for (int row = 0; row < 11; row++) across |= mt(c, row) != 0 && mt(c + 1, row) != 0;  // (above the floor)
    roomRight_ = cam16_ + 256 + (across ? 16 : 0);
    wide = back = total / 2;
  }
  if (card_) wide = back = total / 2;
  if (title) {
    int target = cam16_ <= areaLeft_ ? total / 2 : back;
    if (titleL_ < 0) titleL_ = target;
    titleL_ += (titleL_ < target) - (titleL_ > target);
  } else if (!pipeFade_) {
    titleL_ = -1;  // (kept while the title fades out)
  }
  if (titleL_ >= 0) {
    back = titleL_;
    wide = total - back;
  }
  if (victory) {
    if (victoryL_ < 0) victoryL_ = back;
    victoryL_ += (victoryL_ < total / 2) - (victoryL_ > total / 2);
  }
  if (victoryL_ >= 0) {
    back = victoryL_;
    wide = total - back;
  }
  bool show = card_ || ((gameplay_ || victory || pipeFade_) && (!lockedRoom_ || padRoom_ || openRoom_));
  extL_ = show ? back : 0;
  extR_ = show ? wide : 0;
  if (warpPending_) {
    // Start the game from the title screen if needed, then warp.
    if (operMode == 1) {
      applyWarp();
    } else {
      inputOverride_ = (warpTimer_ % 30) < 4 ? kPadStart : 0;
      if (++warpTimer_ > 900) {
        warpPending_ = false;
        inputOverride_ = 0;
      }
    }
  }
  if (!ok_) return;
  // Enemy activity range: the 4:3 game's distances, measured from the wide
  // view's edge (so enemies, groups included, come in from off the display,
  // and groups land where they do on the console).
  enemyRange_ = active && extR_ > 0 && gameplay_ && !title && !padRoom_ && !openRoom_;
  if (enemyRange_) rescueSpring();
  if (enemyRange_) {
    int spawn = 0x30 + extR_;
    int left = std::min(std::max(0x48, extL_ + 16), 150);
    int right = spawn + 24;
    patch(spawnOp_, (uint8_t)spawn);
    patch(despawnLOp_, (uint8_t)left);
    patch(despawnROp_, (uint8_t)right);
    for (uint32_t op : edgeSpawnOps_) patch(op, (uint8_t)(0x20 + extR_));
  } else {
    unpatchAll();
  }
  checkBg2Source();
  // (Decided again while an area starts, or when the panorama's shape changes.)
  if (gameplay_ && (areaFrames_ <= 60 || bg2Off_ != seamOff_ || bg2Period_ != seamPeriod_)) {
    seamOff_ = bg2Off_;
    seamPeriod_ = bg2Period_;
    bg2Mirror_ = bg2StartSeam();
  }
}

// Follows the area parser: a castle maze loop takes player, camera and parser
// back exactly 4 pages while VRAM stays as it was, so the column history
// moves with them (128 columns keeps the shadow slots); a new area clears it.
void Smb1Engine::trackParser() {
  int pp = parserPos();
  if (pp < lastParser_) {
    int drop = lastParser_ - pp;
    if (gameplay_ && drop > 1024 - 64 && drop <= 1024) {
      for (int& w : colWorld_) w -= 128;
      for (int& w : shadowCol_) w -= 128;
      areaLeft_ -= 1024;
      // The hills layer goes back half as far, its whole 512 px map: on the
      // console it looks the same, and the panorama the margins are drawn
      // from is read 64 columns further on. (Left alone, the margins' offset
      // was voted out of step, and 4-4's walls showed others in them.)
      if (bg2Period_ > 0) bg2Off_ = (bg2Off_ + 64) % bg2Period_;
      bg2Looped_ = true;
    } else {
      clearShadow();
    }
  }
  lastParser_ = pp;
}

uint16_t Smb1Engine::bg2Entry(int col, int row) const {
  if (row >= 31) return 0x0024;  // the column builder blanks the last row
  int page = col >> 5;
  return snes_.wram[0x2000 + page * 0x800 + row * 0x40 + (col & 31) * 2] |
         (snes_.wram[0x2001 + page * 0x800 + row * 0x40 + (col & 31) * 2] << 8);
}

// The panorama's length in tile columns: its pages that hold anything (the
// game fills as many as the level needs and zeroes the rest; 4-1 has 8, 8-1
// 13), less any columns at the end that repeat its start (4-1: 248). Beyond
// it the panorama loops. 0: no panorama.
int Smb1Engine::panoramaPeriod() const {
  int pages = 0;
  for (int p = 0; p < kBg2Pages; p++) {
    const uint8_t* b = snes_.wram + 0x2000 + p * 0x800;
    if (std::all_of(b, b + 0x800, [](uint8_t v) { return v == 0; })) break;
    pages = p + 1;
  }
  int cols = pages * 32;
  auto sameCol = [&](int a, int b) {
    for (int r = 0; r < 31; r++)
      if (bg2Entry(a, r) != bg2Entry(b, r)) return false;
    return true;
  };
  for (int p = 8; p < cols; p += 8) {
    bool periodic = true;
    for (int c = 0; c + p < cols && periodic; c++) periodic = sameCol(c, c + p);
    if (periodic) return p;
  }
  return cols;
}

// Whether the panorama's end doesn't join its start, the column shown where
// the level begins (rows filled on one side, sky on the other).
bool Smb1Engine::bg2StartSeam() const {
  if (bg2Period_ <= 0) return false;
  std::map<uint16_t, int> count;  // (the sky: the commonest tile of its top rows)
  for (int c = 0; c < bg2Period_; c += 4)
    for (int r = 0; r < 8; r++) count[bg2Entry(c, r)]++;
  uint16_t sky = 0;
  int best = -1;
  for (auto& kv : count)
    if (kv.second > best) {
      sky = kv.first;
      best = kv.second;
    }
  int start = bg2Off_ % bg2Period_, end = (start + bg2Period_ - 1) % bg2Period_, n = 0;
  for (int r = 0; r < 31; r++) n += (bg2Entry(start, r) == sky) != (bg2Entry(end, r) == sky);
  return n > 2;
}

void Smb1Engine::checkBg2Source() {
  // Use the WRAM panorama only while the columns on screen match it.
  if (!gameplay_) {
    bg2Fresh_ = true;  // (area loads: decide afresh once play starts)
    return;
  }
  bool match = false, plain = false;
  {
    Ppu& ppu = snes_.ppu;
    int approx = cam16_ >> 1;  // the layer scrolls at half speed
    int h = ppu.bgHofs(1);
    int abs = approx + (((h - approx) % 512 + 512 + 256) % 512 - 256);
    uint16_t base = ppu.bgMapBase(1);
    auto vramCol = [&](int col, uint16_t* v) {
      int vc = col & 63;
      uint16_t mapCol = base + ((vc & 32) ? 0x400 : 0) + (vc & 31);
      for (int r = 0; r < 31; r++) v[r] = ppu.vram[(mapCol + r * 32) & 0x7fff];
    };
    auto fits = [&](const uint16_t* v, int pc) {
      pc = (pc % bg2Period_ + bg2Period_) % bg2Period_;
      for (int r = 0; r < 31; r++)
        if (v[r] != bg2Entry(pc, r)) return false;
      return true;
    };
    auto sky = [](const uint16_t* v) { return std::all_of(v, v + 31, [&](uint16_t e) { return e == v[0]; }); };
    uint16_t v[31];
    if (bg2Fresh_) {
      // (Written whole when the area loads.) The layer can show it whole
      // pages further on: start with the page offset the columns on screen
      // fit best.
      bg2Static_ = bg2SeenStream_ = bg2Looped_ = false;
      bg2CamStart_ = cam16_;
      bg2Period_ = panoramaPeriod();
      bg2Off_ = 0;
      for (uint32_t& m : bg2Fits_) m = 0;
      int best = 0;
      for (int off = 0; off < bg2Period_; off += 32) {
        int n = 0;
        for (int c = (abs >> 3); c < (abs >> 3) + 32; c++) {
          vramCol(c, v);
          if (!sky(v) && fits(v, c + off)) n++;
        }
        int dist = std::min(off, bg2Period_ - off), bestDist = std::min(bg2Off_, bg2Period_ - bg2Off_);
        if (n > best || (n == best && dist < bestDist)) {
          best = n;
          bg2Off_ = off;
        }
      }
      // The columns the area load wrote are the screen just matched, not
      // streamed ahead of it (the vote below would place those left of the
      // screen's middle 64 columns on: coming out of 1-2's end pipe they
      // outvoted the right offset, and the margins' hills were cut off).
      bg2Streamed_ = 0;
    }
    // (see bg2Static_)
    if (bg2Streamed_) bg2SeenStream_ = true;
    bg2Static_ = !bg2SeenStream_ && std::abs(cam16_ - bg2CamStart_) >= 160;  // (strips come every 128 px)
    // Left of the level's start (a castle maze loop takes the camera back past
    // it for a moment): nothing to go by, no vote either way. (Voting there
    // set 4-4's offset wrong and dropped the panorama, its walls showing
    // others in the margins.)
    if (abs < 0 && !bg2Fresh_) {
      bg2Streamed_ = 0;
      return;
    }
    // (After a castle maze loop the offset is known: see trackParser. The
    // castles' backdrop repeats so much that the vote drifted to another.)
    if (bg2Looped_) bg2Streamed_ = 0;
    if (bg2Streamed_ && bg2Period_ > 0) {
      // Columns the game just streamed (ahead of the screen, 8 at a time) fit
      // the page offset it reads the panorama at now; a repeating backdrop
      // fits others too, so go by the offset most of the last 8 streamed
      // columns fit (a castle maze loop leaves older columns on screen at
      // another).
      int c0 = (abs >> 3) + 16;
      for (int vc = 0; vc < 64; vc++) {
        if (!((bg2Streamed_ >> vc) & 1)) continue;
        int c = c0 + (((vc - c0) % 64 + 64) % 64);
        vramCol(c, v);
        if (sky(v)) continue;
        uint32_t mask = 0;
        for (int k = 0; k * 32 < bg2Period_ && k < 32; k++)
          if (fits(v, c + k * 32)) mask |= 1u << k;
        bg2Fits_[bg2FitPos_++ & 7] = mask;
      }
      int votes[32] = {}, cur = bg2Off_ / 32, best = cur, pages = (bg2Period_ + 31) / 32;
      for (uint32_t m : bg2Fits_)
        for (int k = 0; k < 32; k++) votes[k] += (m >> k) & 1;
      auto dist = [&](int a) { return std::min((a - cur + pages) % pages, (cur - a + pages) % pages); };
      for (int k = 0; k < pages; k++)
        if (votes[k] > votes[best] || (votes[k] == votes[best] && dist(k) < dist(best))) best = k;
      if (votes[best] >= 3 && votes[best] > votes[cur]) bg2Off_ = best * 32;
      bg2Streamed_ = 0;
    }
    match = abs >= 0 && bg2Period_ > 0;
    bool fitAll = match;
    int nonSky = 0;
    for (int i = 0; i < 6 && match; i++) {
      int col = (abs >> 3) + 2 + i * 5;
      vramCol(col, v);
      // After a castle maze loop the columns on screen were streamed from 512
      // px (64 columns) further on: either fits.
      if (!fits(v, col + bg2Off_) && !fits(v, col + bg2Off_ + 64) && !fits(v, col + bg2Off_ - 64)) {
        match = fitAll = false;
        break;
      }
      for (int r = 1; r < 31; r++) nonSky += v[r] != v[r - 1];
    }
    // Nothing distinctive to compare (plain sky on screen): no vote either
    // way. (A cloud area's BONUS picture, 2-1's, vanished from the left
    // margin as soon as it had left the 4:3 screen.)
    if (nonSky == 0) {
      plain = fitAll;
      match = false;
    }
  }
  if (bg2Fresh_) {
    // First look at a new area: VRAM is complete by now, so trust it at once
    // (voting from scratch would show the fallback during the fade-in).
    bg2Fresh_ = false;
    bg2Votes_ = match ? 10 : -10;
  } else if (plain) {
    return;
  }
  bg2Votes_ = std::clamp(bg2Votes_ + (match ? 1 : -1), -10, 10);
  if (bg2Votes_ >= 5) bg2Source_ = true;
  if (bg2Votes_ <= -5) bg2Source_ = false;
}

bool Smb1Engine::onExec(uint32_t pc) {
  if ((titleInput_ && (pc == titleInput_ || pc == titleInput_ + (lostLevels_ ? 0x28u : 0x2Au))) || (livesSet_ && pc == livesSet_))
    return titleExec(pc);
  if (coopOk_ && (pc == coopCall_ || pc == coopAfter_ || pc == coopDrawEnd_ || pc == coopEnemyAt_ || pc == coopLeftHi_ ||
                  (coopDeathSmall_ && pc == coopDeathSmall_) ||
                  pc == coopFire_ || pc == coopPack_ || pc == coopHammer_ || pc == coopStar_ ||
                  pc == coopVicDraw_ || pc == coopThanks_ ||
                  pc == coopEndStart_ || pc == coopWhirlRet_ || pc == coopLoop_ || pc == coopMenu_[0] ||
                  pc == coopMenu_[1] || pc == coopMenu_[2]))
    return coopExec(pc);
  CpuRegs& r = snes_.cpu.r;
  if (!r.mf) {
    // Just after SEC; SBC $42 in 16 bits: A is a position on the game's
    // screen, which the sprite X written next is drawn from.
    if (std::binary_search(rel16Sites_.begin(), rel16Sites_.end(), pc)) {
      ref16_ = (int16_t)r.a;
      ref16Seq_ = ++drawSeq_;
      ref16Writes_ = 8;
    }
    return false;
  }
  // Hooks only step in when they change the result; otherwise the game's own
  // instruction runs (with its normal timing). All hooked sites are 8-bit.
  auto loadA = [&](uint8_t v) {  // complete the hooked LDA abs
    r.a = (r.a & 0xff00) | v;
    r.n = v & 0x80;
    r.z = v == 0;
    r.pc += 3;
    return true;
  };
  if (pc == bbRead_) {
    // An object's collision check left of where the area began: the game
    // built nothing there (its map holds other columns), the view shows the
    // fill. Answer with the fill's metatile, so what walks or bounces there
    // does so on the floor that is shown. (The player can't get there.)
    if (!fillMtValid_ || extL_ <= 0 || !gameplay_) return false;
    int obj = r.x & 0xff;
    int x = (int8_t)ram(0x0078 + obj) * 256 + ram(0x0219 + obj) + snes_.peek(bbXAdder_ + ram(0x0004));
    int row = (r.y & 0xff) >> 4;
    if (x >= areaLeft_ || row >= 13) return false;
    uint8_t v = fillMt_[row];
    r.a = (r.a & 0xff00) | v;
    r.n = v & 0x80;
    r.z = v == 0;
    r.pc += 2;  // (LDA ($06),Y done)
    return true;
  }
  if (pc == enemyCall_) {
    std::memcpy(preFlags_, snes_.wram + 0x10, sizeof(preFlags_));
    if (!holdEnemy(r.x & 0xff)) {
      if (coopOk_) coopObjBefore(r.x & 0xff);  // (co-op: its run for the other player; see smb1coop.cpp)
      return false;
    }
    r.pc = (uint16_t)enemyRet_;  // (skip its update this frame)
    return true;
  }
  if (pc == enemyRet_) {
    if (coopOk_ && coopObjPhase_ && coopObjAfter()) return true;
    // Slots filled by that call (a spawn; a group fills several) are new.
    if (enemyRange_) {
      int fresh = 0, n = 0;  // (enemy slots 0-4: a group fills several)
      for (int k = 0; k < 10; k++)
        if (!preFlags_[k] && ram(0x10 + k)) {
          fresh_ |= 1 << k;
          if (k < 5) {
            fresh = k;
            n++;
          }
        }
      if (n == 1) placeLate(fresh);
    }
    return false;
  }
  if (pc == oamUpload_) {
    // The buffer written during the last game frame goes to the PPU now.
    // Where the game put the sprite on its screen (X < 256) it stays there.
    // A true X the 9-bit X doesn't repeat (mod 512) means the game moved the
    // sprite off its screen by bit 8 (to hide it, or folding a far position
    // into 9 bits, which the console hides alike): hidden in the wide view
    // too, unless the console shows part of it at its left edge.
    bool wide = extL_ != 0 || extR_ != 0;
    for (int i = 0; i < 128; i++) {
      int x9 = ram(0x0800 + 4 * i) | (((ram(0x0a00 + (i >> 2)) >> ((i & 3) * 2)) & 1) << 8);
      bool set = wide && oamXSet_[i];
      int x = oamX_[i];
      if (set && x9 < 256) {
        set = x == x9;
      } else if (set && ((x - x9) & 511) != 0) {
        set = x9 < 512 - 32;
        x = -kMaxExt - 64;
      }
      snes_.ppu.objTrueX[i] = (int16_t)x;
      snes_.ppu.objTrueXSet[i] = set;
    }
    if (coopOk_) coopLatch();  // (co-op: who is in which entries, see smb1coop.cpp)
    return false;
  }
  if (pc == springSlot_) {
    // The wide view keeps enemies longer, so enemy slots 0-4 can all be taken
    // as a jumpspring is built: it then went to slot 5, which the flagpole's
    // flag takes a moment later (2-1: the spring vanished before its wall,
    // the player stuck on its tiles, no way on). Free the furthest left enemy
    // behind the 4:3 screen (one the 4:3 game has removed already), cleared
    // as EraseEnemyObject (03:CE00) does.
    if (!enemyRange_ || (r.x & 0xff) < 5) return false;
    int best = -1, bestX = 0;
    for (int k = 0; k < 5; k++) {
      if (!ram(0x10 + k)) continue;
      int ex = (int8_t)ram(0x79 + k) * 256 + ram(0x021a + k);
      if (ex < cam16_ && (best < 0 || ex < bestX)) {
        best = k;
        bestX = ex;
      }
    }
    if (best < 0) return false;
    for (uint16_t a : {0x10, 0x1c, 0x29, 0x0110, 0x012e, 0x0792, 0x03a2}) snes_.wram[a + best] = 0;
    snes_.wram[0x0257 + best] = 0x20;
    r.x = (uint16_t)best;
    return false;
  }
  if (pc == groupPlaced_) {
    // Start the group at the wide view's edge instead ($02/$03 = page/X).
    if (enemyRange_ && extR_ > 0) {
      int x = (ram(0x0002) << 8 | ram(0x0003)) + extR_;
      // The console places a group at its screen edge as it comes in range
      // (its data column, in the slot being filled, less $30): so does this
      // edge. One that waited for free slots (the wide view keeps enemies
      // longer), or came in range as an area loaded mid-level (a pipe exit),
      // lands wherever the edge is by then, maybe in a block (5-1's stairs):
      // it moves on past built solid columns, or else back towards the 4:3
      // game's edge. Where it has to move, the obstacle may go on into
      // columns not built yet, so it only lands on built ones (1-1's coin
      // room exit put the last Goombas past the pipe into the end staircase,
      // built later). Members in unbuilt columns at the edge itself wait for
      // the level under them (see holdEnemy).
      int obj = ram(0x009e);
      int col = (int8_t)ram(0x0079 + obj) * 256 + ram(0x021a + obj);
      if (obj < 10 && x > col - 0x30 + 16) {
        int y = ram(0x0000) + 8;  // (the group's Y, a member's middle)
        // (Members: 2, or 3 when bit 0 of the group type on the stack is set,
        // as the game counts them next.)
        int members = 2 + (snes_.peek((r.sp + 1) & 0xffff) & 1);
        auto clearAt = [&](int gx, bool built) {
          for (int k = 0; k < members; k++) {
            int mx = gx + 24 * k + 8;
            if ((built && mx + 8 > parserPos()) || solidAt(mx, y)) return false;
          }
          return true;
        };
        int to = clearAt(x, false) ? x : -1;
        for (int d = 8; d <= 48 && to < 0; d += 8)
          if (clearAt(x + d, true)) to = x + d;
        for (int d = 8; d <= extR_ && to < 0; d += 8)
          if (clearAt(x - d, true)) to = x - d;
        if (to >= 0) x = to;
      }
      snes_.wram[0x0003] = x & 0xff;
      snes_.wram[0x0002] = (x >> 8) & 0xff;
    }
    return false;
  }
  if (pc == chkPOffscr_) {
    // The player may walk into the view's left margin (the level just
    // passed): the game's wall at its screen edge moves out to there.
    // (A = his off-screen bits; cleared, the game holds nothing back.)
    // (Co-op, the screen held for the other player: past its right edge, as
    // far as the view's; he stopped at 4:3's edge, short of the view's. Far
    // into the right margin the bits are all set, the left edge's too:
    // which edge is his position's.)
    const int pxAll = (int8_t)ram(0x0078) * 256 + ram(0x0219);
    // (... and the walk to the castle: the screen held for the other as he
    // came to the flag, he took the pole at 4:3's edge and couldn't step off
    // it; the game frees the screen once he's down.)
    if (coopActive() && gameplay_ && ram(0x000F) == 5 && pxAll - cam16_ >= 128) {
      r.a &= 0xff00;
      return false;
    }
    if (coopActive() && gameplay_ && pxAll - cam16_ >= 128) {
      // (... and after it, the screen not held by the game itself: he stays
      // where he walked to, the screen catching up (see scrollAmt_), not
      // put back at 4:3's edge at once, 73 px in a frame, over a pit or into
      // a wall, and kept there for the area.)
      if ((coopEdge_ || ram(0x0723) == 0) && extR_ > 0 && (r.a & 0xa0)) {
        const int right = cam16_ + 256 + extR_ - 16;
        if (pxAll > right) {
          snes_.wram[0x0219] = right & 0xff;
          snes_.wram[0x0078] = (right >> 8) & 0xff;
          if (ram(0x000c) == 0x01) snes_.wram[0x005d] = 0;
        }
        r.a &= 0xff00;
      } else if (r.a & 0x80) {
        r.a = (r.a & 0xff00) | 0x20;  // (the game's right edge, not its left)
      }
      return false;
    }
    if (extL_ <= 0 || !gameplay_ || !(r.a & 0x80)) return false;
    int px = (int8_t)ram(0x0078) * 256 + ram(0x0219), lim = playerLeftLimit();
    // (not one fallen below the floor, into a pit: carried over it he landed
    // inside the ground beyond)
    if (px < lim && ram(0x00BB) == 1 && ram(0x0237) > 0xB0 && ram(0x0028) == 2) {
      r.a &= 0xff00;
      return false;
    }
    if (px < lim) {
      snes_.wram[0x0219] = lim & 0xff;
      snes_.wram[0x0078] = (lim >> 8) & 0xff;
      if (ram(0x000c) != 0x01) snes_.wram[0x005d] = 0;  // (as the game: speed kept only walking away)
    }
    r.a &= 0xff00;
    return false;
  }
  if (pc == boundBox_ && boxTable_) {
    // An object's box, in level coordinates (X = its box index: the SprObject
    // index, but for the misc objects (hammers; Y = 6, their Rel_XPos) 4
    // short of it, their boxes following the fireballs').
    int x = r.x & 0xff;
    if (x < kBoxes) {
      int ctrl = ram(0x048f + x) * 4;
      int o = (r.y & 0xff) == 6 ? x + kMiscObj - 13 : x;
      int ox = (int8_t)ram(0x0078 + o) * 256 + ram(0x0219 + o);
      if (x == 0) boxAfterSwap_ = false;  // (the game's player's own box again: see swapRoles)
      boxL_[x] = ox + snes_.peek(boxTable_ + ctrl);
      boxR_[x] = ox + snes_.peek(boxTable_ + ctrl + 2);
      boxFrame_[x] = snes_.frameCount();
    }
    return false;
  }
  if (pc == fireballOff_) {
    // A fireball in the left margin (thrown from there) is on screen: the
    // game would erase it at once. With the enemies active in full view, one
    // anywhere in the view is (it was erased at the 4:3 screen's right edge,
    // short of the enemies in the margin).
    int x = r.x & 0xff;
    uint8_t v = ram(0x03d2);
    if (!(extL_ || extR_) || x > 1 || !v) return false;
    int rel = (int8_t)ram(0x0078 + kFireballObj + x) * 256 + ram(0x0219 + kFireballObj + x) - cam16_;
    if (enemyRange_ && rel > -extL_ - 8 && rel < 256 + extR_) return loadA(0);
    if (extL_ <= 0 || !(v & 0x0f) || rel >= 0 || rel < -extL_ - 8) return false;
    return loadA(v & 0xf0);
  }
  if (pc == hammerThrow_) {
    // A Hammer Bro in the view throws (enemies active in full view).
    int x = r.x & 0xff;
    if (!enemyRange_ || !(extL_ || extR_) || x > 9 || !(ram(0x03d1) & 0x0c)) return false;
    int rel = (int8_t)ram(0x0079 + x) * 256 + ram(0x021a + x) - cam16_;
    if (rel <= -extL_ - 16 || rel >= 256 + extR_) return false;
    return loadA(0);
  }
  if ((pc == hammerGone_ || pc == hammerColl_) && (extL_ || extR_)) {
    // A hammer in the view stays and hits: with the enemies active in full
    // view; or in the left margin with the player there (it vanished on its
    // way to him).
    int k = ram(0x009e);
    if (k > 8) return false;
    int o = kMiscObj + k;
    int rel = (int8_t)ram(0x0078 + o) * 256 + ram(0x0219 + o) - cam16_;
    bool inView = rel > -extL_ - 8 && rel < 256 + extR_;
    bool shown = inView && (enemyRange_ || (rel < 0 && (int8_t)ram(0x0078) * 256 + ram(0x0219) < cam16_ + 16));
    // (Only its X decides: one off the screen above or below goes as before,
    // and has no touch: kept, a hammer fallen 256 px below hit by its Y's
    // low byte.)
    const uint8_t vert = ram(0x03d6) & 0xf0;
    if (!shown || (pc == hammerGone_ && vert)) return false;
    if (pc == hammerGone_) {
      r.pc = (uint16_t)hammerKeep_;
      return true;
    }
    // (ORA Misc_OffscreenBits with its vertical bits only)
    r.a = (uint16_t)(r.a | vert);
    r.n = r.a & 0x80;
    r.z = (r.a & 0xff) == 0;
    r.pc += 3;
    return true;
  }
  if (pc == enemyBoxOff_) {
    // Active in full view: an enemy inside the wide view has a box (it can
    // hit and be hit there); the game gives none to one off its screen.
    if (enemyRange_ && (r.a & 0xff)) {
      int x = r.x & 0xff;
      int rel = (int8_t)ram(0x0079 + x) * 256 + ram(0x021a + x) - cam16_;
      // (its horizontal bits only: one above or below the screen still has
      // none, or a Podoboo far below met the player by its Y's low byte)
      // (All ten of All-Stars' enemy slots, not the NES's first six: a
      // power-up in slot 9 past the screen's right edge had no box, and in
      // co-op Luigi held out there couldn't take it, nor be met by an enemy
      // in slots 6-9.)
      if (x < 10 && rel > -extL_ - 16 && rel < 256 + extR_) r.a &= 0xfff0;
    }
    return false;
  }
  if (pc == collide_ && (extL_ || extR_ || (coop_ && (coopEnemy_ >= 0 || boxAfterSwap_)))) {
    // Two boxes (offsets X and Y): when either object is beyond the game's
    // screen its 8-bit box has wrapped or been clamped to the edge (Mario in
    // the left margin met enemies at the screen's edge): compare in 16 bits.
    int bx = r.x & 0xff, by = r.y & 0xff, a = bx >> 2, b = by >> 2;
    // (An object later in the update order still has last frame's box, as
    // the game's own test uses it.)
    uint32_t f = snes_.frameCount();
    auto miss = [&]() {
      r.c = false;
      uint16_t sp = r.sp;
      uint16_t ret = snes_.peek((uint16_t)(sp + 1)) | snes_.peek((uint16_t)(sp + 2)) << 8;
      r.sp = (uint16_t)(sp + 2);
      r.pc = (uint16_t)(ret + 1);  // RTS
      return true;
    };
    // (Luigi meets the enemies after their run (see coopEnemyAt_): one that
    // appeared in it, a Bullet Bill fired that frame, has no box yet, and the
    // game's test used a box left in its slot by an object long gone: it hit
    // him there, nowhere near the bullet.)
    if (coop_ && boxAfterSwap_ && bx == 0 && coopEnemy_ < 0) {  // (see swapRoles)
      snes_.wram[0x0006] = (uint8_t)by;
      return miss();
    }
    if (coop_ && coopEnemy_ >= 0 && bx == 0 && !(by & 3) && b > 0 && b < kBoxes && f - boxFrame_[b] > 1) {
      snes_.wram[0x0006] = (uint8_t)by;  // (as the routine leaves it)
      return miss();
    }
    if (!(extL_ || extR_)) return false;
    if ((bx & 3) || (by & 3) || a >= kBoxes || b >= kBoxes || f - boxFrame_[a] > 1 || f - boxFrame_[b] > 1) return false;
    auto inside = [&](int k) { return boxL_[k] >= cam16_ && boxR_[k] < cam16_ + 256; };
    // (Otherwise the game's own test: with original timing only the player
    // goes beyond its screen, into the left margin.)
    // (the player's or his fireballs', SprObjects 0 and kFireballObj, +1)
    // (In co-op one can be past the right edge too, held there while the other
    // holds the screen: a Goomba 245 px away hurt him.)
    auto mineOut = [&](int k) {
      return (k == 0 || k == kFireballObj || k == kFireballObj + 1) &&
             ((extL_ > 0 && boxL_[k] < cam16_ + 8) || (extR_ > 0 && boxR_[k] >= cam16_ + 256));
    };
    // (+8: his box starts a few px right of where he is; just in the margin
    // with his box at the edge, his 8-bit screen X had wrapped, and a hammer
    // 242 px away at the right hit him.)
    if (!mineOut(a) && !mineOut(b) && !(enemyRange_ && (!inside(a) || !inside(b)))) return false;
    uint8_t* w = snes_.wram;
    auto unset = [&](int o) { return w[0x0f9c + o] == 0xff && w[0x0f9d + o] == 0xff && w[0x0f9e + o] == 0xff; };
    bool hit = !unset(bx) && !unset(by) && boxL_[a] <= boxR_[b] && boxL_[b] <= boxR_[a] &&
               w[0x0f9d + bx] <= w[0x0f9f + by] && w[0x0f9d + by] <= w[0x0f9f + bx];
    w[0x0006] = (uint8_t)by;  // (as the routine leaves it)
    r.c = hit;
    uint16_t sp = r.sp;
    uint16_t ret = snes_.peek((uint16_t)(sp + 1)) | snes_.peek((uint16_t)(sp + 2)) << 8;
    r.sp = (uint16_t)(sp + 2);
    r.pc = (uint16_t)(ret + 1);  // RTS
    return true;
  }
  if (pc == scrollAmt_) {
    // Co-op: the screen scrolls as far as its player moves, never more: held
    // for the other, the one in front went on, and stayed that far ahead (at
    // the screen's right edge, a warp zone's text never came, its pipes not
    // reached). Well right of the middle, a pixel more a frame (moving or
    // not), till he's back near it.
    if (coopActive() && gameplay_ && ram(0x000F) == 8 && ram(0x0755) >= 0x78 && (int8_t)ram(0x06FF) >= 0 &&
        ram(0x06FF) < 8)
      snes_.wram[0x06FF]++;
    return false;
  }
  if (pc == posForScroll_) {
    // In the left margin his 8-bit screen position reads as right of the
    // middle: the screen would scroll on as he walked right. There he is at
    // the far left (just stored, before its copy to $05 for drawing).
    if (extL_ > 0 && (int8_t)ram(0x0078) * 256 + ram(0x0219) < cam16_) snes_.wram[0x0755] = 0;
    // (... and in the right margin (co-op: gone on while the screen waited
    // for the other) it read as the left half: the screen never followed.
    // There he is at the far right: it catches up, see scrollAmt_.)
    if (extR_ > 0 && (int8_t)ram(0x0078) * 256 + ram(0x0219) >= cam16_ + 256) snes_.wram[0x0755] = 0xff;
    return false;
  }
  if (pc == firebarLda_) {
    // A firebar whose pivot is in the wide view is drawn (its relative
    // position is only worked out after this test: the enemy's own, X).
    firebarDraw_ = true;  // (its flames reach further from it than an enemy's sprites)
    int x = r.x & 0xff;
    uint8_t v = ram(0x03d1);
    if (!(extL_ || extR_) || x > 5 || !(v & 0x0f)) return false;
    int rel = (int8_t)ram(0x0079 + x) * 256 + ram(0x021a + x) - camera();
    if (!(rel > 256 - 16 && rel < 256 + extR_) && !(extL_ > 0 && rel < 0 && rel >= -extL_ - 16)) return false;
    // (Original timing: as on the console while any of its pivot, 8 px, is on
    // the 4:3 screen.)
    if (!enemyRange_ && rel > -8 && rel < 256) return false;
    firebarPivot_ = rel;
    if (!enemyRange_) {
      // Original timing: off the 4:3 screen the bar neither turns nor hurts,
      // as on the console (the game skips both while TimerControl is set:
      // set for this bar only, restored at its RTS).
      firebarTimer_ = ram(0x0747);
      snes_.wram[0x0747] = firebarTimer_ | 1;
      firebarHeld_ = true;
    }
    return loadA(v & 0xf0);
  }
  if (pc == firebarColl_) {
    // The flame meets the player by their 8-bit screen positions: in the wide
    // view one 256 px away met him too (Mario killed at 1-4's start by a bar
    // in the right margin). Their true distance decides; the 8-bit test then
    // sees the player where he truly is.
    const bool valid = flameValid_;
    flameValid_ = false;
    if (!(extL_ || extR_)) return false;
    int fx = flameX_;
    if (!valid) {
      // (The first flame, at the pivot, doesn't pass the wrap test (see
      // firebarWrap_): its X ($06) placed by the pivot the same way. A bar
      // in the right margin hurt the player 256 px left of it by it.)
      if (!firebarHeld_ && !relValid_) return false;
      const int pivot = firebarHeld_ ? firebarPivot_ : curRel_, x8 = ram(0x0006);
      fx = x8;
      for (int c : {x8 - 256, x8 + 256})
        if (std::abs(c - pivot - 8) < std::abs(fx - pivot - 8)) fx = c;
    }
    const int player = (int8_t)ram(0x0078) * 256 + ram(0x0219) - camera();
    if (std::abs(player + 4 - fx) >= 8) {
      r.pc = (uint16_t)firebarCollEnd_;
      return true;
    }
    relHold_ = ram(0x03ad);
    relHeld_ = true;
    snes_.wram[0x03ad] = (uint8_t)player;
    return false;
  }
  if (pc == firebarCollEnd_) {
    if (relHeld_) snes_.wram[0x03ad] = relHold_;
    relHeld_ = false;
    return false;
  }
  if (pc == firebarEnd_) {
    if (firebarHeld_) snes_.wram[0x0747] = firebarTimer_;
    firebarHeld_ = false;
    return false;
  }
  if (pc == firebarWrap_) {
    // In the wide view the flame is wherever its true distance puts it (the
    // sprite X fix-up places it there): only that distance counts. (With
    // original timing the flames on the 4:3 screen stay hidden while the
    // pivot is off it, as on the console.)
    // A flame beyond the display is hidden as the game hides one past its
    // screen (the sprite X fix-up can't place it: it would land inside).
    if (!(extL_ || extR_)) return false;
    int pivot = firebarHeld_ ? firebarPivot_ : curRel_;
    int x8 = ram(0x0006), best = x8;  // (the flame's X, just stored)
    for (int c : {x8 - 256, x8 + 256})
      if (std::abs(c - pivot - 8) < std::abs(best - pivot - 8)) best = c;
    flameX_ = best;  // (for its collision: see firebarColl_)
    flameValid_ = true;
    if (!enemyRange_ && !firebarHeld_) return false;
    bool shown = firebarHeld_ ? (best >= 256 && best < 256 + extR_ + 8) || (best < 0 && best >= -extL_ - 8)
                              : best >= -extL_ - 8 && best < 256 + extR_ + 8;
    int d = r.a & 0xff;
    r.a = (r.a & 0xff00) | (shown ? std::min(d, 256 - d) : 0xff);
    return false;
  }
  if (pc == blinkLda_) {
    // Invincibility cheat keeps the injury timer running; don't blink for it.
    // (Nor for a co-op brother left standing: see coopIdleSafe. The other's
    // drawing is in his turn.)
    if (!(cheats_ & kCheatNoDamage)) {
      const int id = coop_ && (coopMove_ == 1 || coopDrawPhase_ == 1) && cur_ >= 0 ? cur_ : live_;
      if (!coop_ || !pl_[id].idleSafe || ram(0x07AE) > 2) return false;
    }
    return loadA(0);
  }
  for (const auto& s : drawSites_) {
    if (pc != s.pc) continue;
    // The game computes off-screen bits for the 4:3 screen; logic keeps them
    // (object lifetimes, behaviour and collisions stay exactly as on the
    // console). Only the drawing code sees the object as visible when it
    // is inside the wide view, so its sprites are not hidden there.
    // Objects entirely on the 4:3 screen are left alone, so it looks exactly
    // as on the console.
    bool inRightMargin = curRel_ > 256 - 16 && curRel_ < 256 + extR_;
    bool inLeftMargin = curRel_ < 0 && curRel_ >= -extL_;
    if (!(extL_ || extR_) || !relValid_ || relAddr_ != s.relAddr || !(inRightMargin || inLeftMargin))
      return false;  // original bits
    uint16_t bitsAddr = snes_.peek16(pc + 1);
    uint8_t v = ram(bitsAddr);
    if (!(v & 0x0f)) return false;  // already on screen horizontally
    return loadA(v & 0xf0);          // horizontally visible (vertical bits unchanged)
  }
  return false;
}

void Smb1Engine::onWrite(uint32_t adr, uint8_t v, uint32_t pc) {
  uint16_t a = adr & 0xffff;
  if (a >= 0x03ad && a <= 0x03b3) {
    // GetObjRelativePosition: X = object index into the SprObject arrays.
    int x = snes_.cpu.r.x & 0xff;
    relAddr_ = a;
    firebarRel_ = firebarDraw_;
    firebarDraw_ = false;
    if (x < 0x30) {
      int objX = (int8_t)ram(0x0078 + x) * 256 + ram(0x0219 + x);  // (page $FF: left of page 0)
      int rel = objX - camera();
      // (A few px out when the camera moved since: lifts on the frame the
      // scroll moves them. The written value is the one drawn from.)
      int d = (int8_t)(uint8_t)(rel - v);
      // (A bumped block's second position, $03B2, is its brick's pieces',
      // worked out after the block's own and before the block is drawn: with
      // no brick broken, a position left over far away (the block in the left
      // margin was drawn by it, hidden 256 px off). The block's stays.)
      if (a == 0x03b2 && relByFrame_[4] == snes_.frameCount() + 1 && std::abs(rel - d - relBy_[4]) > 64) return;
      if (std::abs(d) <= 8) {
        curRel_ = rel - d;
        curWrites_ = 24;
        relValid_ = true;
        relSeq_ = ++drawSeq_;
        relBy_[a - 0x03ad] = curRel_;
        relByFrame_[a - 0x03ad] = snes_.frameCount() + 1;
        relV_[a - 0x03ad] = v;
        return;
      }
    }
    // A position worked out this frame, moved a few px by the code about to
    // draw from it, with another index in X: a fireball's explosion, 4 px left
    // (03:F4B6, X its fireball number, not its object's). Its true place moves
    // with it (else, past the 4:3 screen's right edge, the burst was drawn
    // 256 px back, behind Mario).
    if (relByFrame_[a - 0x03ad] == snes_.frameCount() + 1) {
      const int d = (int8_t)(uint8_t)(v - relV_[a - 0x03ad]);
      if (std::abs(d) <= 8) {
        curRel_ = relBy_[a - 0x03ad] + d;
        curWrites_ = 24;
        relValid_ = true;
        relSeq_ = ++drawSeq_;
        relBy_[a - 0x03ad] = curRel_;
        relV_[a - 0x03ad] = v;
        return;
      }
    }
    curWrites_ = 0;
    relValid_ = false;
    return;
  }
  if (a >= 0x0c00 && a < 0x0e00) {
    if (((pc & 0x7fffff) == bowserBig_[0] || (pc & 0x7fffff) == bowserBig_[1]) && (extL_ || extR_))
      snes_.wram[a] |= 2;  // (16x16, see kSigBowserHi)
    // The game set this sprite's high bits itself (its 16-bit drawing code,
    // e.g. score pop-ups): leave that sprite's X bit 8 to it this frame.
    // Unless the X bit was just chosen here for this sprite: firebar flames
    // get their high byte written after their X (bit 8 set only to hide a
    // flame off the bottom), which would undo it.
    // (Not the clearing of all of them as a game frame starts, STZ, which
    // can come before the display frame does.)
    uint8_t op = snes_.peek(pc);
    if ((a & 3) == 0 && op != 0x9e && op != 0x9c && op != 0x74 && op != 0x64) {
      int slot = (a - 0x0c00) >> 2;
      if (ourHiFrame_[slot] == snes_.frameCount()) {
        if (ourHi_[slot]) snes_.wram[0x0c00 + (a - 0x0c00)] |= 1;
      } else {
        gameHiFrame_[slot] = snes_.frameCount();
      }
    }
    return;
  }
  if (a >= 0x0800 && a < 0x0a00 && (a & 3) == 0) {
    int slot = (a - 0x0800) >> 2;
    oamXSet_[slot] = 0;  // (unknown unless worked out below)
    bool wide = extL_ != 0 || extR_ != 0;
    // Drawn by the game's 16-bit code from the position it just worked out
    // (it sets bit 8 itself), and not by the object last placed below:
    // the X congruent to v nearest that position.
    auto from16 = [&] {
      if (ref16Writes_ <= 0 || ref16Seq_ <= relSeq_ || !wide) return;
      ref16Writes_--;
      int best = ref16_ + (int8_t)(uint8_t)(v - ref16_);
      if (std::abs(best - ref16_) < 48) {
        oamX_[slot] = (int16_t)best;
        oamXSet_[slot] = 1;
      }
    };
    // (A coin out of a block (a misc object, Misc_Rel_XPos $03B3): the
    // All-Stars code sets its X bit 8 itself, from the 4:3 screen's view;
    // in the left margin it showed 256 px to the right. Its own position.)
    if (gameHiFrame_[slot] == snes_.frameCount() && wide && relAddr_ == 0x03b3 && relValid_ && curWrites_ > 0) {
      curWrites_--;
      int center = curRel_ + 8, best = v;
      for (int c : {v - 256, v + 256})
        if (std::abs(c - center) < std::abs(best - center)) best = c;
      if (std::abs(best - center) < 48) {
        uint8_t& hi = snes_.wram[0x0c00 + (a - 0x0800)];
        hi = best != v ? (hi | 1) : (hi & ~1);
        oamX_[slot] = (int16_t)best;
        oamXSet_[slot] = 1;
        ourHiFrame_[slot] = snes_.frameCount();
        ourHi_[slot] = best != v;
      }
      return;
    }
    if (gameHiFrame_[slot] == snes_.frameCount()) return from16();
    // X bit 8 lives in bit 0 of $0C00+slot (packed into the OAM high table
    // at $0A00 later in the frame). When the object being drawn is known, the
    // bit is set or cleared here: a bit left over from the slot's previous
    // user would otherwise move the sprite 256 px. Sprites the game places
    // with its own 16-bit code keep the bit it sets.
    uint8_t& hi = snes_.wram[0x0c00 + (a - 0x0800)];
    if (!(curWrites_ > 0 && wide)) return from16();
    {
      curWrites_--;
      // Choose the 9-bit X nearest the object that is being drawn.
      int center = curRel_ + 8;
      int best = v, bestD = std::abs(v - center);
      for (int k = -1; k <= 1; k += 2) {
        int c = v + k * 256;
        int d = std::abs(c - center);
        if (d < bestD) {
          best = c;
          bestD = d;
        }
      }
      int reach = firebarRel_ ? 104 : 48;  // (a long firebar's flames: 12 x 8 px)
      bool high = best != v && bestD < reach && best >= -extL_ - 32 && best < 256 + extR_ + 32;
      hi = high ? (hi | 1) : (hi & ~1);
      if (bestD < reach) {
        // Its true place, even where 9 bits can't tell (beyond the right
        // edge the game still sets bit 8 itself, as hardware hides it).
        oamX_[slot] = (int16_t)best;
        oamXSet_[slot] = 1;
      } else {
        from16();
      }
      ourHiFrame_[slot] = snes_.frameCount();
      ourHi_[slot] = high;
    }
  }
}

// A solid metatile at a level position, in the game's collision map (only
// what the area parser has built counts; other columns aren't known yet).
bool Smb1Engine::solidAt(int x, int y) const {
  if (x + 8 > parserPos() || y < 0x20) return false;
  int col = x >> 4, row = (y - 0x20) >> 4;
  if (row >= 13) return false;
  uint8_t mt = ram((((col >> 4) & 1) ? 0x05d0 : 0x0500) + row * 16 + (col & 15));
  return mt >= solidFrom_[mt >> 6];
}

// A walker the wide range spawned past the built level (its collision map
// there is still the level 512 px back: it would fall through, or into what
// is built around it later) skips its update until the level under it is
// built, or it comes into view (no enemy in view is held still). Then it runs
// from there on, as one the 4:3 game spawned.
bool Smb1Engine::holdEnemy(int x) {
  if (!enemyRange_) {
    fresh_ = 0;
    return false;
  }
  if (x < 5 && leavesBuilt(x)) return true;
  if (x >= 10 || !(fresh_ & (1 << x))) return false;
  uint8_t flag = ram(0x10 + x), id = ram(0x1c + x);
  // (Koopas, Buzzy Beetles, Hammer Bros, Goombas, jumping Paratroopas, Spinies;
  // not $04, the Lost Levels' upside-down Piranha Plant.)
  bool walker = (id <= 0x06 && id != 0x04) || id == 0x0e || id == 0x12;
  int ex = (int8_t)ram(0x79 + x) * 256 + ram(0x021a + x);
  if (flag == 0 || (flag & 0x80) || !walker || ex + 16 <= parserPos() || ex - camera() < 256 + extR_) {
    fresh_ &= ~(1 << x);
    return false;
  }
  return true;
}

// A kicked shell heading right out of view, past the built level: the wide
// range keeps it further out than the 4:3 game would (which lets it go 72 px
// past the screen, all built), into columns whose collision map is still the
// level 512 px back, where it could meet walls that aren't there and come
// back into view. It goes, as in the 4:3 game. (Not walkers: one turned by a
// real wall off the view comes back, as on the console.)
bool Smb1Engine::leavesBuilt(int x) {
  uint8_t flag = ram(0x10 + x), id = ram(0x1c + x);
  bool shell = id <= 0x03 && (ram(0x29 + x) & 0x80);  // (Koopas and Buzzy Beetles, kicked)
  if (!flag || (flag & 0x80) || !shell || (int8_t)ram(0x5e + x) <= 0) return false;
  int ex = (int8_t)ram(0x79 + x) * 256 + ram(0x021a + x);
  if (ex + 16 <= parserPos() || ex - camera() < 256 + extR_) return false;
  for (uint16_t a : {0x10, 0x1c, 0x29, 0x0110, 0x012e, 0x0792, 0x03a2}) snes_.wram[a + x] = 0;
  snes_.wram[0x0257 + x] = 0x20;  // (as EraseEnemyObject)
  return true;
}

// The wide view keeps enemies longer (behind the player), so the five enemy
// slots can be full when the next one's spawn point passes; it then starts
// the moment a slot frees, at its own position, by then inside the visible
// margin (5-1's Koopa, 2-2's Blooper popped in). A walker or swimmer that
// starts there begins just beyond the view's edge instead, on built ground
// that is clear (as a late group does), and comes in from off the display.
// Objects tied to a place (cannons, pipes, firebars, platforms, frenzies,
// Hammer Bros on their bricks) and hopping Paratroopas start where the game
// puts them.
void Smb1Engine::placeLate(int k) {
  // (Not as an area appears: what is in view then is there from the start.)
  if (k >= 5 || extR_ <= 0 || areaFrames_ < 45) return;
  uint8_t id = ram(0x1c + k);
  // (Not the hopping Paratroopa, $0E: its arcs from another start met other
  // things, LL D-2's hanging pipe.)
  bool walker = (id <= 0x06 && id != 0x04 && id != 0x05) || id == 0x12;
  bool swimmer = id == 0x07 || id == 0x0a || id == 0x0b;
  if (!walker && !swimmer) return;
  int ex = (int8_t)ram(0x79 + k) * 256 + ram(0x021a + k), edge = cam16_ + 256 + extR_;
  if (ex < cam16_ + 256 - 8 || ex >= edge - 8) return;  // (only in the right margin's view)
  int y = ram(0x0238 + k) + 8;
  bool ground = solidAt(ex + 8, y + 16);  // (one on ground stays on ground: a red Koopa on its ledge)
  for (int x = edge; x <= edge + 48; x += 8) {  // (from just off the display)
    if (walker && (x + 16 > parserPos() || solidAt(x + 8, y) || (ground && !solidAt(x + 8, y + 16)))) continue;
    snes_.wram[0x021a + k] = x & 0xff;
    snes_.wram[0x0079 + k] = (x >> 8) & 0xff;
    return;
  }
}

// A jumpspring set up in slot 5 (no enemy slot was free, and none behind the
// 4:3 screen to give up: LL 8-3's Lakitu kept an extra Spiny in the wide
// view) would go when a power-up or the flagpole's flag takes that slot. It
// moves to an enemy slot as soon as one is free (between frames, not while
// the player is at it), with what its set-up wrote (see kSigSpring).
void Smb1Engine::rescueSpring() {
  constexpr int kFrom = 5;
  if (!ram(0x10 + kFrom) || ram(0x1c + kFrom) != 0x32) return;
  int sx = (int8_t)ram(0x79 + kFrom) * 256 + ram(0x021a + kFrom);
  int px = (int8_t)ram(0x0078) * 256 + ram(0x0219);
  if (std::abs(px - sx) < 48) return;
  for (int k = 0; k < 5; k++) {
    if (ram(0x10 + k)) continue;
    for (uint16_t a : {0x10, 0x1c, 0x79, 0x021a, 0x0238, 0x5e, 0xbc, 0x29}) snes_.wram[a + k] = ram(a + kFrom);
    for (uint16_t a : {0x10, 0x1c, 0x29, 0x0110, 0x012e, 0x0792, 0x03a2}) snes_.wram[a + kFrom] = 0;
    snes_.wram[0x0257 + kFrom] = 0x20;  // (as EraseEnemyObject)
    return;
  }
}

// How far left the player may go: the view's edge, but not before the area
// began, nor past the 32 columns of collision map behind the area parser.
int Smb1Engine::playerLeftLimit() const {
  return std::max({cam16_ - extL_, areaLeft_, parserPos() - 512 + 16});
}

void Smb1Engine::beginLine(Ppu& ppu, int row) {
  if (row == 0) hudFrame();  // (co-op's MARIO & LUIGI: over what the game wrote in this vblank)
  if (extL_ == 0 && extR_ == 0) return;
  if (row == 0) {
    hudHofs_ = ppu.bgHofs(2);
    hudVofs_ = ppu.bgVofs(2);
  }
  // (The cloud layer's scroll can take over a line or two before the end of
  // the status rows: leave those lines alone.)
  if (hudAnchor_ && row < kHudLines && ppu.bgHofs(2) == hudHofs_ && ppu.bgVofs(2) == hudVofs_) {
    // Split the status line in two and pin each half to a screen edge.
    PpuLayerRemap& rm = ppu.layerRemap[2];
    rm.count = 2;
    rm.seg[0] = {(int16_t)-extL_, (int16_t)(128 - extL_), 0, 0};
    rm.seg[1] = {(int16_t)(128 + extR_), (int16_t)(256 + extR_), 128, 0};
  }
}

int Smb1Engine::fetchWideTile(Ppu& ppu, int layer, int tx, int ty, int screenX, uint16_t* entry) {
  if (card_) {
    // The card between levels: its grey is BG1's, filling the game's screen
    // (the rest of the maps hold leftovers of the last level), carried on
    // from the screen's edge.
    if (layer != 0) return PpuHooks::kWideBlank;
    int edge = screenX < 0 ? 0 : 255;
    *entry = ppu.tileEntry(0, ((ppu.bgHofs(0) + edge) >> 3) & 63, ty);
    return PpuHooks::kWideEntry;
  }
  if (layer == 0) {
    // Level layer: only show columns the area parser has actually written.
    int want = (cam16_ + screenX) >> 3;
    int wc = want + (((tx - want) % 64 + 64 + 32) % 64 - 32);
    if (fillValid_ && (wc * 8 < areaLeft_ || (padRoom_ && wc * 8 >= roomRight_))) {
      *entry = fill_[wc & 1][ty & 31];
      return PpuHooks::kWideEntry;
    }
    // (Left of 0 nothing is built, unless a castle maze loop moved the area
    // back past it: then its columns are what VRAM and the copies hold.)
    if (wc < 0 && areaLeft_ >= 0) return PpuHooks::kWideBlank;
    // Bowser beaten, or an open room (see victoryL_, openRoom_), the view
    // centred: its right edge can lie a few px past the last column the
    // parser built. The room's last column goes on.
    int built = parserPos() >> 3;
    if ((victoryL_ >= 0 || openRoom_) && wc >= built && colEntry(wc - 2 * ((wc - built) / 2 + 1), ty, entry))
      return PpuHooks::kWideEntry;
    if (colWorld_[tx & 63] == wc) return PpuHooks::kWideVram;
    // Older than the VRAM ring: use the uploaded copy.
    int s = wc & (kShadowCols - 1);
    if (shadowCol_[s] != wc || wc >= (parserPos() >> 3) - 60) {
      // (8-4's walk to the princess uploads no columns: its own screen shows
      // what the VRAM ring still holds as the camera moves on, and so does the
      // margin ahead.)
      return victoryL_ >= 0 && wc * 8 >= cam16_ + 256 ? PpuHooks::kWideVram : PpuHooks::kWideBlank;
    }
    *entry = shadow_[s][ty & 31];
    return PpuHooks::kWideEntry;
  }
  // A walled-in room's margins are wall: nothing behind it (its BG2 sign
  // has the last level's scenery next to it).
  if (padRoom_ && (layer == 1 || layer == 2)) return PpuHooks::kWideBlank;
  // Bowser beaten: the castle's cloud layer holds only the status line and
  // the messages the ending writes on its screen (8-4's "Thank you Mario! The
  // kingdom is saved!"), which the repeat below put in both margins too.
  if (layer == 2 && victoryL_ >= 0) return PpuHooks::kWideBlank;
  if (layer == 2 && cloudStream_ && ty >= 4) {
    // Cloud panorama: 12 pages of 16x14 metatiles at $7F3000, streamed two
    // tile columns at a time just ahead of the screen. Margin columns are
    // built straight from the map (VRAM only holds a short history).
    int idx = ram(0x0ed9) | (ram(0x0eda) << 8);
    int head = ((idx / 0xe0) * 16 + (idx & 0x0f)) * 2;  // next tile column to stream
    // The stream head sits ~3 columns past the right screen edge; pick the
    // absolute column congruent to tx nearest the pixel's expected position.
    int want = head - 3 - ((256 - screenX) >> 3);
    int absCol = want + (((tx - want) % 64 + 64 + 32) % 64 - 32);
    int mc = ((absCol >> 1) % 192 + 192) % 192;  // the panorama loops
    int r = ty - 4;
    if (r >= 28) return PpuHooks::kWideVram;
    int m = snes_.wram[0x13000 + (mc / 16) * 0xe0 + (r >> 1) * 0x10 + (mc % 16)];
    uint32_t src = cloudTiles_ + m * 8 + ((absCol & 1) ? 4 : 0) + ((r & 1) ? 2 : 0);
    *entry = snes_.peek16(src);
    return PpuHooks::kWideEntry;
  }
  if (layer == 1 && bg2Static_) return PpuHooks::kWideVram;  // (its own 512 px map, see bg2Static_)
  if (layer == 1 && bg2Source_) {
    // Hill panorama (half-speed layer): pick the panorama column congruent to
    // the map column that belongs at this screen position.
    int want = ((cam16_ >> 1) + screenX) >> 3;
    int col = want + (((tx - want) % 64 + 64 + 32) % 64 - 32);
    if (bg2Period_ > 0) {
      // Left of the level's start the panorama loops, its end joining its
      // start as when a long level scrolls through it (the screen 256 px on,
      // the fallback below, doesn't join up: 8-2's hills were cut there).
      // Where its end doesn't join its start either (3-1 opens on a hill),
      // it is mirrored about its first column instead (tiles flipped).
      bool flip = col < 0 && bg2Mirror_;
      int c = flip ? -1 - col : col;
      // An area entered further on (a pipe's way out) has its panorama
      // written from where it starts: the columns before hold the last
      // area's (8-4's castle showed the water level's ruins there). The game
      // never shows them, so they mirror the area's own, about its start.
      int s0 = (areaLeft_ >> 1) >> 3;
      if (areaLeft_ > 0 && col < s0) {
        flip = true;
        c = 2 * s0 - 1 - col;
      }
      uint16_t e = bg2Entry(((c + bg2Off_) % bg2Period_ + bg2Period_) % bg2Period_, ty & 31);
      *entry = flip ? e ^ 0x4000 : e;
      return PpuHooks::kWideEntry;
    }
  }
  if (layer == 2 && ((ty * 8 - ppu.bgVofs(2)) & 0x1ff) < kHudLines) {
    // Status line rows: nothing to show in the margins (with HUD anchoring on
    // they are covered by the split status line anyway).
    return PpuHooks::kWideBlank;
  }
  if (layer == 1 || layer == 2) {
    // Otherwise assume scenery that repeats every 256 pixels: take the tile
    // one period away, which lies inside the visible 4:3 area.
    *entry = ppu.tileEntry(layer, (tx + 32) & 63, ty);
    return PpuHooks::kWideEntry;
  }
  return PpuHooks::kWideVram;
}

// ---------------------------------------------------------------------------
// Cheats. RAM layout (All-Stars keeps the NES $07xx layout, timers moved +$10):
// $075A lives, $0756 power (0 small/1 super/2 fire), $0754 size (0 big, 1 small),
// $07AF star timer, $07AE injury timer, $07E9-$07EB level timer digits, $00A0 player Y speed.

void Smb1Engine::applyCheats(uint32_t f) {
  cheats_ = f;
  if (!f || ram(0x0770) != 1) return;
  uint8_t* w = snes_.wram;
  if ((f & kCheatLives) && w[0x075a] < 9) w[0x075a] = 9;
  if (!gameplay_) return;
  if ((f & kCheatStar) && w[0x07af] < 0x10) w[0x07af] = 0x10;
  if ((f & kCheatNoDamage) && w[0x07ae] < 2) w[0x07ae] = 2;
  if (f & kCheatFire) {
    w[0x0756] = 2;
    w[0x0754] = 0;
  }
  // (Only in normal play, GameEngineSubroutine $08: the flagpole's tally
  // counts the time down to 0 before the level ends.)
  if ((f & kCheatTime) && ram(0x000f) == 0x08) {
    w[0x07e9] = 4;
    w[0x07ea] = 0;
    w[0x07eb] = 0;
  }
  if (f & kCheatMoonJump) {
    // While B is held, cap the player's fall speed so jumps float.
    if ((snes_.input[0] & kPadB) && (int8_t)w[0x00a0] > 1) w[0x00a0] = 1;
  }
  if (coopOk_) coopCheats(f);
}

// ---------------------------------------------------------------------------
// Level select: reproduces the game's own "next area" sequence (set world,
// level and area, load the area pointer and restart the area loader).

int Smb1Engine::areaCount(int world) const {
  if (world < 0 || world >= worlds_) return 0;
  // Every world ends with its castle area (area type bits = 3).
  int a = snes_.peek(worldTbl_ + world);
  for (int n = 0; n < 6; n++)
    if ((snes_.peek(areaTbl_ + a + n) & 0x60) == 0x60) return n + 1;
  return 4;
}

void Smb1Engine::requestWarp(int world, int level) {
  if (world < 0 || world >= worlds_ || level < 0 || level > 3) return;
  warpPending_ = true;
  warpWorld_ = world;
  warpLevel_ = level;
  warpTimer_ = 0;
}

void Smb1Engine::applyWarp() {
  int world = warpWorld_, level = warpLevel_;
  // Five-area worlds contain an intro area ahead of level 2.
  int area = level;
  if (areaCount(world) >= 5 && level >= 1) area = level + 1;
  uint8_t ptr = snes_.peek(areaTbl_ + snes_.peek(worldTbl_ + world) + area);
  uint8_t* w = snes_.wram;
  w[0x075f] = world;
  w[0x075c] = level;
  w[0x0760] = area;
  w[0x0750] = ptr;
  w[0x0751] = 0;  // entrance page
  w[0x0752] = 0;  // alternate entrance control
  w[0x075b] = 0;  // halfway page
  w[0x0757] = 1;  // fetch new game timer
  w[0x0774] = 1;  // disable screen
  w[0x0772] = 0;  // restart area initialisation
  warpPending_ = false;
  inputOverride_ = 0;
}

std::string Smb1Engine::debug() const {
  char buf[2048];
  int n = snprintf(buf, sizeof buf, "cam=%d parser=%d ext=%d+%d hofs1=%d bg2 src=%d off=%d period=%d cols:", camera(),
                   parserPos(), extL_, extR_, snes_.ppu.bgHofs(0), bg2Source_, bg2Off_, bg2Period_);
  for (int i = 0; i < 64; i++) n += snprintf(buf + n, sizeof buf - n, " %d", colWorld_[i]);
  n += snprintf(buf + n, sizeof buf - n, " areaLeft=%d fill=%d/%d", areaLeft_, fillValid_, fillMtValid_);
  if (coopOk_ && coop_) {
    n += snprintf(buf + n, sizeof buf - n, "\n  coop: live P%d (x=%d y=%d) lead P%d task=%d lives=%d alt=%llx/%llx held %d/%d %d/%d",
                  live_ + 1, ram(0x0078) * 256 + ram(0x0219), ram(0x0237), lead_ + 1, ram(0x000f), ram(0x075A),
                  (unsigned long long)altBits_, (unsigned long long)alt2Bits_, heldHave_[0], heldNeed_[0], heldHave_[1],
                  heldNeed_[1]);
    for (int id = 0; id < kCoopMax && n < (int)sizeof buf - 120; id++) {
      const CoopPl& p = pl_[id];
      if (!p.in || id == live_) continue;
      n += snprintf(buf + n, sizeof buf - n,
                    "\n  P%d: valid=%d wait=%d out=%d drawn=%d x=%d y=%d hi=%d size=%d status=%d idle=%u edge=%u colour=%d inj=%d safe=%d",
                    id + 1, p.valid, p.wait, p.out, p.drawn, xOf(id), gv(id, 0x0237),
                    gv(id, 0x00BB), gv(id, 0x0754), gv(id, 0x0756), p.idle, p.edge, variantOf(id), gv(id, 0x07AE), p.idleSafe);
    }
  }
  return buf;
}

void Smb1Engine::onVramWrite(Ppu& ppu, uint16_t adr) {
  uint16_t base2 = ppu.bgMapBase(1);
  if (adr >= base2 && adr < base2 + 0x800) {
    // A hill column streamed in (rows 0-30, then the blank row 31).
    uint16_t off = adr - base2;
    if (((off >> 5) & 31) == 30) bg2Streamed_ |= 1ull << ((off & 31) + ((off & 0x400) ? 32 : 0));
    return;
  }
  uint16_t base = ppu.bgMapBase(0);
  if (adr < base || adr >= base + 0x800) return;
  uint16_t off = adr - base;
  int c = (off & 31) + ((off & 0x400) ? 32 : 0);
  // The parser position points at the metatile column being uploaded, so
  // written columns lie in [r-60, r+4).
  trackParser();
  int lo = (parserPos() >> 3) - 60;
  int wc = lo + (((c - lo) % 64 + 64) % 64);
  colWorld_[c] = wc;
  int s = wc & (kShadowCols - 1);
  int row = (off >> 5) & 31;
  if (shadowCol_[s] != wc) {
    // First write of this column: start from what the VRAM slot holds, so
    // rows the game never rewrites match VRAM.
    shadowCol_[s] = wc;
    uint16_t colBase = base + (off & 0x41f);
    for (int r = 0; r < 32; r++) shadow_[s][r] = ppu.vram[(colBase + r * 32) & 0x7fff];
  }
  shadow_[s][row] = ppu.vram[adr];
}

void Smb1Engine::seedColumns() {
  // After loading a state the upload history is unknown: assume the usual
  // valid window behind the parser.
  int lo = (parserPos() >> 3) - 60;  // (the left margin at 21:9 reaches ~120 px behind the camera)
  for (int c = 0; c < 64; c++) colWorld_[c] = lo + (((c - lo) % 64 + 64) % 64);
  clearShadow();
  lastParser_ = parserPos();
}

bool Smb1Engine::colEntry(int wc, int row, uint16_t* e) const {
  if (wc >= parserPos() >> 3) return false;  // (not built yet)
  if (colWorld_[wc & 63] == wc) {
    *e = snes_.ppu.tileEntry(0, wc & 63, row & 31);
    return true;
  }
  int s = wc & (kShadowCols - 1);
  if (shadowCol_[s] != wc) return false;
  *e = shadow_[s][row & 31];
  return true;
}

// The game builds nothing left of where an area begins, so the margin there
// would be empty. Instead it repeats the area's plain floor and ceiling: per
// 16x16 block row, the commonest block of the area's first 24 columns (the
// earliest of equals). Objects (castles, posts, blocks, pipes) cover a row in
// only some columns, so they drop out. Chosen once per area start.
void Smb1Engine::makeFill() {
  constexpr int kCols = 24;
  int m0 = (areaLeft_ + 15) >> 4;  // first whole metatile column of the area
  uint16_t cols[kCols][32][2];
  bool have[kCols] = {};
  int n = 0;
  for (int i = 0; i < kCols; i++) {
    have[i] = true;
    for (int r = 0; r < 32 && have[i]; r++)
      for (int h = 0; h < 2 && have[i]; h++) have[i] = colEntry((m0 + i) * 2 + h, r, &cols[i][r][h]);
    n += have[i];
  }
  if (n < kCols / 2) return;  // (not built yet: try again next frame)
  // The collision map's metatile for a column and row (the map holds the 32
  // columns behind the parser).
  int pcol = parserPos() >> 4;
  auto mt = [&](int c, int row, uint8_t* v) {
    if (c >= pcol || c < pcol - 32) return false;
    *v = ram((((c >> 4) & 1) ? 0x05d0 : 0x0500) + row * 16 + (c & 15));
    return true;
  };
  fillMtValid_ = true;
  for (int r = 0; r < 32; r += 2) {
    int best = -1, bestN = 0;
    for (int i = 0; i < kCols; i++) {
      if (!have[i]) continue;
      int c = 0;
      for (int j = 0; j < kCols; j++) c += have[j] && memcmp(cols[i][r], cols[j][r], sizeof(cols[i][r]) * 2) == 0;
      if (c > bestN) {
        best = i;
        bestN = c;
      }
    }
    for (int k = 0; k < 2; k++)
      for (int h = 0; h < 2; h++) fill_[h][r + k] = cols[best][r + k][h];
    // (Block row b is tile rows 4 + 2b and 5 + 2b: the same column's.)
    if (r >= 4 && r < 30) fillMtValid_ = mt(m0 + best, (r - 4) / 2, &fillMt_[(r - 4) / 2]) && fillMtValid_;
  }
  // An area that opens with a wall from the top of the playfield to the
  // floor (1-2's bricks) ends there: the wall goes on, not what lies behind it.
  fillWall_ = false;
  if (have[0]) {
    uint16_t empty = cols[0][0][0];  // (rows 0-3, above the playfield, are empty)
    bool wall = true;
    for (int r = 4; r < 28 && wall; r++)
      for (int h = 0; h < 2; h++) wall = wall && cols[0][r][h] != empty;
    if (wall) {
      fillWall_ = true;
      for (int r = 0; r < 32; r++)
        for (int h = 0; h < 2; h++) fill_[h][r] = cols[0][r][h];
      fillMtValid_ = true;
      for (int b = 0; b < 13; b++) fillMtValid_ = mt(m0, b, &fillMt_[b]) && fillMtValid_;
    }
  }
  // An area that opens raised, on blocks standing from the floor up at least
  // two columns wide (the castles' first steps, 1-4), opens there too: its
  // first column goes on as a raised floor. (The plain floor before the
  // steps looked like ground the game doesn't have.)
  if (!fillWall_ && have[0] && have[1] && fillMtValid_) {
    uint8_t a[13], b[13];
    bool ok = true;
    for (int r = 0; r < 13 && ok; r++) ok = mt(m0, r, &a[r]) && mt(m0 + 1, r, &b[r]);
    // (the top row of the run of blocks up from the bottom; 13: none. Any
    // block: All-Stars' castle steps, $F5/$F6, are below CheckForSolidMTiles'
    // $FC yet stood on.)
    auto top = [&](const uint8_t* c) {
      int t = 13;
      while (t > 0 && c[t - 1] != 0) t--;
      return t;
    };
    if (ok) {
      int t0 = top(a);
      bool same = top(b) == t0;
      for (int r = t0; r < 13 && same; r++) same = a[r] == b[r];
      // The plain floor: the commonest column's, or where that has none (a
      // bridge level: 2-3 opens on ground by its castle) the column 8 on.
      int tf = top(fillMt_);
      uint8_t c8[13];
      bool ok8 = tf == 13;
      for (int r = 0; r < 13 && ok8; r++) ok8 = mt(m0 + 8, r, &c8[r]);
      if (ok8) tf = top(c8);
      if (same && t0 < tf) {
        // (only the raised blocks and below: scenery above stays the fill's;
        // the second column's tiles, without the first one's edge)
        for (int r = 4 + 2 * t0; r < 32; r++)
          for (int h = 0; h < 2; h++) fill_[h][r] = cols[1][r][h];
        for (int r = t0; r < 13; r++) fillMt_[r] = a[r];
        // Where the commonest column has no floor, its blocks between the
        // ceiling (a run down from the top: the castles') and the ground are
        // what spans the gaps (2-3's bridge): over the ground they hung in
        // the air. Sky there.
        if (ok8) {
          int c = 0;
          while (c < t0 && fillMt_[c] != 0) c++;
          for (int r = 4 + 2 * c; r < 4 + 2 * t0; r++)
            for (int h = 0; h < 2; h++) fill_[h][r] = cols[0][0][0];
          for (int r = c; r < t0; r++) fillMt_[r] = 0;
        }
      }
    }
  }
  fillValid_ = true;
}
