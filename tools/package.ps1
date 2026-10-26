# Assembles a release from the build: dist\KirbyAirRideRecompiled\ (the program, its libraries, the bundled
# compiler, readme and licences) and dist\KirbyAirRideRecompiled-<version>-windows-x64.zip.
# Nothing of the game goes in - neither its data nor its code, translated or not: the program builds
# kirbyairride_game.dll from the player's own disc at first run (src/runtime/setup.h), and the game reads the disc.
#   tools\package.ps1 -Version 1.2.0
# Needs build\llvm\clang.exe, lld-link.exe and LICENSE.TXT: from the official LLVM 19.1.5 release
# (LLVM-19.1.5-win64.exe, unpacked with 7-Zip: bin\clang.exe, bin\lld-link.exe; llvm/LICENSE.TXT).
param([string]$Version = "1.2.0")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\windows-clang'
$llvm = Join-Path $root 'build\llvm'
$dist = Join-Path $root 'dist'
$out = Join-Path $dist 'KirbyAirRideRecompiled'
foreach ($f in 'clang.exe', 'lld-link.exe', 'LICENSE.TXT') {
    if (-not (Test-Path (Join-Path $llvm $f))) { throw "build\llvm\$f is missing (see the top of this script)" }
}
# the Visual C++ runtime the program uses, beside it (players without the redistributable installed)
$crt = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory |
    Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw 'the Visual C++ redistributable files (Microsoft.VC143.CRT) were not found' }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force (Join-Path $out 'tools\llvm') | Out-Null

# the runtime and its import library (the game module links against it)
foreach ($f in 'kirbyairride.exe', 'kirbyairride.lib') { Copy-Item (Join-Path $bin $f) $out }
# its libraries - never the developer build's game module
Get-ChildItem $bin -Filter *.dll | Where-Object { $_.Name -notmatch '^kirbyairride_game' } | ForEach-Object { Copy-Item $_.FullName $out }
foreach ($f in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll', 'msvcp140_atomic_wait.dll') { Copy-Item (Join-Path $crt.FullName $f) $out }
foreach ($f in 'clang.exe', 'lld-link.exe', 'LICENSE.TXT') { Copy-Item (Join-Path $llvm $f) (Join-Path $out 'tools\llvm') }
Copy-Item (Join-Path $root 'LICENSE.md') $out
Copy-Item (Join-Path $root 'res\THIRD_PARTY_NOTICES.txt') $out
Copy-Item (Join-Path $root 'res\README.txt') $out
Set-Content -Path (Join-Path $out 'Kirby Air Ride (8 players).bat') -Encoding ASCII -Value @(
    '@echo off',
    'rem The 8-rider build (8 players online in City Trial): built from your disc the first time.',
    'start "" "%~dp0kirbyairride.exe" --8p %*')

# nothing translated from the game, nothing generated from it, no player's files
$bad = Get-ChildItem $out -Recurse -File | Where-Object { $_.Name -match '^kirbyairride_game|\.obj$|^dol_|\.dol$|^disc\.txt$|settings\.ini$|\.raw$' }
if ($bad) { throw "game code or a player's file in the package: $($bad.Name -join ', ')" }

$zip = Join-Path $dist "KirbyAirRideRecompiled-$Version-windows-x64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# (Windows' tar writes a standard zip; PowerShell 5.1's Compress-Archive stores backslashes in entry names)
& (Join-Path $env:SystemRoot 'System32\tar.exe') -a -c -f $zip -C $dist KirbyAirRideRecompiled
if ($LASTEXITCODE) { throw 'tar failed' }
Get-Item $zip | ForEach-Object { "{0} ({1:N0} MB)" -f $_.FullName, ($_.Length / 1MB) }
