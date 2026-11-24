PokopiaVR test 10

Start Virtual Desktop on the Quest, connect to this PC, then start SteamVR.
Run PlayVR.bat beside pkp.exe. The dedicated VR controls profile is controls-vr.ini.
Start at 72 Hz in Virtual Desktop. Keep the current SteamVR OpenXR runtime for this comparison.

Changes
  Camera FOV and aspect are set at the source before view copies, using the game projection builder; no late asymmetric matrix override.
  Eye images are matched to the camera uniforms consumed by the scene draw. Unmatched images are skipped instead of guessing an eye.
  Independent headset frame pump on a separate GPU queue; the last complete stereo pair is reprojected during a slow game frame.
  Eyes advance on camera updates, not extra presentations. Each eye retains its actual render pose; queued frames cannot overwrite tracking metadata.
  Timeline semaphores carry staged images between the game and headset queues without holding the game queue during OpenXR calls.
  Brief missing camera updates keep the world in stereo instead of switching to a flat panel.
  First person is anchored to the player's position, with headset movement relative to the initial head centre.
  Right-controller tracking aims abilities; head aim is the fallback when controller tracking is unavailable.
  Title and non-world menus use a world-locked panel placed in front of the current head position.

Touch controls
  Left stick: move. Right stick: smooth turn / menu navigation.
  A/B/X/Y, triggers ZL/ZR, grips L/R, left menu: +.
  Hold left stick click and tilt it for the D-pad.
  Press both sticks together for -; right stick click alone is right stick press.
  Use the headset's system recenter while facing forward.

Performance
  The installed controls-vr.ini starts at a fixed 1280x720 per eye, uncapped, vsync off, temporal upscale/AA off.
  The game still renders alternate eyes. 144 game fps produces 72 new stereo pairs/s; a slower game reuses pairs.
  The frame pump does not create new game motion during a reused pair. The log reports both rates separately.
  Adjust the VR profile through game Settings while launched in VR.

Existing [vr] keys in pkp.ini
  world_scale = 100, head_height = 120, mirror_x = 0
  The old frame_lag key is ignored: eye association now follows the rendered camera.
  panel_distance = 2.5, panel_width = 3.2
  Keep mirror_x at 0; it is not an eye-swap correction.

Current scope
  In-world HUD remains drawn into the eye pictures; it has no separate floating layer yet.
  The local character mesh is not hidden. Room-scale movement does not add collision to the physical head.
  Hand aiming uses the game's existing trigger-driven abilities, not physical swing/contact attacks.
  This runtime uses the current Recompia codebase. Existing VR hook points and the runtime ABI match the installed 0.16 executable;
  newer unrelated features may require an updated/rebuilt game executable.

Log: pkp-vr.log beside the game.
Rollback: use the backup folder created during installation. pkp_rt.dll.flat is still the original flat runtime.
