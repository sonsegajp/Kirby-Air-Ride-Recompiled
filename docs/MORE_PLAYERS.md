# 8 riders

8 riders in City Trial, offline and online (one player per PC), with the HUD, the machine stats, the Stadiums and
the results for 8. Each PC shows one rider's view, so split-screen beyond 4 isn't needed. Air Ride with more than 4
is next. How it was done, below.

## The game's limits (main.dol, GKYE01)
- **Player slots:** `Gm_Player_NumMax` is 5. Slots 0-3 are the riders; slot 4 is King Dedede in the VS. King Dedede
  stadium (`setStadiumSetting` writes `ply_desc[4]`; a few calls use slot 4 by constant).
- **The player table:** 5 × 0x90C bytes at `8055A9F0`. It is reached only through the player accessors
  (`8022B554`-`8022E800`), and 107 instructions make its address (`lis`/`addi`).
- **The game state** (`805359D8`, `gmGetGlobalP`):
  - Per-slot arrays (sized 5) and per-rider arrays (sized 4) are packed between other fields, for example:
    - `ply_desc[5]`, stride 0x30, at +AC8;
    - the race and game logic's arrays at +848..+8CC and +A30..+A88;
    - the City Trial machine stats at +3A0..+46C;
    - the stadium results at +BC8..+CD0.
  - Many loops over them are unrolled by the compiler.
- **The 3D HUD's state** (`805578B8`, `3D_GetData`): about 60 per-rider tables of HUD objects, sized 4 or 5.
- **Network input:** HAL's lbnet token holds 4 console slots × 4 pads. HSD has 20 pads: 0-3 local, then
  4 + 4·console + port.

## Approach
- **New counts:** 8 rider slots and 9 slots in all; King Dedede moves to slot 8.
- **Patches at code-generation time** (`sa2recompile --overrides config/GKYE01/patches_8p.txt`; format in
  `src/recompiler/overrides.h`):
  - `move`: an instruction's address moves to new storage. Whole symbols (the player table) move where their
    address is made; arrays inside structs move per access (every load or store that reaches them).
  - `imm`: raises loop bounds (4 → 8, 5 → 9).
  - Unrolled code and the HUD and results are replaced by native code (`interpReplace` handlers) where needed.
- **New storage:** at `81808000`, past MEM1's 24 MiB but inside the host's 32 MiB guest window (`src/runtime/ctx.h`).
- **The patch file is generated** (by research tools not in this repository yet: they read a symbol map that is
  not ours to share; `config/GKYE01/patches_8p.txt` is their output): `gen8p.py` builds it from a whole-program scan
  (`plyscan.py`: abstract interpretation that finds player-indexed accesses, player loops, and pointers handed on)
  plus the list of arrays.

## Tests
- `SA2_SOLO_TEST="riders=8;citysecs=60;ct.stadium=kirby_melee"` (`lansession.cpp`): City Trial on one PC.
  - Player 1 is human; the other riders are CPUs, filled in after Player Entry.
  - Menus, the Stadium card, the stats screen and the results go on by themselves.
  - The program exits after the match.
- `SA2_COVER=<file>` (`cpu.cpp`): every function that ran, with its mode and scene masks. This shows which code a
  match uses.
- `SA2_MEMTRACE=<file>` with a `--trace-mem` build: finds accesses that still reach the old storage.

## Where it stands (2026-10-25)
- **Layout:** 10 slots. Riders are in 0-3 and 6-9; King Dedede keeps slot 4; slot 5 stays empty, because the game
  uses 5 for "nobody".
- **A solo City Trial with 8 riders runs end to end:** City, Kirby Melee, results.
- **Patches:**
  - About 1,800 relocated instructions and 344 changed words (`scratch_kar/eight/arrays8p.py` → `gen8p.py`).
  - Native replacements and fixes are in `src/runtime/morepl.cpp`:
    - preload list reset;
    - rider spacing;
    - camera distances;
    - start spots for riders 6-9;
    - the item manager moved to `81900000` (its lists are kept per collecting slot);
    - memset of moved arrays.
- **Bugs found the hard way:**
  - A float table at gm +AA8 that scales stats per slot. Unmoved, it gave riders 6-7 NaN machines.
  - The views' 5-byte "given a camera" arrays on the stack.
  - The item manager's 5 groups.
  - The CPU brain table (`cpGlobal`, 5).
  - 71 `player < Gm_Player_NumMax` checks.
- **Loops still at the game's counts (to study):**
  - the results screens (80043B30-80046500);
  - the City stage code (800EF000-800F4000);
  - the CPU AI (80262000-80280000);
  - the preload compare (80232514).
- **Guards that log and skip instead of halting** (`morepl.cpp`): the camera look-at, the surface position, sound
  track release and item removal. None fired in the full 8-rider run; they remain until each cause is known.
