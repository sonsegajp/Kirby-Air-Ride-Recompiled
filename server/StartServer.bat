@echo off
rem Kirby Air Ride: Recompiled - a dedicated online server. Double-click to start; close the window to stop.
rem Players join it in the game: Online > Settings > Service: Dedicated Server, then this PC's address.
rem Extra options go through: StartServer.bat --motd "Welcome!"   (StartServer.bat --help lists them)
setlocal
cd /d "%~dp0"
set PY=
if exist "%~dp0python\python.exe" set PY="%~dp0python\python.exe"
if not defined PY (
    where py >nul 2>nul && set PY=py -3
)
if not defined PY (
    where python >nul 2>nul && set PY=python
)
if not defined PY (
    echo Python was not found. Use the server download that has it built in
    echo ^(AirRideServer-win64.zip from the releases page^), or install Python 3.10 or newer.
    pause
    exit /b 1
)
title Kirby Air Ride dedicated server
echo Kirby Air Ride dedicated server
echo   Ports: TCP 27600 (control) and UDP 27601 (relay). Over the internet, forward both on your router.
echo   Players on your network join with one of this PC's addresses:
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4"') do echo     %%a
echo   Accounts and profiles are kept in airride.db here. Close this window to stop the server.
echo.
%PY% -m airride_master --db "%~dp0airride.db" %*
echo.
echo The server stopped.
pause
