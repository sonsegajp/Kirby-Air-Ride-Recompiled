# First-run setup: building the game from your disc

The download contains **no game code**: none of Kirby Air Ride's code, translated or not. It contains the runtime
(`kirbyairride.exe`, our own code, which stands in for the GameCube's hardware) with the recompiler in it, and a
compiler. The game's code is translated from **your own disc** on **your own computer** the first time you run it,
and kept beside the program as `kirbyairride_game.dll` (the 8-rider build's: `kirbyairride_game_8p.dll`).

## At a glance

| | |
|---|---|
| When | The first start; again after an update of the program, or with another disc image |
| How long | About 3 minutes with 8 processor threads (almost all of it compiling); the 8-rider build as long again, the first time it is started |
| Disk space | About 1 GB while it works (in `%LOCALAPPDATA%\KirbyAirRideRecompiled\build`, removed afterwards); the result is about 55 MB |
| Memory | Up to about 1 GB per compiler running (as many run as there are threads and memory for them) |
| Internet | Not needed: the compiler is bundled |
| Result | `kirbyairride_game.dll` and `kirbyairride_game.txt` (what it was built from) beside the program; `setup.log` |

The window shows the setup before the game: first **Your Disc** (choose the disc image, or drop it on the window;
it is remembered in `disc.txt`), then **Building the Game**: the four steps, each marked done or with its
percentage, a bar for the whole setup, the time so far and an estimate of the time left. **Cancel** (or Escape)
stops it at any point; nothing half-built is kept. When it finishes, the game starts.

![Building the game](images/setup.png)

## The four steps

### 1. Checking your disc

`main.dol`, the game's program, is read from the disc image (any of `.iso`, `.gcm`, `.ciso`, `.rvz`, `.wia`; its size
is where its last section ends) and its SHA-1 compared with the one the recompiler is made for - **Kirby Air Ride
(USA), game ID `GKYE01`**:

| | |
|---|---|
| `main.dol` size | 5,514,016 bytes |
| `main.dol` SHA-1 | `977533bb96e6a16e127946396489be1b4164ac42` |

The program is what is checked, not the image: images of the same disc come in many formats and trims. Another
version (the European or Japanese release, a modified game) stops the setup with a message: the recompiler's names,
the runtime's fixes and the 8-rider build's changes are tied to this exact program.

### 2. Recompiling the game to C++

The recompiler (`src/recompiler`) runs in the program's own process:

1. **Analysis** finds every function in `main.dol` (about 20,500): from the entry point, calls, tail calls, jump
   tables, function pointers in data and the gaps between known functions.
2. **The 8-rider build** only: `config/GKYE01/patches_8p.txt` (embedded in the program) changes about 370
   instructions and moves the addresses of about 2,000 more, which puts the game's per-player arrays in larger
   storage ([MORE_PLAYERS.md](MORE_PLAYERS.md)). Its `was` lines check the program first.
3. **Names**: the SDK functions in `config/GKYE01/names.txt` (OS, GX, DVD, audio, memory card, ...) get a hook slot,
   which the runtime fills with its own implementation.
4. **Emission**: one C++ function per guest function, in 26 files plus headers and function tables - about 52 MB of
   C++.

### 3. Compiling the game

Every file is compiled with the **bundled clang** (`tools\llvm\clang.exe`, LLVM 19.1.5), as many at once as there are
processor threads and memory, largest first so the last ones finish together; at below-normal priority, inside a
job object (they end with the program whatever happens to it).

```
clang --target=x86_64-pc-windows-msvc -std=c++20 -O2 -g0 -ffreestanding -nostdinc
      -fno-exceptions -fno-rtti -fno-threadsafe-statics -funwind-tables -DKAR_GAME_DLL
      -Iinclude -Igen -w -c gen/<file>.cpp -o obj/<file>.obj
```

The module is **freestanding**: no C or C++ library and no headers but the runtime's `ctx.h`, `modules.h` and
`abi.h` (embedded in the program). The functions a compiler may call on its own are in `src/game/glue.cpp`:
`memcpy`, `memmove`, `memset`, `memcmp`, `sqrt`, `fma` (the runtime's), `__chkstk` and `_fltused`. The game's
`setjmp` is the C runtime's, which the program forwards (`_setjmp`, called with the frame as the compiler's builtin
does); unwind tables let a guest thread end by unwinding through the game's frames. So no Windows SDK or Visual
Studio is needed on your computer.

### 4. Linking the game

```
lld-link /dll /noentry /nodefaultlib /machine:x64 /opt:ref /opt:noicf
         /out:kirbyairride_game.dll @objs.rsp kirbyairride.lib
```

`kirbyairride.lib` is the runtime's import library: the module calls the runtime's functions in `kirbyairride.exe`
(hardware registers, indirect calls, the time base, traps, ...). Then the module is copied beside the program,
`kirbyairride_game.txt` records what it was built from, and the work folder is removed.

## When it runs again

`kirbyairride_game.txt` holds the program's version, the module interface version (`KAR_GAME_ABI`,
`src/runtime/abi.h`) and the disc image's path, size, date and program SHA-1. The program builds the module again
when it is missing, when the program was updated, or when the disc image's path, size or date changed - and if the
program's SHA-1 is still the same (the file was moved or renamed), step 1 is all that runs. A module built for
another interface version is refused, and the setup screen offers to build it again.

