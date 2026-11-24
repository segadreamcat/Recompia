PokopiaVR (test build 1) for Recompia 0.16.0

Install
  1. Back up pkp_rt.dll in your Recompia-0.16.0 folder.
  2. Copy pkp_rt.dll, openxr_loader.dll and PlayVR.bat into that folder.
  No Set up again needed.

Play
  1. Start SteamVR (or Virtual Desktop with VDXR).
  2. Run PlayVR.bat. Normal Play / Play.bat still runs flat.
  3. Keep Settings > Gameplay > Shoulder camera at 0 / 0.
  4. Uncap the frame rate (F1 > Display: fps limit off, vsync off).
     The game draws one eye per frame, so it needs 2x your headset rate (144 fps for 72 Hz).

Controls (Touch)
  Left stick     move            Right stick    turn (smooth)
  A / B / X / Y  A / B / X / Y   Triggers       ZL / ZR
  Grips          L / R           Left menu      +
  Left stick click  -            Right stick click  right stick press
  Recenter with the headset's own recenter (hold the Oculus / system button).

Tweaks (pkp.ini, add a [vr] section)
  world_scale = 100   bigger = you feel smaller
  head_height = 60    eye height above the character
  mirror_x = 1        only if left and right eyes look swapped

Known in this build
  HUD and menus are stuck to your face (same picture in both eyes).
  Your own character model can block the view.
  Aiming follows the turn direction, not your hand yet.
