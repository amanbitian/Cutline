@echo off
REM Build when needed, configure the Qt runtime path, and launch the Cutline desktop application.
REM Usage:
REM   scripts\run-app.bat --demo
REM   scripts\run-app.bat --open "D:\Projects\My Film.cutline"
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "APP=%ROOT%\build\app\Cutline.exe"
if not defined CUTLINE_QT_ROOT set "CUTLINE_QT_ROOT=%ROOT%\.tools\qt\6.8.3\msvc2022_64"

if not exist "%CUTLINE_QT_ROOT%\bin\Qt6Core.dll" (
  echo Qt was not found at "%CUTLINE_QT_ROOT%".
  echo Set CUTLINE_QT_ROOT to a Qt 6.5 or newer MSVC installation.
  exit /b 1
)

if not exist "%APP%" (
  call "%HERE%build-app.bat" --target Cutline || exit /b 1
)

set "PATH=%CUTLINE_QT_ROOT%\bin;%ROOT%\build\app;%PATH%"
"%APP%" %*
endlocal
