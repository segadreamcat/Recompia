@echo off
rem PokopiaVR: starts the game with the headset on. Start SteamVR (or Virtual Desktop's VDXR / the Oculus app) first.
cd /d "%~dp0"
if not defined NXRT_FS_SAVE_DIR set "NXRT_FS_SAVE_DIR=%~dp0save"
set "NXRT_VR_ENABLED=1"
start "" pkp.exe --log pkp.log
