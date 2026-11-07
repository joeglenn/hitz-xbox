@echo off
rem NHL Hitz Pro (Xbox) recomp - Windows build (MSVC + Ninja, D3D11 path).
rem Can run from a plain cmd/powershell: finds MSVC itself via vswhere.
setlocal
rem Run from this script's own directory (hitz_pro\), regardless of the
rem caller's current directory, so the relative "build" and "game" paths
rem below are always correct.
cd /d "%~dp0"

rem If cl.exe (the MSVC compiler) is not already on PATH, locate a Visual
rem Studio install with vswhere and load its developer environment; if
rem that still does not produce cl.exe, give up with a clear message
rem instead of a confusing failure deeper in the build.
where cl >nul 2>nul || call :find_msvc
where cl >nul 2>nul || (echo [!] cl.exe not found & exit /b 1)

rem cmake/ninja: PATH first, else the copy bundled with VS
where cmake >nul 2>nul || set "PATH=%PATH%;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where cmake >nul 2>nul || (echo [!] cmake not found & exit /b 1)
where ninja >nul 2>nul || set "PATH=%PATH%;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where ninja >nul 2>nul || (echo [!] ninja not found & exit /b 1)

rem Out-of-source configure into build\, then a parallel Ninja build. Each
rem step's own exit code is checked (|| exit /b 1): a Ninja/CMake failure
rem must stop the script instead of falling through to a stale exe check.
echo === configure ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
echo === build (16k generated functions; first build is long) ===
cmake --build build --parallel || exit /b 1

if not exist "build\hitz_pro_recomp.exe" (echo [!] exe not found & exit /b 1)
echo.
echo Done: %CD%\build\hitz_pro_recomp.exe
echo Run from hitz_pro\: build\hitz_pro_recomp.exe  (expects game\default.xbe next to it)
endlocal
exit /b 0

rem Finds the newest Visual Studio install with the C++ (VC.Tools.x86.x64)
rem workload via vswhere, then loads its MSVC environment (PATH, INCLUDE,
rem LIB) into this cmd session with vcvars64.bat. Leaves cl.exe unset (and
rem returns success anyway) if vswhere or a matching install is missing;
rem the caller's own "where cl" check after this label is what actually
rem decides whether the build can proceed.
:find_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL exit /b 0
echo === loading MSVC environment from %VSINSTALL% ===
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
