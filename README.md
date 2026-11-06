# Super Mario All-Stars — PC Port

A native Windows and Android version of *Super Mario All-Stars* (SNES) with a launcher, true widescreen,
re-anchored HUDs, display options, cheats, level select, achievements, a sound test and a music
player. **It contains no game data: it runs from your own ROM**
(`Super Mario All-Stars (USA).sfc`, CRC32 `925637C7`); no game starts without it.

## Building
Requirements: Windows 10/11, Visual Studio 2022 (C++ workload) and CMake 3.20+.
No other libraries are needed (Direct3D 11, Direct2D, XAudio2, XInput and
DirectInput come with Windows).

```
build.bat
```
The program is written to `build\Release\SuperMarioAllStars.exe`.

### Android
The same code builds for Android (`android/`), as the Mickey & Donald Collection's does: SDL2 2.32.10
compiled from source for the window, sound and controllers, OpenGL ES 3.0 for the picture. You need the
Android SDK with NDK 28.2 and CMake 3.31.6 (both from the SDK manager) and JDK 17 or later. The SDL2 source
belongs in `third_party/SDL2-2.32.10-src` (`android/fetch_sdl.ps1` downloads and checks it), then:

```
cd android
gradlew assembleDebug
```
The APK ends up in `android/app/build/outputs/apk/debug/` (64-bit ARM phones and tablets, Android 6.0 or
later, and the x86_64 emulator). Release builds are signed when `android/keystore.properties` names a
keystore (`storeFile`, `storePassword`, `keyAlias`, `keyPassword`), as the Disney collection's are.

## Running
Put the ROM next to the executable, in a `roms` folder beside it, or in any parent folder up to
three levels up; the location is remembered in `rompath.txt`. Without it the launcher's cards say
*ROM not found* and what is needed, and a ROM copied in while the launcher is open is picked up
within a second. Another version or dump of All-Stars runs too, its file name shown in amber on the
cards: widescreen, cheats and the level select are tuned for the USA version.

To start a game directly, give `SuperMarioAllStars.exe` the argument `--game smb1`, `--game smbll`,
`--game smb2` or `--game smb3` (for example in a shortcut).

