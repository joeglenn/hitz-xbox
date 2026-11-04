@echo off
rem NHL Hitz Pro (Xbox recomp) launcher. Game files go in hitz_pro\game\.
cd /d "%~dp0"
if not defined RECOMP_PB_EXEC set RECOMP_PB_EXEC=1
if not defined RECOMP_VBLANK set RECOMP_VBLANK=paced
if not defined RECOMP_AC97_READY set RECOMP_AC97_READY=1
if not defined RECOMP_FB_WINDOW set RECOMP_FB_WINDOW=1
if not defined RECOMP_D3D_PRESENT set RECOMP_D3D_PRESENT=1
if not defined RECOMP_USB set RECOMP_USB=1
if not defined RECOMP_USB_PADS set RECOMP_USB_PADS=2
if not defined HITZ_GPU set HITZ_GPU=1
rem set HITZ_DEBUG=1 for hitz_stderr.log and hitz_perf.log
if not defined HITZ_DEBUG set HITZ_DEBUG=0
if not exist "game\default.xbe" (echo [!] game\default.xbe not found - extract the game into hitz_pro\game & pause & exit /b 1)
if not "%HITZ_DEBUG%"=="1" (start "" "build\hitz_pro_recomp.exe" & exit /b 0)
rem the redirect must be inside the started command, or it only captures "start"
start "NHL Hitz Pro" /min cmd /c "build\hitz_pro_recomp.exe 2> hitz_stderr.log"
