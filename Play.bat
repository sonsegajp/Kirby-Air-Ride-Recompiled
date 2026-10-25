@echo off
rem Kirby Air Ride: Recompiled - plays the developer build (build\windows-clang) from your own disc image.
rem The first time it asks for the disc image (or drag it onto this file); it remembers it in disc.txt.
setlocal
cd /d "%~dp0"
if not "%~1"=="" (
    > disc.txt echo %~f1
)
if not exist disc.txt (
    echo Kirby Air Ride: Recompiled plays from your own Kirby Air Ride disc image
    echo ^(USA, GKYE01: .iso, .gcm, .ciso, .rvz or .wia^).
    echo.
    set /p DISC=Drag the disc image onto this window and press Enter:
    call :save
)
set /p DISC=<disc.txt
if not exist "%DISC%" (
    echo The disc image "%DISC%" was not found. Delete disc.txt to choose it again.
    pause
    exit /b 1
)
start "" "%~dp0build\windows-clang\kirbyairride.exe" --iso "%DISC%" --fullscreen
exit /b 0

:save
set DISC=%DISC:"=%
> disc.txt echo %DISC%
exit /b 0
