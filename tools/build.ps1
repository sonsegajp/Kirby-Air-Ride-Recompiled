# Configure + build with Visual Studio 2022's clang-cl and Ninja.
# Usage: tools/build.ps1 [-Target <name>]
param([string]$Target = "")
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'devenv.ps1')
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
if (-not (Test-Path build/windows-clang/build.ninja)) { cmake --preset windows-clang; if ($LASTEXITCODE) { Pop-Location; exit $LASTEXITCODE } }
if ($Target) { cmake --build build/windows-clang --target $Target } else { cmake --build build/windows-clang }
$code = $LASTEXITCODE
Pop-Location
exit $code
