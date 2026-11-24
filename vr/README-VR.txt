PokopiaVR test 12

Start Virtual Desktop on the Quest, connect to this PC, then start SteamVR.
Run PlayVR.bat beside pkp.exe. The dedicated VR controls profile is controls-vr.ini.
Start at 72 Hz in Virtual Desktop. Keep the current SteamVR OpenXR runtime for this comparison.

Changes in build 12
  Sharper 1920x1080 source rendering, 8x anisotropic filtering and chromatic aberration disabled in the dedicated VR profile.
  First person hides the local body through its native fade component; third person and reading views restore it.
  Conversations and menus use a raised reading panel when the game locks camera input (Camera Settings: VR Reading Panel).
  Camera Settings also has VR First Person and VR Shortcuts switches.

Retained camera fixes
  The source camera keeps the native display aspect and uses a symmetric FOV covering the headset. The compositor receives that actual FOV.
  Every projection rebuild keeps the VR lens. Objects and terrain therefore agree even on scene paths that enforce the native display aspect.
  Eye images are matched to scene camera uniforms. Stationary-camera matches prefer the sibling of the captured left eye, keeping pairs together.
  Unmatched images are skipped instead of guessing an eye. Sampled diagnostics audit projection consistency without inspecting every draw.
  Independent headset frame pump on a separate GPU queue; the last complete stereo pair is reprojected during a slow game frame.
  Eyes advance on camera updates, not extra presentations. Each eye retains its actual render pose; queued frames cannot overwrite tracking metadata.
  Timeline semaphores carry staged images between the game and headset queues without holding the game queue during OpenXR calls.
  Brief missing camera updates keep the world in stereo instead of switching to a flat panel.
  First person is anchored to the player's position, with headset movement relative to the initial head centre.
  Right-controller tracking aims abilities; head aim is the fallback when controller tracking is unavailable.
  Title and non-world menus use a world-locked panel placed in front of the current head position.
  Reading-panel centre is 30 cm above eye level; with distance 2.5 m and width 2.4 m, dialogue sits below centre without being at your feet.

Touch controls
  Left stick: move. Right stick: smooth turn / menu navigation.
  A/B/X/Y, triggers ZL/ZR, grips L/R, left menu: +.
  Hold left stick click and tilt it for the D-pad.
  Short RIGHT stick click: switch first/third person (keyboard F11).
  Short LEFT stick click with the stick centred: save a screenshot (keyboard F12).
  Press both sticks together for -; this chord does not trigger either shortcut.
  Hold-click plus tilt still operates the D-pad. Switch VR Shortcuts off to restore the normal right-stick press.
  Use the headset's system recenter while facing forward.

Performance
  The installed controls-vr.ini starts at a fixed 1920x1080 source image for each alternate eye, uncapped, vsync off, temporal upscale/AA off.
  This has 2.25 times the source pixels of build 11. SteamVR resolution alone cannot recover detail missing from the game source.
  For more performance, change Render Resolution to 1600x900 or 1280x720 in the Display Settings tab.
  The game still renders alternate eyes. 144 game fps produces 72 new stereo pairs/s; a slower game reuses pairs.
  The frame pump does not create new game motion during a reused pair. The log reports both rates separately.
  Adjust the VR profile through game Settings while launched in VR.

Existing [vr] keys in pkp.ini
  world_scale = 100, head_height = 120, mirror_x = 0
  The old frame_lag key is ignored: eye association now follows the rendered camera.
  panel_distance = 2.5, panel_width = 2.4
  Keep mirror_x at 0; it is not an eye-swap correction.

Screenshots
  Captures are PNG files in vr-screenshots beside pkp.exe. latest.txt names the newest capture.
  The shortcut saves locally; it does not automatically upload to chat. Tell the assistant you took a screenshot so it can open the file.
  Captures contain the desktop game view, including native dialogue and HUD.

Current scope
  In-world HUD remains drawn into the eye pictures; it has no separate floating layer yet.
  Room-scale movement does not add collision to the physical head.
  Hand aiming uses the game's existing trigger-driven abilities, not physical swing/contact attacks.
  This runtime uses the current Recompia codebase. Existing VR hook points and the runtime ABI match the installed 0.16 executable;
  newer unrelated features may require an updated/rebuilt game executable.

Log: pkp-vr.log beside the game.
Rollback: use the backup folder created during installation. pkp_rt.dll.flat is still the original flat runtime.
