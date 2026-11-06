// Super Mario All-Stars specific layer: game detection, widescreen logic,
// HUD re-anchoring, cheats, level select and music data.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/snes.h"
#include "game/achievements.h"

class Smb1Engine;
class Smb2Engine;
class Smb3Engine;

enum class GameId { Boot = 0, Menu, SMB1, SMBLL, SMB2, SMB3 };

struct CheatDef {
  int id;          // bit index in the cheat mask
  GameId game;
  const char* name;
  const char* desc;
};

struct TrackDef {
  GameId game;
  const char* name;
  uint8_t cmd;     // value written to the game's music port
};

struct LevelSelectDef {
  GameId game;
  std::vector<std::string> worlds;
  std::vector<std::vector<std::string>> levels;  // per world
  std::vector<std::vector<int>> codes;           // optional per-level engine codes
};

class SmasGame : public PpuHooks, public Snes::StateExt {
public:
  explicit SmasGame(Snes& snes);
  ~SmasGame();
  void install();

  // Snes::StateExt: widescreen state saved with save states (applied by
  // onStateLoaded, after the per-game state has been reset).
  std::vector<uint8_t> saveExt() const override;
  void loadExt(const uint8_t* data, size_t size) override;

  void beforeFrame();
  void afterFrame();
  // Call after a save state was loaded (rebuilds tracking state).
  void onStateLoaded();
  void cancelWarps();  // also call on a user-initiated console reset
  // Super Mario Bros. / The Lost Levels: a brick broken from below sends
  // Mario back down, as on the NES (All-Stars keeps him rising). BMF54123's
  // brick fix, written into (or taken out of) the console's ROM.
  void setNesBrickBounce(bool on);

  // ---- identification
  GameId current() const { return game_; }
  static const char* gameName(GameId g);
  bool inGameplay() const { return gameplay_; }
  std::string describe() const;
  std::string verifyWide(int layer);  // development self-check

  // ---- widescreen
  void setWidescreen(int ext);          // requested margin (pixels per side)
  int widescreen() const { return reqExt_; }
  int activeExtL() const { return extL_; }  // margins actually rendered this frame
  int activeExtR() const { return extR_; }
  static constexpr int kMaxSupportedExt = 112;
  bool hudAnchor = true;
  bool dimWrap = true;                  // SMB2: dim the margins of areas that wrap round
  bool enemyRange = true;               // extend enemy activity to the widescreen view
  Smb1Engine* smb1Engine() { return game_ == GameId::SMBLL ? smbll_.get() : smb1_.get(); }  // (testing)
  bool coop = false;
  int coopPick = -1;
  // Every player's controller this frame (P1 and P2 are also the console's
  // own two), and which players are there (bit per player: online, those in
  // the session; else all), for the co-op engines.
  static constexpr int kPlayers = 5;
  uint16_t pads[kPlayers] = {};
  uint8_t padsPresent = 0x1F;  // a title screen's co-op choice (1 on, 0 off) for the app to keep; -1: none

  // ---- cheats
  static const std::vector<CheatDef>& cheatList();
  void setCheats(uint64_t mask) { cheats_ = mask; }
  uint64_t cheats() const { return cheats_; }

  // ---- level select
  static const std::vector<LevelSelectDef>& levelSelectList();
  // Request a warp: applied when the target game reaches gameplay.
  // character: SMB2's ($008F: 0 Mario, 1 Peach, 2 Toad, 3 Luigi; -1 the select's highlighted one)
  bool requestLevel(GameId g, int world, int level, std::string* err, int character = -1);
  // Start a game from the launcher (see smas.cpp); levelLoading() meanwhile.
  bool requestTitle(GameId g, bool startFile);

  // From a level select until the chosen level is being played (or the warp
  // has ended without it): the app runs the game out of sight at full speed
  // meanwhile, so the level comes up directly.
  bool levelLoading() const { return loading_; }
  // One-shot status message for the UI (empty if none).
  std::string takeMessage();

  // ---- music
  static const std::vector<TrackDef>& trackList();

  // ---- achievements (the checks of the running game; see achievements.h)
  AchievementTracker& achievements() { return ach_; }

  // ---- character palette variant (0 = original)
  int playerPalette = 0;  // Mario's colour variant (see game/palette.h)
  int luigiPalette = 0;   // Luigi's

