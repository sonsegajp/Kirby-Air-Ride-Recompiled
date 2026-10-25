# Packs the dedicated server for Windows with no install needed: dist\AirRideServer-win64.zip holds the server
# (server\airride_master), StartServer.bat and Python's official embeddable runtime (python.org; its licence,
# python\LICENSE.txt, allows redistribution). Unzip anywhere and double-click StartServer.bat.
# Usage: tools\server\make_server_zip.ps1 [-PythonVersion 3.13.17]
param([string]$PythonVersion = "3.13.17")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cache = Join-Path $root "build\cache"
$stage = Join-Path $root "build\server-dist\AirRideServer"
$out = Join-Path $root "dist\AirRideServer-win64.zip"
New-Item -ItemType Directory -Force $cache, (Split-Path $out) | Out-Null
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\python" | Out-Null

# Python's embeddable runtime (cached)
$pyZip = Join-Path $cache "python-$PythonVersion-embed-amd64.zip"
if (-not (Test-Path $pyZip)) {
    Invoke-WebRequest "https://www.python.org/ftp/python/$PythonVersion/python-$PythonVersion-embed-amd64.zip" -OutFile $pyZip
}
Expand-Archive -Path $pyZip -DestinationPath "$stage\python" -Force
# Its ._pth file is its whole module path: add the folder above it, where airride_master is.
$pth = Get-ChildItem "$stage\python\python*._pth" | Select-Object -First 1
Add-Content -Path $pth.FullName -Value ".."

# The server
Copy-Item -Recurse (Join-Path $root "server\airride_master") "$stage\airride_master"
Get-ChildItem "$stage\airride_master" -Recurse -Directory -Filter __pycache__ | Remove-Item -Recurse -Force
Copy-Item (Join-Path $root "server\StartServer.bat") $stage
Copy-Item (Join-Path $root "server\DEPLOY.md") "$stage\SERVER.md"
Copy-Item (Join-Path $root "LICENSE.md") $stage
@"
Kirby Air Ride: Recompiled - dedicated server
==============================================

1. Double-click StartServer.bat. Windows may ask whether python.exe may use the network: allow it
   (private networks for friends at home; public too to be reached over the internet).
2. In the game, players choose Online > Settings > Service: Dedicated Server and type this PC's
   address (the window lists them). Over the internet, forward TCP 27600 and UDP 27601 on your
   router to this PC and give friends your public address.
3. Close the window to stop it. Accounts and profiles stay in airride.db beside StartServer.bat.

More (a VPS, backups, bans, options such as --motd "Welcome!"): SERVER.md.
The python folder is Python $PythonVersion's embeddable runtime from python.org (python\LICENSE.txt).
"@ | Set-Content -Encoding ascii "$stage\README.txt"

if (Test-Path $out) { Remove-Item $out }
# (Windows' tar writes the zip: Compress-Archive in PowerShell 5 puts backslashes in its paths)
Push-Location (Split-Path $stage)
& (Join-Path $env:SystemRoot "System32\tar.exe") -a -c -f $out (Split-Path -Leaf $stage)
Pop-Location
"Wrote $out ($([math]::Round((Get-Item $out).Length / 1MB, 1)) MB)"
