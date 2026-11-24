@echo off
setlocal
rem Connect the headset in Virtual Desktop and start SteamVR before launching.
cd /d "%~dp0"
if not exist "%~dp0pkp.exe" (
  echo Put PlayVR.bat beside pkp.exe.
  pause
  exit /b 1
)
if not defined NXRT_FS_SAVE_DIR set "NXRT_FS_SAVE_DIR=%~dp0save"
if exist "%~dp0controls-vr.ini" set "NXRT_PLAY_CONTROLS=%~dp0controls-vr.ini"
set "NXRT_VR_ENABLED=1"
start "" "%~dp0pkp.exe" --log "%~dp0pkp-vr.log"
endlocal
