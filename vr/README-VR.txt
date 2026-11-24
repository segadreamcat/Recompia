PokopiaVR test 13

Start Virtual Desktop on the Quest, connect to this PC, then start SteamVR.
Run PlayVR.bat beside pkp.exe. The dedicated VR controls profile is controls-vr.ini.
Start at 72 Hz in Virtual Desktop. Keep the current SteamVR OpenXR runtime for this comparison.

Changes in build 13
  2560x2560 square source targets match the actual display aspect, avoiding the wide render's wasted side pixels.
  Quest 3 visible eye region is approximately 1986x2146 pixels (build 12 was about 838x905).
  RCAS sharpening runs on the actual headset images, before the OpenXR transfer. Desktop sharpening alone does not affect them.
  Camera Settings: VR Sharpness starts at 65%, VR HUD Size at 55% so edge widgets fit inside both eyes.
  VR Reading Panel is off in this profile: scripted cameras stay in stereo with tracked head movement and native camera position.
  Selection menus and follow-camera tutorial popups still use the readable panel. Turn VR Reading Panel on to use it for locked dialogue too.
  First-person body hiding, perspective and screenshot shortcuts are retained.

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
  The installed controls-vr.ini uses fixed 2560x2560 source targets, 16x anisotropic filtering, uncapped, vsync off, temporal upscale/AA off.
  This gives over five times build 12's pixels in the headset-visible region. SteamVR resolution alone cannot recover missing game detail.
  Display Settings includes square VR render sizes: 1920x1920 (lighter), 2560x2560 (installed), 2880x2880 (more GPU memory/work).
  Keep Display Aspect on Auto so the render targets and all scene projections agree. Resolution changes require restarting the game.
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
  In-world HUD is inset into the eye pictures at infinity; it has no separate floating layer yet.
  Scripted-camera support uses the primary world camera. Pre-rendered videos and full selection screens use the panel.
  Room-scale movement does not add collision to the physical head.
  Hand aiming uses the game's existing trigger-driven abilities, not physical swing/contact attacks.
  This runtime uses the current Recompia codebase. Existing VR hook points and the runtime ABI match the installed 0.16 executable;
  newer unrelated features may require an updated/rebuilt game executable.

Log: pkp-vr.log beside the game.
Portable package: merge VR-settings.ini into the matching sections of your existing pkp.ini; keep your paths.
Rollback: use the backup folder created during installation. pkp_rt.dll.flat is still the original flat runtime.
