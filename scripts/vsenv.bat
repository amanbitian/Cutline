@echo off
REM Finds a Visual Studio with the C++ tools and prepares the compiler environment.
REM Sets VSROOT, CMAKE, CTEST and NINJA, and runs vcvars64. Called by the other
REM scripts; it does not use setlocal, so what it sets is visible to its caller.
REM
REM Lookup order:
REM   1. CUTLINE_VSROOT, when set (an explicit choice always wins);
REM   2. vswhere, the installer's own locator, for the newest install with the C++
REM      x64 tools;
REM   3. nothing: it says what to do, rather than guessing a path.
REM CMake and Ninja come from the PATH when present, otherwise from the copies
REM Visual Studio ships.

REM The installer folder name contains parentheses, so it is resolved outside any
REM parenthesised block, where a closing parenthesis would end the block early.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if defined CUTLINE_VSROOT set "VSROOT=%CUTLINE_VSROOT%"
if not defined VSROOT if exist "%VSWHERE%" call :locate
if not defined VSROOT (
  echo No Visual Studio with the C++ x64 tools was found.
  echo Install one, or set CUTLINE_VSROOT to its installation directory.
  exit /b 1
)
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo "%VSROOT%" has no VC\Auxiliary\Build\vcvars64.bat; it is not a C++ install.
  exit /b 1
)

set "VSCMAKEDIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "CMAKE="
set "CTEST="
set "NINJA="
for %%X in (cmake.exe) do if not "%%~$PATH:X"=="" set "CMAKE=%%~$PATH:X"
for %%X in (ctest.exe) do if not "%%~$PATH:X"=="" set "CTEST=%%~$PATH:X"
for %%X in (ninja.exe) do if not "%%~$PATH:X"=="" set "NINJA=%%~$PATH:X"
if not defined CMAKE set "CMAKE=%VSCMAKEDIR%\CMake\bin\cmake.exe"
if not defined CTEST set "CTEST=%VSCMAKEDIR%\CMake\bin\ctest.exe"
if not defined NINJA set "NINJA=%VSCMAKEDIR%\Ninja\ninja.exe"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars64 failed in "%VSROOT%".
  exit /b 1
)
exit /b 0

:locate
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
exit /b 0