Battery saves go to `saves\smas.srm`, save states to `saves\`,
screenshots to `screenshots\`, settings to `smas_port.ini`. Files are written in full before
they replace the old ones, so a full disk can't leave a truncated save (you are told the save
failed instead). Save states carry a checksum: a damaged one is refused rather than loaded. They
also keep the widescreen state, so a state loads exactly as it was saved, and one saved at a
narrower width (or by an older version) brings in the enemies of the wider view. If the graphics
driver resets (a driver update or crash), the picture comes back by itself and the game keeps
running.

## On Android
Install the APK (Android asks you to allow installing apps from the place you opened it). On the first
start the launcher's card says *ROM not found*: tap it, pick `Super Mario All-Stars (USA).sfc` (or a
`.smc`) in the file picker, and it is copied into the app. A ROM can also be put in the app's folder
`Android/data/com.darhkfox.allstars/files` (or a `roms` folder there) with a computer.

- **Touch:** an SNES pad over the game: the D-pad bottom left, A B X Y bottom right, L and R at the
  sides, Select and Start bottom centre, Menu top left, and (when they're switched on and achievements
  are off) rewind and fast forward top right. Tap a game's card to choose it, tap it again to play; in
  the menus tap a row to use it, drag to scroll, and the button top right goes back.
- **Controllers and keyboards** work as on Windows (the same bindings); the on-screen controls hide
  while one is in use (*Options → Controls → On-Screen Controls*: While Touching, Always or Never, and
  their opacity).
- **Back** opens and closes the menu, as Esc does.
- The picture fills the screen (*Aspect Ratio: Auto*), with the same filters and colour settings. The
  window settings, VSync, Black Frame Insertion, High Frame Rate and slang shaders are Windows only.
- Saves, states and settings are kept in the app's own storage; screenshots go to
  `Android/data/com.darhkfox.allstars/files/screenshots`. Leaving the app saves the game's progress.

## Launcher
The program opens on a launcher, laid out like the Mickey & Donald Collection's, over a night-blue
sky with twinkling stars and a checkerboard floor after the All-Stars title screen. By default it
shows one **Super Mario All-Stars** card with a live preview of the cartridge's title: choosing it
starts the cartridge as it is, at its own title and game select (`--game allstars` starts it
directly). Below are buttons for **Options**, **Achievements**, **Music Player**, **Credits** and
**Quit**.

*Options → Launcher Shows → The Four Games* gives one card per game instead (Super Mario Bros., The
Lost Levels, Super Mario Bros. 2, Super Mario Bros. 3), each with a live attract-mode preview of that
game and the chosen card's sound playing. Each preview is its own console run through the All-Stars
game select into the game's title, out of sight while it starts. Choosing a card resets the console
and takes it through the All-Stars menu to that game's save files (behind a short black loading
screen), where you pick the file as usual. The launcher remembers the last game chosen.

*Options → Game Covers* shows the player's own cover pictures on the cards instead of the live
previews, and *Custom Background* their own picture behind the launcher (filling the window, darkened
a little so the text reads). None are shipped. PNG or JPG files named `smb1`, `lostlevels`, `smb2`,
`smb3`, `allstars` (the single card) and `background` go in the `covers` folder (*Open Covers Folder*:
next to the exe on Windows, in `~/.local/share/SuperMarioAllStars` on Linux); on Android *Choose Cover*
/ *Choose Background* copy a picture from the phone in. They're read with
[stb_image](https://github.com/nothings/stb) (`third_party/stb`, public domain / MIT), big ones
shrunk to 1024 px (2048 for the background). A card without a cover keeps its preview; one with a
cover runs no preview console at all, which also makes the launcher lighter on slow devices.

## Controls
| | Keyboard | Joypad (Xbox layout; PlayStation, Switch Pro and USB/SNES pads supported) |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| B / A / Y / X | Z / X / A / S | bottom / right / left / top face buttons |
| L / R | Q / W | LB / RB |
| Start | Enter | Menu/Start |
| **Menu** | Esc or Right Shift (Select) | View/Select |
| Game's Select button | *Menu → Press Select In Game* | left stick press |
| Save / load state | F5 / F8 | |
| Fast forward | Tab (hold) | RT (hold) |
| Rewind | Backspace (hold) | LT (hold) |
| Fullscreen | F11 or Alt+Enter | |
| Launcher: choose / play / quit | Arrow keys / Enter / Esc | D-pad / A / B |
| Screenshot | F12 | |

Hold Rewind to go back through roughly the last minute of play (at twice the game's speed),
then let go to carry on from there; game progress the game has saved is never undone. Rewind and
fast forward can each be switched off under *Menu → Controls*.

Everything can be rebound under *Menu → Controls* (keyboard and pad, both players). Each
controller keeps its pad number while others are plugged in or out, and gets it back when it
reconnects, so the players never swap controllers. Select opens
the menu and backs out of it. The games only need their own Select button for the All-Stars save
menu (pause, then Select). If you'd rather have it on Select, set *Controls → Select Button* to
"Tap: Game Select - Hold: Menu". In the menus and the launcher the pad's bottom face button confirms
and the right one goes back; *Controls → Menu Buttons → Right Confirms* swaps them (Nintendo's layout).

## Menu
The pause menu is a dark panel over the dimmed game, tinted in the running game's colour (red
SMB1, green Lost Levels, purple SMB2, orange SMB3; the launcher's buttons gold). Every row
explains itself in the line at the bottom. It is laid out as:

* **Resume** and **Press Select In Game**.
* **Save states** — the slot (with the time it was saved, or "Empty"), Save State, Load State.
* **Game** — Level Select, Cheats, Achievements (earned / total), Sound Select, Music Player,
  Options (Display, Graphics, Colour, Audio, Controls, fast forward and rewind, achievements on
  or off), Credits.
* **System** — Take Screenshot, Reset Game (back to the game's save files), Return to Launcher,
  Quit to Desktop (the last three ask first).

Short notices (state saved, screenshot taken) appear in the top-right corner, or at the bottom
while the menu is open. An unlocked achievement shows a banner at the top of the screen.

What each page holds:
* **Level Select** — every level listed under its world, the game chosen at the top (it starts on
  the one being played); picking one asks first. For SMB2 a Character choice (Mario, Luigi, Toad,
  Peach) sets who plays it, picked for you on the character select. Any world and level of all four games: SMB1, The Lost Levels (worlds
  1–9 and A–D), SMB2 (1-1 to 7-2) and SMB3 (every world map, numbered level and fortress, and
  the levels without a numbered panel: each world's airship and the Coin Ship, World 2's desert and pyramid,
  World 5's second fortress and tower, World 7's Piranha Plant levels, World 8's tanks,
  battleship, hand traps, super tanks and Bowser's castle. Those are entered from 1-1's panel
  with the level swapped as it loads, the map left as it was).
  The chosen level comes up directly: the game is run to it behind a short black loading screen,
  launched from the game-select screen when needed (a running game is reset first, so unsaved
  progress is lost). In SMB3 it is a new game in that world, with the world as play would have
  left it: the levels numbered before the chosen one are cleared, and where the map's paths lead
  to it on foot, the fortresses before it too, with their locks open (World 1's 1-5: 1-1 to 1-4,
  the fortress and its lock). Where the way leads through a map pipe (World 4 on), only the
  numbered levels are. Clearing the level marks its panel as usual; losing a life there sends
  Mario back to the world's start, from where the cleared part of the map is open.
* **Cheats** (per game) — SMB1/Lost Levels: infinite lives, star power, invincibility, always
  fire, infinite time, moon jump. SMB2: infinite lives, full health, star power, four
  hearts, super jump, float like Peach. SMB3: infinite lives, star power, infinite flight,
  full P-meter, infinite time, always raccoon, stocked item box, invincibility.
* **Sound Select** — classic sound test: any music number of any game's sound bank plus
  the three sound-effect channels.
* **Music Player** — named track lists for the menu, SMB1 & Lost Levels, SMB2, SMB3 courses
  and SMB3 world maps, with repeat / play-next and an 8-channel level display. It plays on
  its own sound chip, so the game is untouched.
* **Display** — windowed / borderless / exclusive fullscreen, monitor selection, window
  size, aspect ratio (4:3, 16:10, 16:9, 21:9, auto), pixel shape (CRT 8:7 or square),
  scaling (fit, integer, stretch; integer uses whole multiples both ways, with CRT pixels as
  close to 8:7 as that allows), HUD re-anchoring, widescreen enemy behaviour, dimmed sides
  in SMB2's wrap-around climbing areas,
  side-border style, VSync (turn it off for G-SYNC / FreeSync), black frame insertion (a black
  refresh after each game frame for sharper motion, like a CRT; needs VSync and a 120, 180 or
  240 Hz display, and the picture gets darker), High Frame Rate (on 100 Hz and faster
  displays the refreshes between the game's 60 frames show in-between pictures: each line is
  drawn again with the layers' scroll and the sprites part of the way back to the last
  frame, cuts and jumps left as they are; the game still runs at 60 and the picture is a frame
  later; at a rate that isn't a multiple of 60, like 100, 144 or 165 Hz, each in-between is
  drawn at the moment its refresh shows it, so motion stays even; with VSync off, on
  G-SYNC / FreeSync, they're presented at the display's rate), FPS
  counter.
* **Graphics** — nearest, sharp bilinear, bilinear, CRT scanlines, CRT aperture grille and
  LCD grid filters, effect strength, screen curvature, sprite-limit removal.
  **Slang Shader** runs a RetroArch slang preset (`.slangp`) in place of the filter, through
  [librashader](https://github.com/SnowflakePowered/librashader) (`librashader.dll` next to the
  exe, MPL-2.0, `third_party/librashader`; without it the game runs on the built-in filters).
  Presets are listed from a `shaders` folder next to the exe, with their shader files beside
  them, e.g. the libretro `slang-shaders` collection; the colour settings don't apply to them.
  Seven CRT presets are bundled (`third_party/slang-shaders`, copied to `shaders\` by the build):
  crt-lottes (public domain), crt-geom, crt-royale, crt-easymode, crt-aperture, zfast-crt and
  crt-pi (GPL, `LICENSE-GPL-2.0.txt`), unmodified from libretro/slang-shaders with only the files
  they use; see `third_party/slang-shaders/README.txt`.
  Some presets assume a 4:3 picture and can stretch or frame oddly in widescreen.
* **Colour** — colour profiles (Original, Vivid, CRT Television, Muted, Greyscale, Sepia,
  Pocket, Virtual, Night, Console Accurate), brightness, contrast, saturation, gamma,
  colour temperature, colour-vision assist (protanopia / deuteranopia / tritanopia), and
  Mario's Colours / Luigi's Colours: alternate outfits for each brother (Super Mario Bros. and The
  Lost Levels), kept by each in co-op whichever is the game's player.
* **Menu Style** — how the menus look: *Storybook* (the default: a cream card with a dark outline, Luckiest Guy
  titles in the game's colour, a yellow star cursor), *Retro* (black with brick bars, a 5x7 pixel font drawn by the
  port, ? block switches), *Themed Dark* (the dark panel under a heading in the game's pattern: bricks, the Lost
  Levels' green bricks, SMB2's starry night, SMB3's checkers) or *Classic* (the original dark panel). `src/app/menu.cpp`.
* **Audio** — volume, **Music Volume** and **Sound Effects Volume** (each 0–100%: music off with the sound effects kept, or the other way round; the sound driver marks the voices an effect holds at ARAM `$1A`, so the rest are the music and only the mix changes), latency, mute when unfocused, and **Music Pack**: an MSU-1 custom soundtrack (see below).
* **Updates** — *Check for Updates* (on by default): once a day the launcher reads GitHub's public "latest release" for
  this repository (`src/app/updatecheck.cpp`; WinHTTP on Windows, the system `curl` on Linux, `HttpURLConnection` on
  Android, from a background thread) and shows a badge when its tag is newer than the app's version; the badge, or
  *Get Version*, opens the release page. Nothing is sent but the request; nothing is downloaded or installed.
* **Gameplay** — **NES Brick Bounce**: in Super Mario Bros. and The Lost Levels, a brick broken from below sends Mario back down as on the NES instead of letting him keep rising (BMF54123's brick fix, updated by Blittle McNilsen, written into the console's ROM while on: `SmasGame::setNesBrickBounce`). Off by default.
* **Credits** — where to find the author (YouTube, Reddit, GitHub; each opens in your browser).
* **CO-OP GAME on the title screens** of Super Mario Bros., The Lost Levels and Super Mario Bros. 3:
  a third choice under 1 PLAYER GAME / 2 PLAYER GAME (MARIO GAME / LUIGI GAME; in SMB3 a fourth,
  under BATTLE GAME), picked with the game's own cursor. It starts a one-player game with co-op on
  (Luigi on controller 2); the game's own choices start without it. The choice sets the option below.
  In SMB1 and The Lost Levels the status bar's name reads MARIO & LUIGI during a co-op game, and
  the level card (WORLD 1-1) shows both brothers' heads; in SMB3 the status bar (in levels and on the map) shows a combined M L badge, x and the shared lives,
  and the map's WORLD box shows both brothers. A co-op game
  starts (or continues) with at least 10 lives, as the brothers share them.
* **Options → Multiplayer → Two-Player Co-op (SMB1, Lost Levels, SMB3)** — Mario and Luigi play
  at the same time in Super Mario Bros. and The Lost Levels. Controller 1 has the character the
  game was started with (Mario, or Luigi in a Lost Levels Luigi game), controller 2 the other (by
  default the second controller, or I J K L, N jump, H run). The game's own player code runs a
  second time each frame with the other player's variables swapped in, so both move, jump, bump
  blocks, stomp enemies, get hurt, take power-ups and throw fireballs as the game's player does;
  in The Lost Levels each has his own physics and sprites (Luigi jumps higher and slides more).
  Objects that meet the player in their own code (lifts, springboards, firebars, hammers) run a
  second time for the other player, so both ride lifts, bounce on springs and get burnt.
  The world is shared. Whoever is further right moves the screen and the other is held at its
  left edge; the screen waits while the one behind is at that edge and still playing. Left idle
  (controller untouched for 3 seconds) while the other plays, he is brought to where the other
  last stood on the floor (or swims), can't be hurt by enemies, and is brought back rather than
  lost if he drops into a hole, until his controller is used again. He walks through the game's scenes too: the pipe intro (as
  before 1-2), and at the flag he slides down the pole if he reaches it and follows into the castle.
  Whoever takes Bowser's axe has the victory scene (Toad thanks him); the other comes along to
  Toad. The score is one shared score; after a game over, Continue goes on with the character the
  game was started with. Either can take a pipe, a vine or the flag: the game goes on with him and the other
  comes along after it. Lives are shared: a player who dies while the other plays on comes back
  beside him once that player is on his feet (on solid ground where there is some), small and
  blinking, for one life; with none left he sits out until the next area.
  When a player dies while the other plays on, his death tune plays and the area's music then
  carries on from where it paused (not from its start).
  Save states and rewind keep both players (see `src/game/smb1coop.cpp`).
* **Super Mario Bros. 3 co-op** (the same option; see `src/game/smb3coop.cpp`) —
  Luigi on controller 2, run through the game's own player code with his own variables: his own
  form and power-ups, stomps, hits, the star, flying, picking up the goal card. Each enemy or item
  deals with whichever player is nearer (enemies' fireballs and hammers too; a kicked shell hits
  either, as in the original), or with the one playing while the other is left idle. Luigi throws his own fireballs and hammers, carries shells and
  slides down slopes. The screen follows the point between the two (both
  directions, and up for a flyer) and its edges hold them in view (the whole wide view in
  widescreen). A player whose controller has been left alone for 3 seconds rests: the screen
  follows the other, and the resting one is brought to where the other last stood when the
  screen leaves him behind (an auto-scroll's edge included), and nothing hurts him until his
  controller is used again (pits, lava and the time limit still count). Lives are shared. Either can die while the other plays on, as in
  SMB1's co-op: if Mario dies, Luigi takes over as the game's player (controllers and colours
  follow each brother). The one who died plays his death tune and jump, then comes back where the
  other stands, small and blinking. Either can take a pipe or a
  door, or grab the goal card or a Koopaling's wand: the other goes along, into the next area in
  the form he had. The map has Mario only; the level start card reads MARIO START! / LUIGI START!.
* **Online Play** (the menu's *Online Play*, or `--host [port]` / `--join address`; see `src/app/netplay.h` and
  `src/app/app_net.cpp`) — two copies of the port in lockstep over UDP (port 7845 by default, forwarded to the
  host, or a VPN such as Tailscale). The console is deterministic, so only the controllers travel: the host's local
  controller is controller 1, the client's controller 2, and each frame runs on both once both are known, a few
  frames after they were pressed (the input delay, from the ping; Online Play > Input Delay). The host sends its
  save state as a *sync point*, with the settings that change what the console does (widescreen margin, co-op,
  cheats, NES Brick Bounce): at the start, after its menu changed something (a state loaded, a game started, a
  setting), and when the two drift apart (every second each hashes its work RAM; the client compares). States go
  as a run-length coded XOR delta to the last sync point (about 120 KB the first time, 10-20 KB after), in
  acknowledged chunks; inputs are sent redundantly until acknowledged, so a lost packet costs nothing. Either
  side's menu pauses both. The client can't load states or start games; its battery save is never written while
  the console holds the host's, and is read back when the session ends. Both need the same version, ROM (a copier
  header aside) and music pack, checked as the client joins.
* **Co-op speed** — the game's player code runs twice a frame in co-op, which slowed busy scenes
  down on the console's own clock, so with co-op on (SMB1, The Lost Levels, SMB3) the emulated
  CPU runs at twice its speed (`Snes::setCpuFast`); the PPU, DMA and sound chip keep their timing.
  One-player games keep the console's timing exactly.

## Custom soundtracks (MSU-1)

The port plays MSU-1 music packs, the CD-quality soundtracks made for flash carts and emulators. None are included.
Put each pack in its own folder in the `msu` folder (Windows: next to the exe; Steam Deck:
`~/.local/share/SuperMarioAllStars/msu`; Android: `Android/data/com.darhkfox.allstars/files/msu`), with its
MSU-1 patch (`.bps` or `.ips`, for the USA ROM without a copier header) and its tracks (`NAME-1.pcm`, `NAME-2.pcm`,
...), then choose it under Options > Audio > Music Pack. A pack without a patch can use one put in the `msu` folder
itself.

The patch is applied to the console's copy of the ROM as it loads (the file is never changed; the launcher's
previews run it unpatched), and `src/core/msu1.cpp` emulates the chip: its registers at `$2000-$2007`, the tracks
streamed from disk and resampled from 44.1 kHz into the console's 32 kHz sound. The track and its place are kept in
save states. Both the standard patch (each song replaced) and the Super Mario All-Stars Deluxe patch (one of several
versions of the music picked at each new level) work as they do on hardware.

## Achievements
82 achievements: 20 for Super Mario Bros., 19 for The Lost Levels, 20 for SMB2, 21 for SMB3,
and two for the whole collection. *Menu → Achievements* lists them by game with the date each
was earned. Achievements with several steps show progress, for example "3 / 8" castles; the
"defeat 100 enemies" ones count across all your play. A
few are secrets: they are marked "Secret:" in their description. They are saved in `achievements.ini`
next to the settings, and they stay earned.

**Fair play.** While achievements are on, cheats, level select, fast forward and rewind are
off. *Options → Achievements* switches them off, and those features come back. A run counts
from a game started from the launcher, or from *Reset Game*, with achievements on. A save state remembers
whether its run counts, so loading one from a cheated run doesn't earn anything. The first start
explains this once.

| Game | Achievements |
|---|---|
| Super Mario Bros. | Wrong Castle (World 1 castle), Castle Crasher (every castle, 0/8), Thank You Mario! (8-4), The Long Way Round (finish from 1-1 without a warp zone), Shortcut (a warp zone), Top of the Pole (5000 at the flagpole), Fireworks Show, Coin Heaven (a vine up to a hidden area), Pocket Change (100 coins), Not a Scratch (a castle without getting hurt), Star Power (5 enemies with one Starman), Lives to Spare (20 lives), Player Two (finish a level as Luigi), Six Figures (100,000 points), Fired Up (a Fire Flower), Express to World 8 (warp straight there), Bro Down (a Hammer Bro), Barbecue Bowser (Bowser by fireballs), Goomba Stomper (100 enemies, counted over all your play), and a secret: Speedy Plumber (finish 1-1 with 300 or more on the clock) |
| The Lost Levels | Same Old Story, Lost No More (castles of worlds 1-8, 0/8), Peach Found, Secret World 9, The Very End (World D castle), Warp Ahead, Lofty Leap, Celebration, Cloud Nine, Coin Purse, Lost but Unharmed, Shooting Star, Hot Stuff (a Fire Flower), Brother Beater (a Hammer Bro), Green Machine (a castle in the LUIGI GAME), Bowser Flambé (Bowser by fireballs), Lost Legion (100 enemies), and two secrets: Wrong Way! (get sent back by a warp zone), No Time to Lose (finish 1-1 with 300 or more on the clock) |
| Super Mario Bros. 2 | One for each boss: Bomb Squad (Mouser), Snake Charmer (Tryclyde), Cat and Mouser, Cool Customer (Fryguy), Rock Lobster (Clawgrip), Heads Will Roll and Wake Up! (Wart). Also Dream Team (clear a level with each hero, 0/4), Untouchable (a level without getting hurt), Jackpot (a life from the Bonus Chance), Full of Heart (four hearts), Mushroom Hunter, Starman, Down the Jar (a warp), POW! (three enemies at once), Extra Lives (10 lives), Through the Door (Subspace), Phanto Menace (wake a Phanto), Egg Return (defeat Birdo), Veggie Tales (100 enemies) |
| Super Mario Bros. 3 | Royal Rescue (a king's wand), All the King's Wands (0/7), Dark Land (World 8), The End of Bowser, Warp Whistle, Frog Prince, Tanooki Time, Hammer Time, Wings of Wonder (P-Wing), Full House (three matching cards), Coin Hoard (100 coins), Packed Inventory (28 items), Toad's Treasure, Seven Figures (1,000,000 points in one game; a game over resets the score, saves keep it), Lives Galore (99 lives), Stone Cold (a Tanooki statue), If the Shoe Fits (Kuribo's Shoe), Lullaby (a Music Box on the map), Rock Breaker (a Hammer on a map rock), Koopa Krusher (100 enemies), and a secret: Five-Up (three Star cards at the goal) |
| Collection | All-Star (finish all four games, 0/4), Completionist (every other achievement) |

In SMB1 co-op, Player Two counts when Luigi takes the axe or rides the flagpole down (as the
game's player, or kept aside and already on the pole as the flag scene starts), and Not a Scratch
is lost when either player is hurt.


**Export to CSV** (top of the Achievements list) writes an `achievements export` folder: `achievements.csv` in
the columns PlayniteAchievements' spreadsheet import reads (ID, Title, Description, Points, Trophy Type, Hidden,
Rarity, Category, Progress, Progress Total, Unlocked, Unlock Time, Unlocked Icon, Locked Icon) and `icons/` with
each achievement's badge, earned and locked, drawn by the port (`src/app/achexport.cpp`: a colour per game, a rim
per trophy tier, a symbol per achievement). Points follow the tier: bronze 10, silver 25, gold 50, platinum 100.

## Frame pacing
The games run at 60.1 frames per second. With VSync on, the frame loop waits on the display's
vertical blank itself, so driver settings that force VSync off or use "Fast Sync" don't cause
uneven frames. On 60, 120 or 240 Hz displays every frame lasts the same number of refreshes. With
VSync off, frames are presented at the game's own rate, which is what G-SYNC / FreeSync displays want.

## How widescreen works
The port emulates the SNES hardware and renders each scanline wider than the original
256 pixels. Per-game logic (in `src/game/`) makes the extra area show real content:
* level tiles come from the game's own streamed tilemaps; columns the game has not
  produced yet are never shown as garbage;
* parallax scenery that the game streams only just ahead of the screen is rebuilt from
  the game's scenery data;
* the status bar is split and pinned to the screen edges;
* sprites in the margins get correct 9-bit positions, and with *Widescreen Enemies:
  Active In Full View* enemies spawn just outside the wide view and stay active across
  it, instead of popping in or freezing at the old 4:3 edges. What's in the margins then
  behaves as on the screen: the games skip things for objects off their 4:3 screen, and
  those tests use the wide view instead (fireballs and kicked shells hit enemies there,
  enemies there fire, throw and spit, and their projectiles aren't erased at the old edge).

Per game:
* **SMB1 / The Lost Levels** — the screen can't scroll back, and the game stops the player at
  the left edge of its 256-px screen. The extra width is shown ahead of the player first, as far
  as the game has always built the level, with room for an enemy to stand on it (72 px). The rest
  (20 px at 16:9, up to 128 px at 21:9) shows the level just passed on the left, and the player
  can walk back into it, right up to the display edge: the game's own wall at its screen edge
  moves out to there, and his collisions with enemies and fireballs there are worked out in full
  precision (the game's are 8-bit and screen-relative).
  Left of where an area begins (a level start, a pipe exit) the game has built nothing, so the
  margin there continues the area's own floor, ceiling, hills and clouds (an area that opens with
  a wall, like 1-2, carries on the wall, and one that opens raised, like the castles' first steps,
  carries on the raised floor, with sky above where the rest of the area is a bridge, as in 2-3
  and 7-3; the hills carry on as their panorama loops, or mirrored
  about the level's start where its loop doesn't join up); the player still stops at the area's
  real start, as on the console. The level is built
  exactly when the console builds it, because level objects act when they're built (scroll locks,
  warp zones, frenzies, castle loops). The game's logic is untouched: only its sprite drawing is
  told that objects in the margin are visible. With *Widescreen Enemies: Original Timing*
  fireballs, hammers and enemies behave, collide and disappear exactly as on the console; with
  *Active In Full View* the player's fireballs fly on into the margin and hit enemies there
  (they were erased at the 4:3 edge), Hammer Bros there throw, and their hammers stay in the
  view (also with the player in the left margin, where they reach him and hurt him). A coin
  block hit in the left margin bounces and shows its coin there. With *Original Timing*, the game's own
  screen is pixel-identical to 4:3; this is checked automatically in 1-1, 1-2, 1-4 and 2-2. The
  hill and cloud panoramas are drawn from the game's background maps in RAM. The "BONUS" Mario
  picture in the cloud bonus areas (2-1's vine) leaves through the margin as it scrolls by: it
  used to vanish once it had left the 4:3 part, the sky on screen then too plain to keep its
  layer recognised. Night levels have
  no clouds, and their hills are a fixed 512 px map the hardware wraps, drawn as such (3-1's and
  6-2's used to be cut at the edges of the 4:3 part). The castle mazes keep their margins when
  they loop: after a wrong turn sends Mario back, the walls in the margins carry on the ones in
  the 4:3 part (4-4's showed other parts of the castle's backdrop). The level next to the
  start, where the loop leaves the camera, is shown too, not left blank. An area entered further on (a pipe's way out) carries its own background on, mirrored,
  left of where it starts (8-4's castle showed the water level's ruins there). With *Active In Full View*,
  enemies (groups of Goombas and Koopas and Bowser's flames included) arrive from just outside the
  wide view, the game's own distances measured from its edge. That is further out than the game
  has built the level, so a walking enemy spawned there waits, out of sight, until the level under
  it is built (it would otherwise fall through the unbuilt part, or end up inside the bricks built
  around it). A group of enemies that had to wait for room (the wide view keeps enemies longer),
  or that comes in range as an area loads mid-level (out of a pipe), never starts inside a block
  or where the level isn't built yet (1-1's last two Goombas used to end up in the end staircase
  after the coin room). A single walker or swimmer that had to wait for one of the five enemy
  slots starts just beyond the view's edge rather than at its own place, which by then is in view
  (2-2's Blooper popped in), on the same kind of ground; where there is none clear there (5-1's
  Koopa, at a block), and for hopping Paratroopas, it starts in its place. A jumpspring always
  gets one of the enemy slots: with them all taken it went to the slot the flagpole's flag and
  power-ups use, and vanished (2-1's spring before its wall, leaving the player stuck on its
  tiles); now an enemy behind the 4:3 screen makes room, or the spring moves to the first slot
  that frees up. A vine stays until it leaves the view (the game erased it once off its 256 px
  screen, so it vanished in the margin as Mario climbed on and the screen moved). Sprites are
  placed by their objects' true positions, so one that
  leaves on the right (a kicked shell) can't reappear on the left, and sprites the game hides by
  moving them off its screen (sparks, fireworks) stay hidden. Enemies that walk off left past where
  an area began keep to the floor shown there. The status line is split to the
  corners. The underground coin rooms are shown wide and centred: their brick wall carries on
  into both margins and the exit pipe is drawn whole, its right half included (checked in every
  coin room of both games). The short scene before x-2's pipe (the castle, the pipe Mario walks
  into) is shown wide and centred too, from the moment it fades in: its ground, hills and clouds
  carry on into both margins (it used to be 4:3). So is the card between levels (WORLD 1-2, the
  lives left, the level's picture): its grey fills the display and the status line is split to
  the corners as in play. Leaving play (a pipe, a death, a level's end, a vine), the picture
  changes once the screen has gone dark. Once Bowser is beaten, the walk to Toad (or the princess) and the
  messages stay wide, eased to the centre of the display (the messages are centred on the
  game's screen); before, the view dropped to 4:3 as the message came up. 8-4's ending shows
  its message once (it is on the cloud layer, which the margins used to repeat) and the room
  whole to the right. Bowser is drawn whole
  as he comes into view: All-Stars' own Bowser drawing made half his sprites 8x8 while they were
  off its 256 px screen, which cut off his shell in the margin. Firebars are drawn in the margin too: with *Original Timing* they stay
  still there until they reach the 4:3 screen, as the console leaves them; with *Active In Full
  View* they turn (and burn) across the whole view.
* **SMB2** — enemies spawn across the whole wide view, including when an area starts (the
  game's check against spawning on top of the player compared positions modulo 256 px, which
  refused almost every spawn ahead at 21:9). Levels are kept whole in RAM, so the margins are drawn from the level data
  with the game's own tile tables (checked tile-for-tile against the game's output); night
  skies come from the game's own star map. Near an area's start the slower hills and clouds
  show more than the game has laid out. The clouds are a loop of four screens (shuffled each
  time an area is entered), so the loop carries on; hills that repeat do the same. Other
  scenery repeats its first screen where its ends join up, and is mirrored about the area's
  start where they don't, so no cloud or hill is ever cut off. All 8 cloud orders and every area's
  start and end were checked at 21:9. Jar interiors are shown wide and centred, the jar standing
  in its area's scenery: the screen's background carries on into the margins (repeated where its
  ends join up, mirrored where they don't), only what runs on from the screen's edge, so a moon
  or a lone cloud isn't doubled. Vertical areas (one screen wide; the game wraps them around)
  and single-page areas are shown 4:3, centred. Both switch while the screen is dark. Enemies are drawn across the
  whole view, and Albatosses and diving Beezos arrive from its edges. The life meter moves to the
  edge, and the pause box (with its undimmed window) is centred in the view. Subspace, and the
  fades into and out of it, stay 4:3; the end of a level switches width once the screen is dark.
* **SMB3** — enemies and moving platforms spawn across the whole wide view and move as soon as
  they are in it. On auto-scroll levels, where the scroll speed is known, anything heading
  into view is kept on the console's timeline: it comes in from the edge of the picture already
  moving, and is exactly where the 4:3 game would have it from the moment that game spawns it
  (1-4's drifting platforms cross the gaps at their original times). The game has room for only
  five level objects at a time, and the wider view shows more of them: objects still waiting to
  come into view don't hold a place, what the 4:3 game has at that moment always gets one, and
  objects that find no room come in as soon as there is. An object about to come into view
  takes a place from one out of sight that the 4:3 game has already removed (behind the player)
  or that is further out, and the one nearest to coming into view goes first; until then the
  objects kept behind the player held every place, and those ahead appeared only at the 4:3
  spawn point, about 100 px inside the wide picture. Where the console itself is short of room
  (the tank levels) or the margin behind still shows the objects holding the places (World 7's
  pipe-plant level), an object can still appear inside the wide picture rather than at its
  edge. Objects the 4:3 game has always get a place first, on auto-scroll and normal
  levels alike, taking it from one that only the margins show (one out of sight first) or that
  the 4:3 game has already removed. Walking enemies simply start where they appear, so they may
  be further along than on the console. The playfield margins are drawn from the level data in
  RAM as it was at the frame's vblank, which is what the game's own tilemap shows that frame, so a
  block an enemy carries off or a coin collected changes on the same frame in the margins as in
  the 4:3 picture; the status bar stays centred on the screen. Vertical areas show their scenery in the
  margins. Single-screen rooms are wide too, centred: the boss rooms, the bonus rooms behind
  pipes, the hammer brothers' and the King's room, with black either side: each is a set piece,
  and made up from its edges it showed things it doesn't have (a second throne, the hammer
  brothers' blocks twice). The airships' boss rooms instead carry on: the cabin's log walls
  and beams as wide as the view, the deck's posts in front of them. The mushroom house keeps its own shape, with black either side,
  and so does the world map (it stays the wide shape between the map and the levels, the
  picture never switching to 4:3 and back).
  The spade game's black surround carries on. The battle game's arena wraps around, so its
  pipes, platforms and floor carry on off both sides, dimmed (the "Dim Wrap-Around Sides"
  option), with the backdrop pattern continuing; the round panel between rounds carries its
  gold border on, undimmed. Objects in the margins are
  spawned when a level loads, as they would be if they were on screen (two sharing a spot
  included). A level opens with its circle reaching out to the edges of the wide picture, and
  the picture goes wide while the screen is still black. A door or pipe that moves the camera
  without the screen going dark (the World 1 fortress's door) spawns only what the new view
  shows, not everything between the two places (it used to bring the fortress's hidden Warp
  Whistle chest out at the door). Levels that scroll a lower band separately (the World 1
  fortress's moving floor, the tank levels' ground) keep that band joined to the margins. The
  level's own controllers (the objects that
  start, steer and stop auto-scrolling, or switch a level's behaviour, as in the hand-trap and
  World 4 levels) act exactly when they do at 4:3, however much further the wide view reaches,
  so the game's camera always follows the console's path. After the goal card the player walks
  on past the level's end to the edge of the wide picture (at 4:3 he walks 32 px off the
  screen; the level ends at the same moment either way). On an auto-scroll level, where the
  view shows past the level's start or end, the player is held at the level's start or end
  instead of the view's edge, and stands still there (he used to drift past the end and die in
  the cloud treasure areas while the camera was still on its way). Enemies brought in to fill the margins are set up from the side they come in on.
* **SMB2 and SMB3** keep their own camera. In SMB2, at the ends of a level the view stops at
  the level edge rather than showing space outside the level, so the player can walk right up to
  the display edge. While the view is held there, the slower background layers are shifted to
  match it (rounded exactly as the game rounds them), so the parallax stays correct. SMB3's
  view stays centred on the game's screen, as at 4:3, at a level's start and end too, auto-scroll
  levels included: the goal area, an airship's end and the boss rooms sit in the middle. The
  level carries on past its ends: its floor, ceiling or sky as they repeat there (checkered
  floors stay checkered, a floor's end piece isn't repeated), and the scenery between them,
  where it reaches the end, mirrored about it, so a slope becomes a hilltop and bushes, posts
  and hills go on as they look (where the end is plain, it carries on plain, and where it is a
  wall, as the fortress boss rooms end, it carries on as a wall; where it is a cliff, as 2-2
  ends, the ground carries on solid beyond it; where a level ends at a solid wall from top to
  bottom, as 3-5 and the World 4 tower levels do, the wall carries on, a pipe through it with
  it; where a platform's own edge is at the level's start, as 3-9's big bolted blocks are,
  there is open sky beyond rather than a mirrored copy). Plain textures
  such as dirt carry on unturned; coins and blocks are never shown there, nor anything the
  level has only once (a pipe's rim, the goal's card frame). Inside a level, a wall the player
  can't get past (solid all the way down the screen, as the fortresses' sections and boss rooms
  are walled off) is carried on over what lies beyond it, rather than showing the next part of
  the level or what moves there; and a room walled in on both sides (the bonus rooms down the
  pipes in 2-1, 4-5 and 3-9, the last with walls only a block or two thick) is centred on the
  display. The "COURSE CLEAR!" message hides the layer it is drawn on in the margins (3-7's
  clouds showed there). SMB3's layer speeds are measured as the
  level scrolls; a newly measured speed never makes a layer jump.
* **SMB3 auto-scroll levels** (such as 1-4 and the airships) use the whole wide picture: the
  player can run ahead to the display's right edge and is pushed by its left edge, exactly as the
  4:3 game does with its own screen edges (and is crushed if the scroll pins him against a wall),
  and enemies there can be stomped (and can hurt). The
  picture scrolls exactly as steadily as the original: it stays centred on the game's screen,
  through pauses on the way (the W6 airship, 7-4), to the level's end. Near the start, the
  player can walk back to the level's beginning (off an airship's stern too, as off any
  ledge). Objects heading into view are kept on the console's timeline from the
  first frame, while the scroll is still picking up speed. When 5-9's camera jumps ahead, what only
  the margins showed of the part left behind goes with it. The pause menu is centred on the
  display.
* The title screens are wide too:
  * SMB1's and The Lost Levels' title is the start of 1-1 with the logo over it. It is shown
    centred, as that level would be. When the demo starts scrolling, the view eases back to the
    level's own layout.
  * SMB2's title and story, and SMB3's title, are pictures drawn 256 px wide. Each layer is
    carried on into the margins from the picture's edges, line by line.
    * Where a pattern repeats there (red border, stage curtain, checkered floor), the repeat
      continues.
    * SMB3's hills at the edges are mirrored as far as each hill reaches, so the half-hills
      become whole groups ending in their own outline.
    * Elsewhere the edge pixel carries on.

  * SMB2's character select is framed by a lattice of diamonds, which carries on over the
    margins with its colours alternating, as in the frame.
  * SMB2's level cards ("WORLD 1-1") have a blue background with small emblems scattered over
    it. The margins get the same emblems, read off the card, at the same density. Emblems the
    card's blank edge columns cut in half are made whole.
  * The select stays wide while the card wipes in over it.

  The game carousel is wide from the moment it starts to appear. The All-Stars title is wide too:
  the sky and the pale band go on in their colours, and the checkerboard floor carries on in
  perspective, each row's tiles continued at their width and in their colours (read off the tiles
  at both screen edges; the floor is symmetric about the middle). The cast stays as drawn. As the
  title dissolves into the game select (black triangles drawn in ring by ring from the edges in),
  the margins dissolve with the outermost ring, in its triangles. World maps and other full-screen compositions stay 4:3, with the chosen side-border
  style.

## Development tools
`build\Release\smastool.exe` is a headless runner/debugger used to reverse-engineer the
games (see `tools/SMASTOOL.md`); findings are documented in `docs/re/`.

### Source layout
| Folder | What |
|---|---|
| `src/core/` | The SNES: 65C816, SPC700 and DSP, PPU (with the widescreen and high frame rate parts) |
| `src/game/` | The four games' widescreen, achievements, cheats, level select and co-op |
| `src/app/` | The application, the same on both systems: `app.cpp` (launcher, game, menu pages, achievements), menus (`menu.cpp`), launcher, music player, settings, drawn on `canvas.h`; `sys.h` for files and paths. Windows: `app_win.cpp` (window, frame pacing, test modes), `video.cpp` (Direct3D 11), `canvas_d2d.cpp` (Direct2D / DirectWrite), `audio.cpp` (XAudio2), `input.cpp` (XInput / DirectInput) |
| `src/platform/android/` | Android: `app_android.cpp` (SDL window, events, pacing, ROM picker), `video_gles.cpp` (OpenGL ES picture and canvas, text from Android's own font), `audio_sdl.cpp`, `input_sdl.cpp`, `touch.cpp` |
| `android/` | Gradle project; `SmasActivity.java` adds the ROM picker and the UI font rasteriser to SDL's activity |

The Android build uses SDL2 by Sam Lantinga and contributors (zlib licence, `third_party/SDL2-2.32.10-src`).
The launcher's title is set in Luckiest Guy by Astigmatic (Apache 2.0 licence, `third_party/fonts`), copied beside the exe and into the Android app.
