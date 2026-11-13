LEGO Star Wars: The Complete Saga - VR mod by RanitaVR
======================================================

Developed by RanitaVR
https://github.com/ranitadesign/LEGO-STAR-WARS-TCS-VR

A fan-made VR mod for LEGO Star Wars: The Complete Saga (PC), played in
first person with motion controllers: swing the lightsaber with your hand,
aim blasters with the controller, pick Force targets by looking at them,
and fly pods and ships from a cockpit.

This zip contains ONLY the mod. It does not include any game data: you
need your own copy of the game.

This is an early, experimental build. Expect bugs and occasional crashes.


Requirements
------------
- LEGO Star Wars: The Complete Saga installed from Steam (the PC version).
- A PC VR headset that works with SteamVR (Quest via Link, Air Link,
  Virtual Desktop or Steam Link; Index; Vive; etc.).
- Windows 10 or 11.


Installation
------------
1. Install LEGO Star Wars: The Complete Saga on Steam and launch it once
   normally, then quit. This installs DirectX 9 files the game needs.
2. Unzip this archive anywhere (for example the Desktop). Keep all the
   files together (including RanitaVR.bmp) in the "LEGO Star Wars VR" folder.
3. Start SteamVR with your headset connected.
4. Run "LEGO Star Wars VR.exe" from the folder.

The mod finds the game in your Steam libraries automatically. If it says
the game data was not found, open saga_vr.ini and set the game folder, for
example (remove the leading ";"):

    data_dir=C:\Program Files (x86)\Steam\steamapps\common\Lego Star Wars Saga

Optional: add "LEGO Star Wars VR.exe" to Steam with "Add a Non-Steam Game"
to launch it from your library or from inside VR.


Quest users without SteamVR
---------------------------
SteamVR is the default. To use the Meta Quest Link app directly instead
(Link / Air Link), open saga_vr.ini and change

    runtime=steamvr

to

    runtime=meta

Use "runtime=system" to keep whatever OpenXR runtime is active on your PC.


Controls (Quest Touch layout)
-----------------------------
Left stick ............. move
Right stick ............ snap turn (30 degrees), or smooth turn (see smooth_turn)
Right stick up/down .... free play: next / previous character
A ...................... jump / confirm in menus
Right trigger .......... attack / shoot
Swing right hand ....... lightsaber swing
Hold the blade in the way of a shot to deflect it, or between you and an enemy blade to block it ("Blocked!")
Double jump, then point the lit saber down ... Jedi ground slam
Swing left hand ........ punch (characters with a gun) - hit an enemy within reach
Right grip ............. lightsaber on / off
B, X or left trigger ... special (Force, build) - hold it, like the original
Y ...................... swap character
Left stick click ....... pause menu / skip cutscene
Right stick click ...... recenter view
Grips (in a pod) ....... hold the levers; right lever = speed
Right grip (in a ship) . grab the flight stick (near your right hand)

Index controllers: the left B button also opens the pause menu.

On the title screen, press A (or Enter on the keyboard) to start.
Cutscenes and menus are shown on a big screen in front of you.


Settings (saga_vr.ini)
----------------------
Open it with Notepad. The most useful ones:

render_width / render_height
    Per-eye resolution. Lower them (for example 1200 / 1284) if the game
    stutters.
fp_world_scale
    Size of the world in first person. 0.25 makes you about minifig sized.
smooth_turn
    Set it to a speed in degrees per second (for example 120) to turn
    smoothly with the right stick instead of in 30 degree steps.
saber_pitch
    Tilt of the lightsaber blade in your hand, in degrees.
swing_speed
    How fast (m/s) you must swing for it to count as an attack.
unlock_all
    Set to 1 to unlock all characters, levels and extras. The game will
    ask to save; choose yes. This changes your VR save permanently.

Keyboard: F10 recenters the view, F7 turns the lightsaber on or off.


Saves
-----
VR saves are written to a "SavedGames" folder inside the game's Steam
folder. They are separate from the saves of the normal game.


Known issues
------------
- 32-bit game: long sessions can run out of memory and crash. Restart the
  game if it gets unstable.
- Performance is around 36-70 fps depending on the PC and the
  resolution setting.
- Some characters' hands or held weapons can look slightly off.
- Anakin as a boy can look untextured in some Podrace cutscenes.


Troubleshooting
---------------
- Nothing appears in the headset: make sure SteamVR (or the Meta app with
  runtime=meta) is running before you start the mod.
- "d3dx9_35.dll" or "xinput1_3.dll" missing: launch the original game once
  from Steam, or install the "DirectX End-User Runtimes (June 2010)" from
  Microsoft.
- Crashes: the "log" folder next to the mod has log files (and a
  vr_fault.txt after a crash). Send them along with a bug report.


Credits
-------
VR mod developed by RanitaVR (https://github.com/ranitadesign).

Built on saga-pc, the community decompilation of LEGO Star Wars: The
Complete Saga. LEGO, Star Wars and the game belong to their respective
owners. This is an unofficial fan project, not affiliated with or
endorsed by them. No game assets are included.
