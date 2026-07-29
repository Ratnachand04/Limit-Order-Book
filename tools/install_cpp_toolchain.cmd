@echo off
REM ---------------------------------------------------------------------------
REM  Adds the C++ toolchain this project needs to Visual Studio Community 2026.
REM
REM  Run this ONCE, as Administrator (right-click -> "Run as administrator").
REM  It installs, into C:\Program Files\Microsoft Visual Studio\18\Community:
REM
REM     * MSVC v14.5x C++ compiler and standard library   (cl.exe)
REM     * Windows 11 SDK                                  (the CRT headers)
REM     * C++ CMake tools for Windows                     (cmake.exe + ninja.exe)
REM     * AddressSanitizer support                        (LOB_SANITIZE=ON)
REM
REM  Roughly 8-10 GB.  Safe to re-run; already-present components are skipped.
REM ---------------------------------------------------------------------------
setlocal

set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe"
set "VSPATH=%ProgramFiles%\Microsoft Visual Studio\18\Community"

if not exist "%VSINSTALLER%" (
  echo ERROR: Visual Studio Installer not found at:
  echo        %VSINSTALLER%
  exit /b 1
)

echo Installing the C++ workload into "%VSPATH%" ...
echo This takes a while. A progress window will appear.
echo.

"%VSINSTALLER%" modify ^
  --installPath "%VSPATH%" ^
  --add Microsoft.VisualStudio.Workload.NativeDesktop ^
  --add Microsoft.VisualStudio.Component.VC.CMake.Project ^
  --includeRecommended ^
  --passive ^
  --norestart

echo.
echo Installer launched (exit code %ERRORLEVEL%).
echo When it finishes, verify with:
echo     tools\check_toolchain.ps1
endlocal