## How the pieces fit together

```
kirbyairride.exe  --builds-->  kirbyairride_game.dll   (loaded by kirbyairride.exe)
  runtime: CPU helpers,          translated game code
  interrupts, threads, DVD,      + src/game/glue.cpp
  audio, memory card, GX ->
  Aurora, window, input, online
  + recompiler, embedded headers/names/overrides; tools\llvm: clang + lld-link
```

- The program loads the module from its own folder and asks it for its `GameModule` (`src/runtime/modules.h`):
  the function tables, the SDK hook slots, the run-time override flags, the 8-rider build's code overrides, and
  pointers to the state the translated code uses all the time (its copy of the guest memory base, the
  pending-interrupt flag, the call-trace flag, the REL bases). Those live in the module, so the hot paths stay plain
  global accesses; the runtime reaches them through the pointers.
- The runtime's functions the module calls are exported from `kirbyairride.exe` (`RT_API` in `src/runtime/ctx.h`)
  and imported through `kirbyairride.lib`.

## Files

| File | What |
|---|---|
| `tools\llvm\clang.exe`, `lld-link.exe` | The official LLVM 19.1.5 Windows release's compiler and linker, unchanged. Apache License 2.0 with LLVM Exceptions: `tools\llvm\LICENSE.TXT` |
| `kirbyairride.lib` | The runtime's import library, for linking the module |
| `kirbyairride_game.dll`, `kirbyairride_game_8p.dll` | The game's code, made by the setup (never in the download) |
| `kirbyairride_game.txt`, `kirbyairride_game_8p.txt` | What each was built from |
| `disc.txt` | The disc image you chose |
| `setup.log` | The last setup: each step's time, the program's SHA-1, compiler or linker errors |

## If it stops

The screen says why, and `setup.log` has the details. The usual causes:

- *This disc image's program is not the Kirby Air Ride (USA) one*: another version or a modified game.
- *The game's folder can't be written to*: the program is in a protected folder such as Program Files. Move it to a
  folder of your own.
- *Couldn't write the recompiled code*: the drive with `%LOCALAPPDATA%` needs about 1 GB free.
- *Files the setup needs are missing*: the download was not extracted in full (`tools\llvm`, `kirbyairride.lib`).

## For developers

The developer build makes the same module with CMake from `build/gen`: the `kirbyairride_game` target, compiled with
the same freestanding flags by Visual Studio's clang-cl (a module with no `.txt` beside it is taken as a developer
build and never rebuilt by setup). To try the setup itself on a developer build, put the LLVM release's `clang.exe`
and `lld-link.exe` in `build\windows-clang\tools\llvm`. Headless runs (`--headless --iso <image>`) run the setup with
its progress on the console when the module is missing.
