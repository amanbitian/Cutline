@echo off
REM Configure and build the native core with the VS 18 toolchain.
REM Usage: scripts\build.bat [target]
setlocal
call "%~dp0vsenv.bat" || exit /b 1
if not exist build\native\CMakeCache.txt (
  "%CMAKE%" -S . -B build\native -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DCUTLINE_BUILD_NATIVE_APP=OFF ^
    -DBUILD_TESTING=ON || exit /b 1
)
"%CMAKE%" --build build\native %* || exit /b 1
endlocal
