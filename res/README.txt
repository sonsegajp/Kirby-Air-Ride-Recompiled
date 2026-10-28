Kirby Air Ride: Recompiled
==========================

Kirby Air Ride (GameCube) running natively on Windows: the game's program, recompiled to C++ on your own
computer from your own disc. Nothing of the game is in this download - no code, no data.

You need
--------
- Windows 10 or 11, 64-bit, and a graphics card with Direct3D 12 or Vulkan.
- Your own Kirby Air Ride disc image, USA (GKYE01): .iso, .gcm, .ciso, .rvz or .wia.
- About 1 GB free for the first-run setup (its work files are deleted when it is done; the game it builds is
  about 55 MB).

Playing
-------
1. Unzip this folder somewhere of your own (Documents, Desktop - not Program Files).
2. Run kirbyairride.exe.
3. The first time it asks for your disc image (or drop the file on the window), then builds the game from the
   program on it: it checks the disc, recompiles the game to C++, and compiles it with the compiler in
   tools\llvm. That takes a few minutes, once. Then the game starts.
   The disc you chose is remembered in disc.txt; delete that file to choose another.

8 players online (City Trial): run "Kirby Air Ride (8 players).bat". The first time, it builds the 8-rider build
(a few minutes more). For 8-player online every player in the lobby uses it. Offline it plays as the game does
(up to 4 riders).

Controls: a gamepad, or the keyboard: WASD stick, Space A, Left Shift B, F X, C Y, Tab Z, Q/E L/R, Enter START,
arrow keys D-pad, IJKL C-stick. F1 opens the menu (graphics, audio, online settings).

City Trial maps
---------------
Custom City Trial maps are mods: each in its own folder in mods\maps (the Hyrule Field download unzips there).
Choose one in City Trial's Game Settings, "Stage". Online, the host's map is sent to everyone in the lobby.
The map editor (its own download: unzip it into this folder) makes them from a 3D model (.gltf .glb .obj .fbx
.dae .smd): spawns, areas with their music and minimap, warps, event points, fog and sky; Build makes the mod,
Play starts the game on it. It reads the city's files it needs from your disc through the game.

The disc
--------
The recompiler is made for the USA release's program: main.dol, 5,514,016 bytes,
SHA-1 977533bb96e6a16e127946396489be1b4164ac42. Setup checks it and says so if your disc holds another one.

If something goes wrong
-----------------------
setup.log (beside the program) says what the first-run setup did; kirbyairride.log what the game did.
Reports: https://github.com/sonsegajp/Kirby-Air-Ride-Recompiled/issues

Licences
--------
Kirby Air Ride: Recompiled is under the PolyForm Noncommercial License 1.0.0 (LICENSE.md). The software it
uses, and the compiler it bundles, are listed with their licences in THIRD_PARTY_NOTICES.txt.
Kirby Air Ride is (c) Nintendo / HAL Laboratory. This project is not affiliated with or endorsed by them.
