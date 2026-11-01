@echo off
rem NHL Hitz Pro (Xbox) recomp - Windows build (MSVC + Ninja, D3D11 path).
rem Can run from a plain cmd/powershell: finds MSVC itself via vswhere.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul || call :find_msvc
where cl >nul 2>nul || (echo [!] cl.exe not found & exit /b 1)

rem cmake/ninja: PATH first, else the copy bundled with VS
where cmake >nul 2>nul || set "PATH=%PATH%;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where cmake >nul 2>nul || (echo [!] cmake not found & exit /b 1)
where ninja >nul 2>nul || set "PATH=%PATH%;%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
where ninja >nul 2>nul || (echo [!] ninja not found & exit /b 1)

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

:find_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL exit /b 0
echo === loading MSVC environment from %VSINSTALL% ===
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0
