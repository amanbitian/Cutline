@echo off
REM Configure and build the desktop application with Qt (build\app).
REM Qt is looked for at .tools\qt\<version>\msvc2022_64 (the layout aqtinstall produces:
REM   py -m pip install aqtinstall
REM   py -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O .tools\qt)
REM or wherever CUTLINE_QT_ROOT points. Usage: scripts\build-app.bat [cmake --build arguments]
setlocal
call "%~dp0vsenv.bat" || exit /b 1
if not defined CUTLINE_QT_ROOT set "CUTLINE_QT_ROOT=%~dp0..\.tools\qt\6.8.3\msvc2022_64"
if not exist "%CUTLINE_QT_ROOT%\lib\cmake\Qt6\Qt6Config.cmake" (
  echo Qt was not found at "%CUTLINE_QT_ROOT%". See the header of this script.
  exit /b 1
)
if not exist build\app\CMakeCache.txt (
  "%CMAKE%" -S . -B build\app -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCMAKE_PREFIX_PATH="%CUTLINE_QT_ROOT%" ^
    -DCUTLINE_BUILD_NATIVE_APP=ON ^
    -DBUILD_TESTING=ON || exit /b 1
)
"%CMAKE%" --build build\app %* || exit /b 1
endlocal
