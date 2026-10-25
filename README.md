# Kirby Air Ride: Recompiled

A native PC version of **Kirby Air Ride** (GameCube, USA, `GKYE01`), made by static recompilation: the game's
PowerPC code is translated to C++ ahead of time and runs natively, with its own runtime in place of the console's
hardware. No emulator.

**Nothing of the game is included: neither its data nor its code, translated or not.** You need your own copy of the
game (a USA disc image). The recompiler turns the program on your disc into C++ on your computer, and the game reads
everything else from the disc as it plays.

![City Trial in 16:9](docs/images/16x9_city1.jpg)

## Features

- **Widescreen** at any aspect ratio, with the HUD kept at the screen's edges and the menus laid out for it; or the
  original 4:3.
- **Smooth motion** on 120/144 Hz displays (in-between frames), the game itself at its 60 Hz.
- **Online play** in the game's own LAN mode (Air Ride and City Trial, up to 4 consoles a match): accounts, a server
  browser, lobbies with chat and host settings, peer-to-peer connections with a relay as fallback, and a lockstep
  netcode that hides latency. Play on the official service or on a **dedicated server** anyone can run
  ([docs/ONLINE.md](docs/ONLINE.md)).
- **A settings menu** (F1) drawn with the game's own art: display, frame rate, in-between frames, volume.
- **More than 4 riders** (work in progress, developer build): City Trial with 8 riders - start spots, the Stadiums,
  the HUD (places 5th-8th, the High Jump and Air Glider lists), the machine stats and the results for 8
  ([docs/MORE_PLAYERS.md](docs/MORE_PLAYERS.md)).

## Screenshots

### 16:9

<p>
  <img src="docs/images/16x9_title.jpg" width="49%" alt="Title screen, 16:9">
  <img src="docs/images/16x9_city2.jpg" width="49%" alt="City Trial, 16:9">
  <img src="docs/images/16x9_race_start.jpg" width="49%" alt="A race's start, 16:9">
  <img src="docs/images/16x9_race2.jpg" width="49%" alt="Racing, 16:9">
</p>

### 4:3

<p>
  <img src="docs/images/4x3_title.jpg" width="49%" alt="Title screen, 4:3">
  <img src="docs/images/4x3_city1.jpg" width="49%" alt="City Trial, 4:3">
  <img src="docs/images/4x3_race_card.jpg" width="49%" alt="A Stadium's title card, 4:3">
  <img src="docs/images/4x3_race2.jpg" width="49%" alt="Racing, 4:3">
</p>

### Online

<p>
  <img src="docs/images/online_main.png" width="49%" alt="The Online menu">
  <img src="docs/images/online_browser.png" width="49%" alt="The server browser">
  <img src="docs/images/online_lobby.png" width="49%" alt="A lobby: players, the next match's settings, chat">
  <img src="docs/images/online_profile.png" width="49%" alt="A profile, with Kirby in your colour">
  <img src="docs/images/online_host.png" width="49%" alt="Hosting a server">
  <img src="docs/images/online_dedicated_settings.png" width="49%" alt="Online Settings: choosing a dedicated server">
</p>

### 8 riders (developer build)

<p>
  <img src="docs/images/8p_results.png" width="49%" alt="A Stadium's results for 8 riders">
  <img src="docs/images/8p_stats.png" width="49%" alt="The machine stats after the city, for 8 riders">
  <img src="docs/images/badge_6th.jpg" width="49%" alt="A race with 8 riders: 6th place">
</p>

## Building and playing

There is no ready-made download of the game yet (a launcher that builds it from your disc, like
[Sonic Adventure 2 Battle: Recompiled](https://github.com/sonsegajp/Sonic-Adventure-2-Battle-Recompiled)'s, is
planned). To build it yourself on Windows you need:

- Visual Studio 2022 with **C++ desktop development** and **C++ Clang tools** (clang-cl, CMake and Ninja come with it),
- Python 3.10 or newer,
- your Kirby Air Ride disc image (USA: `.iso`, `.gcm`, `.ciso`, `.rvz` or `.wia`).

```powershell
git clone --recursive https://github.com/sonsegajp/Kirby-Air-Ride-Recompiled.git
cd Kirby-Air-Ride-Recompiled
python tools\extract.py "<your disc image>" orig                      # the program, from your disc (git ignores orig\)
powershell -File tools\build.ps1 -Target sa2recompile                  # the recompiler
build\windows-clang\sa2recompile.exe --dol orig\main.dol --rels orig\rel --names config\GKYE01\names.txt --out build\gen
powershell -File tools\build.ps1 -Target kirbyairride                  # the game: runtime + the recompiled program
Play.bat                                                               # asks for your disc image the first time
```

Nothing generated from your disc ever enters the repository (`orig\` and `build\` are ignored). The 8-rider build is
made the same way with `tools\build8p.ps1` (into `build\8p-agent`).

**Online:** a build made from this repository has no official service built in (its address is set when building:
`-DKAR_ONLINE_MASTER=host:port`, or `online_master.txt`). Use a dedicated server: in the game, **Online > Settings >
Service: Dedicated Server**, then its address.

**Controls:** a controller works directly (recommended); the keyboard too. F1: settings. F11: full screen. Saves go
to `saves\MemoryCardA.raw` (a GameCube memory card image, the same format as Dolphin's); settings to
`kirbyairride-settings.ini`.

## Running a dedicated server

**Windows, nothing to install:** download `AirRideServer-win64.zip` from the
[releases](https://github.com/sonsegajp/Kirby-Air-Ride-Recompiled/releases), unzip it and double-click
**StartServer.bat** (it has Python's embeddable runtime built in). It listens on TCP 27600 and UDP 27601 and lists
this PC's addresses; over the internet, forward those two ports on your router. Players choose
**Online > Settings > Service: Dedicated Server** and type the address. It keeps its own accounts in `airride.db`.

Linux or macOS: `server/run.sh` (Python 3.10+, standard library only). A VPS, backups, bans and the other options:
[server/DEPLOY.md](server/DEPLOY.md). The protocol: [docs/ONLINE.md](docs/ONLINE.md).

## How it works

| Part | Where |
|---|---|
| The recompiler: finds every function in the game's program and translates its PowerPC code to C++ | `src/recompiler` |
| The runtime: memory, the CPU's state, the OS and hardware the game calls, audio (DSP, AX), input, the memory card | `src/runtime` |
| Graphics: the game's GX commands drawn with [Aurora](https://github.com/encounter/aurora) (WebGPU/Dawn), plus our patches (`patches/aurora`, applied at configure time) | `src/runtime/gx.cpp`, `extern/aurora` |
| Widescreen: 3D views widened, each 2D piece placed by what it is (HUD clusters to the nearest edge) | `src/runtime/gx.cpp` |
| Online: the game's network library replaced, a lockstep in place of its LAN ring, the client, the server | `src/runtime/lan*.cpp`, `airnet.cpp`, `server/` |
| 8 riders: recompiler overrides that move the game's per-player arrays to larger storage | `config/GKYE01/patches_8p.txt`, `src/runtime/morepl.cpp` |

## Licence

[PolyForm Noncommercial 1.0.0](LICENSE.md): use, change and share it, but not commercially.
[Aurora](https://github.com/encounter/aurora) (a submodule) has its own licence; `src/runtime/place_digits.inc` is drawn
with [RocknRoll One](https://fonts.google.com/specimen/RocknRoll+One) (SIL Open Font License).

Kirby Air Ride is a trademark of Nintendo; this project is not affiliated with Nintendo or HAL Laboratory. It contains
none of their code or data.
