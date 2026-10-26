# Outlaws VR

Outlaws VR is our full Unreal Engine 5 rebuild of LucasArts' Outlaws (1997) for virtual reality. It isn't an
injector or a mod for the original executable: we wrote a new game in UE 5.1 that opens your own Outlaws
Remastered files when it starts and builds every level, sprite, sound and script from them as you play. You get
the original's levels, enemies and story, with your hands on the guns, on a Quest 3 or Quest 2 by itself or on
PC VR, alone or with friends.

> **Warning:** this is an experimental community project. It is not affiliated with or endorsed by Lucasfilm
> Games, LucasArts or Nightdive Studios. You need your own copy of **Outlaws Remastered** (Steam or GOG); nothing
> from the game is included here. Back up your saves before you update.

**v0.95 (public beta, source preview before 1.0).** The whole game is here and playable, and this is the first
release with the source code public, so people can look at the code before 1.0. Feedback on the code gets cleaned
up for 1.0. Expect some rough edges. The release notes for each version are on this repository's
[Releases](../../releases) page.

**Feedback on the code:** open an issue on GitHub (https://github.com/outlawshub-arch/OutlawsVR/issues).

**Game bugs:** post in **#outlaws-vr-bugs** on the OutlawsTheGame Discord (https://discord.com/invite/VVA98hb)
with your device, the build line, the level, what happened and the log, or message **T4H_Dingle** on Discord.

## Features

- **Levels built from the original's data at runtime.** Sectors, adjoins, walls, floors and ceilings come from
  your LAB files, and the original's INF scripts drive the doors, lifts, switches and triggers.
- **Nothing from the game ships with it.** Textures, sprites, sounds, music and cutscenes are read from your
  install; on PC the cutscenes and music are converted once on first start.
- **Hands-on VR play.** Each hand picks its own weapon from a wheel, reloads, grabs and punches bad guys, opens
  doors, takes items and climbs onto ledges and roofs.
- **Aimed throws with an arc preview.** Knives, bottles and dynamite go where your hand points from your face:
  left and right come from where your hand is relative to your face, distance from how high you hold it. A live
  arc shows where the throw will land. Walking and turning never steer it.
- **3D guns built from your own sprites at runtime.** Each held gun is extruded at load time from the game's
  sprite using small recipe files in `Content/HeldGuns`, so there are no meshes or images of the game's art in the
  repo.
- **The main game and the Historical Missions.** Both campaigns play through, with saves, autosaves, a resume
  point, and end-of-level stats on a score card you hold in your hand.
- **Co-op for up to 8.** Play the story together over a home network or the internet; join late, drop and
  reconnect, revive each other and pass items from the wheel.
- **A Bad guys multiplier.** Add extra bad guys, alone or in co-op, spread out over the level away from the
  players' start. Open from the first start, off by default.
- **Multiplayer on the original's maps.** Death Match, Team Play, Tag, Kill The Fool With The Chicken, Capture The
  Flag and Secret Document, plus custom `.rcm` maps from your Outlaws folder. Match bots choose their gun by range
  and run for health or cover when badly hurt.
- **Voice chat.** Built in, off until you turn it on, with an optional push to talk on either grip.
- **A randomizer.** Enemies, items and mission order can be shuffled from a seed. Open from the first start, no
  story finish needed; everything is off by default.
- **Hidden boxes and Extras.** One hidden box in each story level, plus achievements, par times and a collectibles
  shelf on the Extras page.
- **Quest standalone, with a Quest 2 profile.** The same build runs on a Quest 3 or a Quest 2 without a PC; on a
  Quest 2 it uses lower settings by itself.
- **An enemy throttle for busy scenes.** When lots of bad guys are awake at once, the far ones think and move
  less often, with caps set per device (Quest 3, Quest 2, PC) that tighten when the frame runs over budget. A bad
  guy fighting you in plain view is never throttled.
- **Refresh rate choice.** Pick the headset's refresh rate in the graphics options where the headset offers more
  than one.
- **Comfort options.** Smooth or teleport movement, snap or smooth turning, movement speed, dominant hand, and a
  one-controller mode.
- **A desktop mode for testing.** Walk round a level with keyboard and mouse, no headset needed.

## Requirements

- **Outlaws Remastered** (Steam or GOG), installed.
- **Quest standalone:** a Meta Quest 3 or Quest 2 in Developer Mode, a USB cable, and a Windows PC to install from.
- **PC VR:** Windows 10 or 11, a PC that runs PC VR, about 2 GB free, and a headset on Quest Link, Air Link or
  SteamVR (OpenXR). Only Quest controllers have been tried so far.

## Installation (drop-in)

Download `OutlawsVR-DropIn-<date>-<build>.zip` from the releases page and unzip it. It holds one folder,
`OutlawsVR`. Put that folder inside your Outlaws Remastered folder (the one with `olgeo.lab` in it).

- **Quest standalone:** turn on Developer Mode, plug the headset in, and run **`Install on Quest.bat`** from the
  `OutlawsVR` folder. It installs the game and copies the files it needs from your Outlaws install to the
  headset. The game then shows in the headset's Library under **Unknown Sources**.
- **PC VR:** start Link, Air Link or SteamVR and make it the active OpenXR runtime, then run
  **`Play Outlaws VR.bat`**. The first start converts the cutscenes (it downloads FFmpeg once).

To update, delete the old `OutlawsVR` folder, put the new one in its place and, for the Quest, run
`Install on Quest.bat` again. Saves and settings are kept. Don't uninstall the game from the headset to update it:
that deletes your saves. If Windows says the old folder is in use, run `Tools\Stop adb.bat` from the new folder or
unplug the Quest. `Install on Quest.bat` downloads the latest adb from Google the first time it runs; with no
internet it uses the adb that comes in the package.

## Controls

Right-handed defaults (Quest Touch controllers). With **Dominant hand: Left** the sticks swap sides.

| Input | Action |
| --- | --- |
| Trigger | Fire; in menus, point and pull to choose |
| Hold **A** (right hand) or **X** (left hand), point at a slot, release | Weapon wheel for that hand (items such as the medical bag are used from it; a grip while it is open drops the item pointed at, to pass it in co-op) |
| **B** / **Y** | Reload the gun in that hand; back in menus |
| Left menu button | Pause menu |
| Left stick | Walk (follows your head, or a hand, in the options); click to toggle run |
| Right stick left / right | Turn (snap or smooth) |
| Right stick up | Jump (or dip and spring up with your body) |
| Right stick held down | Crouch, as deep as you push; under water, dive |
| Right stick click | Switch a double-barrel's barrels |
| Click both sticks | Recenter the view |
| Grip | Take an item or bottle, open a door, use a switch, grab a ledge, grab a bad guy, revive a co-op partner; things in reach glow first |
| Grip a ledge and pull | Climb; let go to drop |
| Swing an empty fist | Punch (grab a bad guy with one hand, punch or shoot him with the other) |
| Knife: squeeze the trigger, let go to throw | Throw |
| Dynamite: pull the trigger to light, let go to throw | Throw |
| Bottle: hold the grip, let go to throw | Throw |
| Hold a grip (with Push to talk on) | Talk |

Throws are aimed, not swung: left and right come from where your hand is relative to your face, and distance
from your hand height. Watch the arc preview to see where it will land. Walking and turning never steer a throw.

**Classic button layout** (untick *Y / B reload* in the VR options): the dominant top button jumps, the other
top button uses the medical bag, and a quick tap of **A** or **X** reloads.

**One controller** (Left only or Right only): the stick walks, and a hard push to the side turns; **A** or **X**
opens the wheel and a tap reloads; **B** or **Y** jumps; click the stick to toggle run, hold it a second for the
pause menu; duck with your body to crouch.

Desktop mode: W A S D, mouse, Space to jump, C or Ctrl to crouch, Shift to run, E to use.

## Co-op and multiplayer

- **Host:** main menu, **New Game**, **Co-op** for the story, or **Host Game** for a multiplayer match. Set the
  map, mode, limits, max players and an optional password, then **Host**.
- **Join:** type the host's address, or pick it from the Recent list. On a home network a host shows up in the
  join list by itself.
- **Over the internet:** only the host sets anything up. The game can open **UDP port 7777** on the router for you
  (Options > Multiplayer, on by default; it closes it again when you stop hosting). If your router doesn't allow
  that, forward the port to the hosting machine by hand. Let the game through the Windows firewall on private
  networks. Press **Show internet address** in
  the host lobby once: to show your public IP the game asks api.ipify.org, then shows **Your internet address** in
  the lobby. The choice is remembered on that machine, so later hosting shows it straight away (**Hide internet
  address** turns it off). Nothing is asked at launch, by a host who never pressed it, or when you join.
  Share the address privately with the people you invite.

## Logs

- **PC:** `%LOCALAPPDATA%\OutlawsUE\Saved\Logs` (saves are in `OutlawsSaves` and settings in
  `OutlawsSettings.ini` beside it).
- **Quest:** `/sdcard/Android/data/com.outlawsvr.OutlawsUE/files/UnrealGame/OutlawsUE/OutlawsUE/Saved/Logs`. Pull
  it with `adb pull` from the drop-in's `Tools\platform-tools` folder.

Please include the build line from the bottom of the main menu with any report.

## Test hardware

- Meta Quest 3 (standalone and over Link)
- Meta Quest 2 (standalone)
- A mid-range Windows 11 desktop: a 6-core Intel CPU, an RTX 3070 with 8 GB, and 16 GB of RAM

## Building from source

1. Install **Unreal Engine 5.1** and Visual Studio 2022 (MSVC v143).
2. Clone this repo, right-click `OutlawsUE.uproject` and choose *Generate Visual Studio project files*, then build
   `OutlawsUEEditor` (Development Editor, Win64), or open the `.uproject` and let the editor compile it.
3. The start map is `/Game/Outlaws/Maps/L_Runtime`. Run it in VR with
   `UnrealEditor.exe OutlawsUE.uproject /Game/Outlaws/Maps/L_Runtime -game -vr`, and add
   `-OLInstall="<your Outlaws Remastered folder>"` if it can't find your install.
4. **Quest (Android):** you need the Android SDK, NDK r25b and JDK 11. Set `ANDROID_HOME`, `NDKROOT` and
   `JAVA_HOME`, or copy `Config/UserEngine.ini.example` to `Config/UserEngine.ini` (per user; git ignores it) and
   point it at them; the example says where command-line builds look. Then package for Android (ASTC) from the
   editor or with RunUAT `BuildCookRun`.
5. Optional: `Config/UserGame.ini.example` (or `-OLItemDir=<dir>`, or `OUTLAWSVR_ITEMDIR`) points a dev build at a
   local folder of placeholder art. Nothing in it ships, and the game runs without it.

`tools/olue/item3d` holds the Python scripts that make the held-gun recipes; `tools/olue/questperf` is a Quest
performance capture helper (set `ADB` or `ANDROID_HOME`, and `UE_ROOT`); `tools/olue/upnp/mock_igd.py` is a
stand-in router for testing the automatic port opening.

A build from this repository uses the engine's own OpenXR plugin. The released builds carry a small fix to Epic's
OpenXR plugin (the swapchain wait and an early recenter no longer end the game), which can't be published here
because it is Epic's engine source.

The Python tools need Python 3.10 or later and these packages (not included):
`pip install numpy scipy pillow shapely scikit-image mapbox_earcut`.

Two optional Windows runtime files are not in this repository: the Real-ESRGAN upscaler (put the official
v0.2.5.0 release in `ThirdParty/RealESRGAN`; without it the game skips the sharper grabbed-enemy frames) and
FFmpeg, which the installer downloads on first run. See `THIRD_PARTY_NOTICES.md`.

## Credits and licence

- Outlaws VR: made by T4H_Dingle. Tested by Dotard and T4H_Dingle. Code under the MIT licence (see `LICENSE`).
- earcut by Mapbox (ISC), ported to C++ for floor and ceiling triangulation.
- Built with Unreal Engine by Epic Games (Unreal Engine EULA; the engine is not included).
- libsndfile (LGPL-2.1) for music decoding, Real-ESRGAN ncnn Vulkan (MIT/BSD-3) for the grabbed-enemy upscale,
  and FFmpeg (downloaded on first run) for cutscene conversion. None of them are in this repository.
- Built with the help of Claude AI, by Anthropic.
- Built on the shoulders of The Force Engine, by Lucius, and LawMaker, the Outlaws level editor by Alexei Novikov and David Lovejoy, whose work
  showed how Outlaws behaves.
- The original game, Outlaws, by LucasArts. It is not included; you need your own copy of Outlaws Remastered.

See `THIRD_PARTY_NOTICES.md` for the full notices.