  // PpuHooks
  void beginLine(Ppu& ppu, int row) override;
  int fetchWideTile(Ppu& ppu, int layer, int mapX, int mapY, int screenX, uint16_t* entry) override;
  void onVramWrite(Ppu& ppu, uint16_t adr) override;

private:
  std::vector<uint8_t> pendingExt_;
  AchievementTracker ach_;
  Snes& snes_;
  GameId game_ = GameId::Boot;
  GameId lastGame_ = GameId::Boot;
  bool gameplay_ = false;
  bool menuDissolve_ = false;  // the game-select carousel appearing (see beforeFrame)
  bool smasTitle_ = false;     // the All-Stars title, its margins painted on (see paintTitleMargins)
  bool titleDissolve_ = false;
  bool titleEdgesGone_ = false;  // the dissolve has covered the screen's edges (see titleDissolveMargins)
  static constexpr int kTitleCacheW = 112;
  std::vector<uint32_t> titleCache_;  // the last whole title frame's margins, per row: left then right
  void titleDissolveMargins(uint32_t* frame);
  void paintTitleMargins(uint32_t* frame, bool keep);
  bool picture_ = false;       // a screen drawn as one 256 px picture is shown wide (see pictureRemap)
  void pictureRemap(Ppu& ppu, int row);
  bool select_ = false;        // SMB2's character select is shown wide (see selectLattice)
  bool battle_ = false;        // SMB3's battle game: its screen round, the other side in the margins (dimmed)
  bool throne_ = false;        // SMB2's ending throne room: its edge tile columns repeated
  bool wasParade_ = false;
  bool parade_ = false;        // SMB2's ending parade: its crowd strip round its 256 px
  uint16_t paradeBg_ = 0;
  bool paradeFull_[32] = {};
  int wipeFrames_ = 0;  // an SMB2 picture being wiped in: black margins
  int selectLattice(Ppu& ppu, int ty, int x, uint16_t* entry);
  bool latticeValid_ = false;  // its frame's edge tiles, taken as it appears
  uint16_t latticeL_[32] = {}, latticeR_[32] = {};
  bool card2_ = false;         // SMB2's card between levels is shown wide (see cardTile)
  bool blackWide_ = false;     // SMB3's world map: as it is, black either side
  bool onMap_ = false;
  int cardTile(Ppu& ppu, int ty, int x, uint16_t* entry);
  struct CardPattern {         // its background tile and motifs, read off the card
    bool valid = false;
    bool card = false;         // (it reads as a card, see learnCard)
    uint16_t bg = 0;
    uint16_t motif[8] = {};    // (each the top-left entry of a 2x2-tile motif)
    int count = 0;
  } cardPat_;
  void learnCard();
  int reqExt_ = 0;
  int extL_ = 0, extR_ = 0;
  uint64_t cheats_ = 0;
  std::unique_ptr<Smb1Engine> smb1_, smbll_;
  std::unique_ptr<Smb2Engine> smb2_;
  std::unique_ptr<Smb3Engine> smb3_;

public:
  Smb3Engine* smb3() { return smb3_.get(); }
  // A co-op turn with the other player's variables in (a frame the game
  // overran ends there): no save state or rewind snapshot now.
  bool coopMidTurn() const;

private:
  // Automated launch of another title from the All-Stars menu, followed by a
  // level warp once that title is running.
  struct Warp {
    bool pending = false;
    GameId game = GameId::Boot;
    int world = 0, level = 0;
    int character = -1;  // (SMB2: see requestLevel)
    int stage = 0;
    int timer = 0;
    int lastIdx = 0;
    int attempts = 0;
    bool carousel = false;
    bool forceReset = false;  // relaunch even though the title is running
    bool sawMenu = false;
    bool titleOnly = false;  // (see requestTitle)
    bool startFile = false;
    int lastHofs = -1, still = 0, panelFrames = 0, pressT = -1000;
  } warp_;
  bool loading_ = false;
  int loadFrames_ = 0, loadIdle_ = 0;
  void updateLoading();
  std::string message_;
  uint16_t warpInput_ = 0;
  bool warpOverride_ = false;
  void driveLaunch();
  bool startTitleWarp();

  Smb1Engine* engine();  // SMB1-engine instance for the running title (or null)
  void detectGame();
  void applyCheats();
  void updateWarp();
  void applyPlayerPalette(Ppu& ppu);
  static bool execThunk(void* ctx, uint32_t pc);
  static void writeThunk(void* ctx, uint32_t adr, uint8_t v, uint32_t pc);
};
