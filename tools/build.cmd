@echo off
rem Configures and builds the add-on into <project>\build\addon (Ninja, RelWithDebInfo), runs the
rem checksum test and lists the exports of <project>\build\bin\ACVUpscaler.addon64.
rem Build output stays inside the project: MSVC fails on object paths longer than 250 characters.
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 goto :fail

set "PATH=C:\Program Files\CMake\bin;%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"

cmake -S "%ROOT%" -B "%ROOT%\build\addon" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 goto :fail

cmake --build "%ROOT%\build\addon"
if errorlevel 1 goto :fail

ctest --test-dir "%ROOT%\build\addon" --output-on-failure
if errorlevel 1 goto :fail

dumpbin /nologo /exports "%ROOT%\build\bin\ACVUpscaler.addon64"
if errorlevel 1 goto :fail

echo.
echo Build OK: %ROOT%\build\bin\ACVUpscaler.addon64
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
