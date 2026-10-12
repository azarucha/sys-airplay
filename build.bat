@echo off
rem Baut sys-airplay unter Windows. Voraussetzung: devkitPro (siehe README.md).
setlocal
cd /d "%~dp0"

if not exist "sysmodule\source\airport_key.h" (
    python fetch_deps.py || goto :fail
)

if "%DEVKITPRO%"=="" set DEVKITPRO=/opt/devkitpro
if not exist "C:\devkitPro\msys2\usr\bin\bash.exe" (
    echo devkitPro wurde nicht unter C:\devkitPro gefunden. Bitte zuerst installieren.
    goto :fail
)

C:\devkitPro\msys2\usr\bin\bash.exe -lc "cd \"$(cygpath -u '%~dp0')\" && make" || goto :fail
echo.
echo Fertig. Den Inhalt des Ordners "out" ins Hauptverzeichnis der SD-Karte kopieren.
exit /b 0

:fail
echo.
echo Build fehlgeschlagen.
exit /b 1
