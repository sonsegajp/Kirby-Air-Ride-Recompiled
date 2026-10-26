# The 8-player developer build (docs/MORE_PLAYERS.md): the game regenerated with config/GKYE01/patches_8p.txt into
# build/gen-8p, built in build/8p-agent. Usage: tools/build8p.ps1 [-NoGen] [-Trace LO:HI]
param([switch]$NoGen, [string]$Trace = "")
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'devenv.ps1')
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
if (-not $NoGen) {
    $a = @('--dol', 'orig/main.dol', '--rels', 'orig/rel', '--names', 'config/GKYE01/names.txt', '--out', 'build/gen-8p',
           '--overrides', 'config/GKYE01/patches_8p.txt')
    if ($Trace) { $a += @('--trace-mem', $Trace) }
    & build/windows-clang/sa2recompile.exe @a
    if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE }
}
if (-not (Test-Path build/8p-agent/build.ninja)) {
    cmake -S . -B build/8p-agent -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
        "-DSA2_GEN_DIR=$root/build/gen-8p" -DKAR_GAME_8P=ON
    if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE }
}
cmake -S . -B build/8p-agent -DKAR_GAME_8P=ON | Out-Null
cmake --build build/8p-agent --target kirbyairride kirbyairride_game
$code = $LASTEXITCODE
Pop-Location
exit $code
