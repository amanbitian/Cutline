@echo off
REM Runs the desktop application off-screen, lets it settle, saves a picture of its window and exits.
REM   scripts\shot.bat out.png [application arguments, e.g. --demo --do select:Counter]
REM Uses Qt's software renderer so it works without a display. The application's own preferences are not touched:
REM a scratch configuration folder is used. QML errors, which the windowed program cannot print to a console on
REM Windows, are sent to standard error.
setlocal
set "HERE=%~dp0"
set "OUT=%~1"
shift
if not defined CUTLINE_QT_ROOT set "CUTLINE_QT_ROOT=%HERE%..\.tools\qt\6.8.3\msvc2022_64"
set "PATH=%CUTLINE_QT_ROOT%\bin;%HERE%..\build\app;%PATH%"
set QT_QPA_PLATFORM=offscreen
set QT_QUICK_BACKEND=software
set QT_FORCE_STDERR_LOGGING=1
set "QT_QPA_FONTDIR=C:\Windows\Fonts"
set "CONFIG=%TEMP%\cutline-shot-config"
if "%CUTLINE_DELAY%"=="" set CUTLINE_DELAY=3500
"%HERE%..\build\app\Cutline.exe" --config "%CONFIG%" --screenshot "%OUT%" --delay %CUTLINE_DELAY% --no-audio %1 %2 %3 %4 %5 %6 %7 %8 %9
endlocal
