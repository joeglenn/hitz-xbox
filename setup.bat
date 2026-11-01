@echo off
rem NHL Hitz Pro recomp: one-step setup (patch, extract, generate, build).
rem   setup.bat --iso "D:\Games\NHL Hitz Pro (USA).iso"
rem   setup.bat --help        for all options
rem Python is provided automatically (uv, downloaded into .tools\ on first run).
setlocal
cd /d "%~dp0"
set "UV=%~dp0.tools\uv.exe"
if exist "%UV%" goto have_uv
echo === Getting uv (Python provider) into .tools\
if not exist ".tools" mkdir ".tools"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol='Tls12';" ^
  "Invoke-WebRequest https://github.com/astral-sh/uv/releases/latest/download/uv-x86_64-pc-windows-msvc.zip -OutFile .tools\uv.zip;" ^
  "Expand-Archive -Force .tools\uv.zip .tools; Remove-Item .tools\uv.zip"
if not exist "%UV%" (echo [!] could not download uv - check the internet connection & exit /b 1)
:have_uv
set "UV_CACHE_DIR=%~dp0.tools\cache"
set "UV_PYTHON_INSTALL_DIR=%~dp0.tools\python"
"%UV%" run --no-project --python 3.12 --with capstone python setup.py %*
set RC=%ERRORLEVEL%
if not "%RC%"=="0" echo. & echo [!] setup failed (exit %RC%) - see the message above.
endlocal & exit /b %RC%
