@echo off
REM Build then run the native CTest suite.
REM Set CUTLINE_STRICT=1 to make a test that could not run for want of a
REM prerequisite (a media fixture, FFmpeg) a failure instead of a skip. A test that
REM does not apply to this build or machine (no audio device, nothing to refuse
REM because the build can encode) still skips.
setlocal
call "%~dp0build.bat" || exit /b 1
call "%~dp0vsenv.bat" || exit /b 1
"%CTEST%" --test-dir build\native --output-on-failure %*
endlocal
