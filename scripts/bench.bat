@echo off
REM Builds and runs the render benchmark in Release. Debug numbers are
REM meaningless for this: MSVC's iterator debugging dominates the inner loops.
setlocal
call "%~dp0vsenv.bat" || exit /b 1
if not exist build\release\CMakeCache.txt (
  "%CMAKE%" -S . -B build\release -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCUTLINE_BUILD_NATIVE_APP=OFF ^
    -DBUILD_TESTING=OFF || exit /b 1
)
"%CMAKE%" --build build\release --target cutline_bench || exit /b 1
build\release\cutline_bench.exe %*
endlocal
