@echo off
REM TNxPilot build script — run on WINDOWS.
REM Needs: Visual Studio Build Tools (cl.exe on PATH — run from a
REM "x64 Native Tools Command Prompt") and Inno Setup (ISCC on PATH)
REM for the installer step. No Python, no Qt, no .NET, no internet
REM needed at build time beyond what is already installed.
REM
REM Output:
REM   TNxPilot.exe                          (single small EXE, static CRT)
REM   installer-output\TNxPilot-Setup-v1.0.exe   (offline installer)
setlocal
cd /d %~dp0

where cl >nul 2>&1
if errorlevel 1 (
  echo ERROR: cl.exe not found.
  echo Open the "x64 Native Tools Command Prompt for VS" and run build.bat there,
  echo or run:  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  pause
  exit /b 1
)

echo [1/2] Compiling TNxPilot.exe (C++17, /O2, static CRT /MT)...
cl /nologo /O2 /MT /std:c++17 /EHsc /W3 ^
  /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  src\main.cpp src\fileops.cpp src\config.cpp src\dialogs.cpp ^
  /link /SUBSYSTEM:WINDOWS /OUT:TNxPilot.exe ^
  comctl32.lib gdiplus.lib shlwapi.lib shell32.lib
if errorlevel 1 (
  echo BUILD FAILED — see the compiler errors above.
  pause
  exit /b 1
)
echo OK: TNxPilot.exe built.

echo [2/2] Building offline installer with Inno Setup...
where iscc >nul 2>&1
if errorlevel 1 (
  echo WARNING: ISCC (Inno Setup) not found on PATH.
  echo Portable EXE is ready: TNxPilot.exe
  echo Install Inno Setup, then run:  iscc installer.iss
  pause
  exit /b 0
)
iscc installer.iss
if errorlevel 1 (
  echo INNO SETUP FAILED.
  pause
  exit /b 1
)

echo.
echo BUILD OK.
echo   Portable:  TNxPilot.exe
echo   Installer: installer-output\TNxPilot-Setup-v1.0.exe
pause
