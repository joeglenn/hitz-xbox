@echo off
rem NHL Hitz Pro recomp: one-step setup (patch, extract, generate, build).
rem   setup.bat --iso "D:\Games\NHL Hitz Pro (USA).iso"
rem   setup.bat --help        for all options
rem Python is provided automatically (uv, downloaded into .tools\ on first run).
rem This is the only one of the three setup scripts that can finish the build
rem step, since build_windows.bat needs MSVC, which only exists on Windows.
setlocal
cd /d "%~dp0"
set "UV=%~dp0.tools\uv.exe"
if exist "%UV%" goto have_uv
echo === Getting uv (Python provider) into .tools\
if not exist ".tools" mkdir ".tools"
rem uv is a self-contained Python version/package manager; this downloads its
rem release zip with PowerShell and unpacks it, so nobody has to install
rem Python by hand first. Remove the leftover .tools\uv.zip once expanded.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12';" ^
  "Invoke-WebRequest https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-pc-windows-msvc.zip -OutFile .tools\uv.zip;" ^
  "Expand-Archive -Force .tools\uv.zip .tools; Remove-Item .tools\uv.zip"
if not exist "%UV%" (echo [!] could not download uv - check the internet connection & exit /b 1)
:have_uv
rem UV_PYTHON_INSTALL_DIR/UV_CACHE_DIR keep uv's downloaded Python and its
rem package cache inside this project's .tools\ folder instead of the user's
rem global uv cache, so a clean checkout stays self-contained and disposable.
set "UV_CACHE_DIR=%~dp0.tools\cache"
set "UV_PYTHON_INSTALL_DIR=%~dp0.tools\python"
rem --no-project avoids treating this folder as a uv-managed Python project;
rem --with capstone installs the one package setup.py needs into that throwaway env.
"%UV%" run --no-project --python 3.12 --with capstone python setup.py %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" echo. & echo [!] setup failed (exit %RC%) - see the message above.
endlocal & exit /b %RC%
