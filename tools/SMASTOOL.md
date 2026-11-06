# smastool - headless SNES runner / debugger

```
smastool <rom.sfc> -c "cmd; cmd; ..."     (commands separated by ';')
smastool <rom.sfc> script.txt              (one command per line, '#' comments)
```

The emulator starts from power-on. Use `load` to start from a saved state.
Frame counts continue from the state's frame number. All addresses are hex.
WRAM is `7E0000-7FFFFF` (also mirrored at `000000-001FFF`).

## Running
| command | effect |
|---|---|
| `run N` | run N frames with the currently held buttons |
| `rewindtest PLAY BACK` | check the rewind buffer: play PLAY frames (held buttons, A pulsed), rewind BACK snapshots as the app does, replay, and compare with the first run |
| `hold BTN` | set player-1 buttons, e.g. `hold right+b`, `hold none` (b,y,a,x,l,r,up,down,left,right,start,select) |
| `hold2 BTN` | player-2 buttons |
| `hold3 BTN` .. `hold5 BTN` | players 3-5's buttons (co-op: they drop in with `start`) |
| `present HEX` | which players are there (bit per player; online play's session, default 1F) |
| `hash` | frame, CPU cycles, scanline, a hash of WRAM and the APU's cycles (comparing two runs frame by frame) |
| `press BTN N M` | hold BTN for N frames, release, run M more frames |
| `save PATH` / `load PATH` | save state / load state |
| `wide E` | request widescreen margin E pixels per side (game layer may clamp) |
| `bench N` | run N frames and print speed |
| `reset` | soft reset (console Reset button) |
| `warp G W L` | level select: G = 1 SMB1, 2 Lost Levels, 3 SMB2, 4 SMB3; world / level 1-based (SMB3 level: 1 = map, 2.. = levels then fortresses) |
| `cheats HEX` | set the cheat bit mask (bits 0-7 SMB1, 8-15 LL, 16-23 SMB2, 24-31 SMB3) |
| `palette N` | character colour variant (0 = original) |
| `ach on` / `ach off` | achievement checks from now (a run that counts); each event prints `ACH unlock|mark|progress <id> <value> f<frame>` |
| `verifywide` | SMB2: compare the RAM-rendered layers with VRAM over the visible area. SMB3: prints the playfield check of the last frame since the previous `verifywide` (every line, at that line's own scroll, across the 4:3 part) and arms it for the next: use `verifywide`, `run 1`, `verifywide`. Tiles that draw the same pixels count as matching; up to 12 mismatched tiles are listed with their level position and metatile. It also runs at `wide 0` (lines scrolled with the camera) |

| `ext` | print the widescreen margins of the last frame drawn and the frame number: `ext L R frame` |
| `seamscan on TOP BOTTOM PATH` / `seamscan off` | after every frame, log widescreen picture faults to PATH: `S frame side run y ctl moved`, a vertical break `run` rows long from `y` exactly at an edge of the 4:3 part (`ctl`: the longest 2-8 px either side; `moved`: the 4:3 column there changed), and `B frame side rows y`, margin rows black from the display edge in (24 px or more) where the 4:3 row isn't. Rows TOP..BOTTOM only (leave out the status bar) |

## Inspecting
| command | effect |
|---|---|
| `shot PATH.png` | screenshot (includes widescreen margins when active) |
| `peek ADDR N` / `poke ADDR V` | read / write memory (no side effects) |
| `regs` | CPU registers + frame number |
| `disasm ADDR N M X` | disassemble N instructions (M/X = 1 for 8-bit acc/index) |
| `ppu` | PPU registers summary (BG maps, scroll, windows, color math) |
| `bgshot LAYER PATH.png` | render a whole BG tilemap (layer 1-4) with current palettes |
| `oam` | list on-screen OAM entries; `sx=` is where the PPU draws each one relative to the 4:3 screen (`true` when the game layer supplied its true position) |
| `dump PREFIX` | write .wram .vram .cgram .oam .aram .sram files |
| `apu` | SPC700 registers, APU ports, DSP registers |
| `game` | current game detected by the game layer |

## Tracing / searching
`trace`, `watch`, `rwatch`, `vramlog` and `ports` run their N frames themselves, with the
buttons held at that point; a `run` after them adds more frames.

| command | effect |
|---|---|
| `trace PATH N [LO HI]` | log every CPU instruction for N frames (optionally only PCs in [LO,HI]); note: frame boundaries are at V=0, NMI runs at V=225 |
| `watch LO HI PATH N` | log CPU writes to [LO,HI] (PC, value, scanline) for N frames |
| `rwatch LO HI PATH N` | log CPU reads of [LO,HI] for N frames |
| `vramlog PATH N [LO HI]` | log runs of VRAM word writes for N frames, with a camera value (see `camaddr`) |
| `camaddr HI LO` | RAM addresses printed as `cam=` in vramlog (default 7E071A 7E071C) |
| `ports PATH N` | log CPU writes to APU ports $2140-$2143 (value + PC) for N frames |
| `snap NAME` / `diff NAME [LO HI]` | WRAM snapshot / list changed WRAM bytes (offsets 0-1FFFF) |
| `find VAL [LO HI]` | list WRAM offsets holding byte VAL |
| `spctrace PATH N FRAMES` | log the first N SPC700 instructions executed over FRAMES frames |
| `rec` / `wav PATH` | start recording audio / write WAV (32040 Hz) and print peak/RMS |

## Game facts
* `$7FFF00` selects the running title: 0 = All-Stars menu, 2 = SMB1, 4 = Lost Levels, 6 = SMB2, 8 = SMB3.
* NMI dispatch per title: SMB1 `JSL $038003`, LL `$0D8003`, SMB2 `$118003`, SMB3 `$208003`.
* Game code banks: SMB1 `$03-$05`, LL `$0D-...`, SMB2 `$11-...`, SMB3 `$20-...`.
* SMB1 keeps the NES RAM layout for `$0700-$07FF` (e.g. `$0770` OperMode, `$075A` lives,
  `$075F` world, `$075C` level, `$071A-$071D` screen left/right page+x, `$0725/$0726` area
  parser page/column), block buffer at `$0500`/`$05D0`.