- **Tools:**
  - `SA2_8P_DEBUG` (slot log; at the results scene, every per-slot race and Stadium array, moved and original);
  - `SA2_WATCH=addr:len` with `SA2_WATCH_NAN` (first NaN written into a range, with the guest stack);
  - `tdiff.py` (instructions that touch new static addresses with 8 riders vs 4);
  - `SA2_SOLO_TEST="...;allcpu=1"`: player 1 a CPU too, so every Stadium plays out and ends by itself;
  - the hang report logs the game's file preload table (`morepl::stallDump`).

## The Stadiums with 8 (2026-10-25, later)
- **Every Stadium run** with `riders=8;allcpu=1` (`scratch_kar/eight/solo.ps1`, each run its own exe and shader cache).
- **Fixed:**
  - the machine respawn's start spot (`f_800D4FB8`, asserted `num < grGetStartposNum()` in the Drag Race): riders 6-9
    take riders 0-3's;
  - start spots in a Stadium: its 4 spots are one line about 5 units apart, so riders 6-9 form a second row 12 units
    behind (beside them, 12 units to the side, they overlapped other riders and stuck or retired them); the city keeps
    the side shift;
  - 10 more player checks the message scan missed (`Ply_GetKONum` and neighbours: their message comes from a base
    register), and the "player not found!" check after `f_800C3510`'s slot search;
  - three per-slot arrays at gm +5B4, +5DC (8 bytes) and +634 (0x50: the race record now and before) - written by
    Game_Think's slot loop, unmoved they ran over gm +7C4..A34 (the mode, the scene descriptor at +814: High Jump's
    crash, calling a float);
  - four more race arrays autoarr found over gm (+8CC, +8E0, +91C, +95A: the finish's records);
  - the game manager's data (GameManagerGObj_Create: 0xE4 bytes from the pool at 80537400, pointer at r13+0x490):
    per-slot arrays at +15, +6C, +80, +94, +A8, +BC, +D0 inside a heap object - unmoved, riders 6-9 wrote past them
    (Kirby Melee's effect-list asserts, Destruction Derby's call to 0xD) and riders 1-4 scored nothing in Kirby Melee.
    `HLE(HSD_ObjAlloc)` answers that one allocation with a fixed address (81900800; the pool's counts kept), and
    `gen8p.py`'s `LD_BASES` treats accesses through r13+0x490 as static ones there, so the arrays move like the rest.
- **All eight Stadiums now run with 8 riders** (`riders=8;allcpu=1`, two full passes in a row), each ending on the
  8-rider results screen.
- **Debugging tools added:** `SA2_WATCH_EXCEPT=lo-hi[,lo-hi...]` (writes by those guest functions not reported; each
  report names the writing function), `SA2_WATCH_GUESTONLY=1` (host code's writes skipped), `SA2_WIDE_LOG=s<scene>:<n>`
  (16:9 placements n frames into a major scene).
- **Results:** the game copies each Stadium's results for 4 from per-slot arrays that the 8-player build has for all:
  places from the ranking +848 (0-based), times +8B8 (frames; shown as hundredths rounded up), points +A38 (Target
  Flight, Kirby Melee, Destruction Derby), distances +A4C (feet; in a race, how far along). The ranking itself is right
  only for the first 4 slots, and the game's own results screen reads storage the relocation misses (all zeros), so
  `src/runtime/results8.cpp` draws the whole results screen for 8: ranks by the Stadium's rule, the winner's Kirby.

## The HUD with 8 (2026-10-25, later)
- **Race place badge** (32x40 CI8 art for 1-4 only: red, orange, green, grey): 5th-8th are drawn in the grey "4"'s
  style for its palette (`src/runtime/place_digits.inc`, made from the SIL OFL font RocknRoll One) and swapped in
  for the "4" by content when the rider with the view is 5th or lower (`gx.cpp` `textureSource`).
- **Live ranking** (`f_800108B0`): one read of "has the other rider finished" (gm +852) was missed by the scan
  (`r3` lost at a branch join); it is moved by hand in `arrays8p.py`.
- **Riders out of the race** (gm +A30: fallen, or out of time) are ranked from the bottom; Game_Think's nine
  unrolled counts see only slots 0-3, so `morepl::frame` places each new one again, counting all riders.
- **High Jump / Air Glider opponents' list**: its builder (`3DHud_CreateHighJumpOpponentHeightElement`) stopped at
  slot 4 (`imm 8012BB18 10`); the rows' numbers reached only some opponents (the rest shared row 0), so
  `morepl::frame` numbers every rider without a view; with more than 4 riders the rows go up from the first, 3
  apart (sdata2 805E002C, read only there), into the empty space above.
- **The machine stats after the city** and **the Stadium results**: drawn for 8 by `src/runtime/results8.cpp`.
- **Still 4-only:** the time-up flag (an unrolled block marks slots 0-3 out when time runs out; the results screen
  ranks for itself, so only the HUD of riders 6-9 is affected).

## Online with 8 (2026-10-25, later)
- **Seats** (`src/runtime/lan.h`): the game's LAN mode knows 4 consoles, its input token 4 slots of 4 controller ports.
  PCs 0-3 are consoles 0-3 (port 0); PCs 4-7 sit on spare ports - console 0's ports 2, 3, 1 and console 1's port
  1, the raw network pads 6, 7, 5 and 9. Riders read the pad of their controller index (ply_desc +6, player +0x8D),
  so riders 6, 7, 8, 9 are given exactly those pads and need no other input path.
- **Connecting** (`lansync.cpp` `connect`, replacing the net library's 804876A0): every PC knows the lobby's order,
  so the console list (count, place, ring neighbours, an address each) is filled in at once - no SSDP handshake,
  which could never take 8.
- **The lockstep** (`lansync.cpp`): up to 8 peers; each round the game gets slots 0-3 from PCs 0-3 and, during a
  stage only, PCs 4-7's controllers in their seats (in the menus a controller showing up on another port is another
  player to wait for). The session id is the match's seed.
- **Riders** (`lansession.cpp` `ctSetup`): Player Entry enters consoles 0-3 (each PC presses A on its own controller,
  as a person would - the LAN Player Entry reads the input sets, not the pads afterwards); riders 6-9 are added for
  PCs 4-7 on their seats' pads. The slots the 8-player build's raised loops left with copies (a CPU in 6, a "human"
  on pad 8) are cleared - in 2-4 PC sessions too.
- **Screens** (`lansession.cpp` `lanViews`, replacing f_80020188): each PC keeps the screen of the rider on its own
  controller only. The game's version is unrolled for 4 slots and, in the 8-player build, its 2-console branch was
  split by the relocation (writes past the views): every PC drew a split screen of all humans.
- **The players' HUD** (3D_InitMapAndPrompt 80113150): its 12 loops over slots 0-3 raised to 10, so a PC whose rider
  is in slots 6-9 has a speedometer, timer, place and the rest.
- **Lobby** (`server/`, `online_menu.cpp`): matches of 2-8 (4 unless the host says); above 4, City Trial only
  (Air Ride is not converted for 8 yet).
- **Tests**: `scratch_kar/eight/lanN.ps1 -N <2-8>`: N copies on the loopback LAN in one session (`SA2_LAN_CHECK` seeds
  compared); `SA2_NO_DRAW=1` (copies not rendered but for dumped frames) and `SA2_LAN_LOST_MS` let 8 run on one PC.

## Online with 8 in step (2026-10-26)
Eight PCs online through a dedicated server now play a whole City Trial match in step - the city, the machine stats,
the Stadium and the results (two full matches, eight copies on one PC, every 120 rounds compared). What parted them:
- **Rounds overwritten** (`lansync.cpp`): a PC made a round of its own every retrace even while its game was loading
  and took none; past 512 (`kHistory`) those overwrote rounds it had not queued yet, and it ran on other input. Now
  a PC makes at most `kMaxAhead` (90) rounds past its queue - the others wait for one that is loading, as the game's
  own ring would - keeps sending meanwhile, and a console counts as lost only when nothing at all is heard from it.
- **Disc reads** (`hle_dvd.cpp`): a read ended when this PC's disc image had it, so the game saw files arrive in
  different steps. In a lockstep a read's end is held until the main thread's next safe point (or until it sleeps
  waiting for it): the same point of the game's run everywhere. Music and movie streams are left alone.
- **Sets flushed at a scene's start** (`lansync.cpp`): a scene's loop empties the input queue as it begins (8007CBB0);
  a set queued while the scene loaded was dropped on one PC and used on another. Sets are queued only once the loop
  runs.
- Also: the online menu holds the game on Select Mode (it timed out to the title under a long lobby wait); the sound
  library's assert on a late music stream (synth.c:1590) is skipped - the music drops out instead of the game stopping.

## Status
- [x] Scan, coverage, solo test harness
- [ ] Player table relocated to 9 slots; slot loops raised
- [ ] Game state arrays
- [ ] HUD
- [ ] Unrolled code
- [x] Spawn points for riders 5-8 (city: aside; stadiums: a second row)
- [x] Results for 8 (City Trial: src/runtime/results8.cpp)
- [x] HUD: the race place 5th-8th, the High Jump / Air Glider list for 7 opponents, the machine stats screen for 8
- [x] Online: up to 8 PCs (seats on spare ports, the connection filled in, riders 6-9, a screen each, the HUD)
- [x] Online with 8 in step through a whole City Trial match (rounds capped, disc reads and scene starts in step)
- [ ] Two matches in a row with 8 (a rematch in the same lobby)
- [ ] Air Ride with more than 4 riders
