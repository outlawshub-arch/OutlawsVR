# Outlaws VR

A new game written in Unreal Engine 5.1 that plays LucasArts' **Outlaws** (1997) in VR, on Quest 3, Quest 2 and PC VR. It isn't an injector or a patch for the original executable. At start-up it opens the player's own **Outlaws Remastered** install (Steam or GOG) and builds every level, sprite, sound and script from those files as it plays.

**Playing it?** The download, install steps, controls and the player's manual are on the [Releases](../../releases) page. Report game bugs in **#outlaws-vr-bugs** on the [OutlawsTheGame Discord](https://discord.com/invite/VVA98hb).

**Status:** v0.95, public beta and source preview before 1.0. Code feedback is welcome as [issues](../../issues), and it gets tidied up for 1.0.

> Not affiliated with or endorsed by Lucasfilm Games, LucasArts or Nightdive Studios. Nothing from the game is in this repository; you need your own copy of Outlaws Remastered to run it.

## How it works

1. **Find the install.** The game looks for Outlaws Remastered beside itself (the drop-in layout) or at `-OLInstall=<folder>`.
2. **Read the archives.** LAB and KPF archives are opened in place (`OutlawsLab`, `OutlawsFormats`): levels, INF scripts, item and NWX definitions, textures, sprites, palettes, sounds and the menu documents.
3. **Build the level at runtime.** Sectors, adjoins, walls, floors and ceilings become meshes (`OutlawsLevel`, `OutlawsLevelActor`, `OutlawsGeometry`; floors and ceilings are triangulated with a port of earcut). Textures are decoded from the game's own data (`OutlawsTextures`).
4. **Run the original logic.** The level's INF scripts drive elevators, doors, lifts, switches and triggers (`OutlawsInf`, `OutlawsInfRuntime`). Items, enemies and their animation come from the game's ITM/NWX data (`OutlawsSpriteActor`, `OutlawsEnemy`).
5. **Make the 3D items.** Held guns and props are built at load time from the player's sprites (`OutlawsGunBuilder`, `OutlawsItem3d`). The repo ships only shapes and recipes: geometry and UVs in `Content/HeldGuns` and `Content/Item3d`, and the recipe ops that paint them from the sprite at runtime. No pixels of the game's art are stored.
6. **VR and play.** Player, hands, wheel, grabbing, climbing and throws are in `OutlawsPlayer*`. Co-op and matches use UE replication (`OutlawsCoop*`, `OutlawsMpHost`, `OutlawsDeathmatch`, `OutlawsMatchModes`, `OutlawsRemotePlayer`), with our own small UPnP client for the host's port (`OutlawsUpnp`).

The behaviour reference is the original game itself, with [The Force Engine](https://github.com/TheForceEngine/TheForceEngine) (Jedi engine reconstruction) and [LawMaker](https://github.com/df21-net/editor/tree/main/lawmaker) (wall and sector flags, level rules) as guides. No code is copied from either.

## Repository layout

| Path | What's there |
| --- | --- |
| `Source/OutlawsRuntime` | The game: formats, level building, INF runtime, player, enemies, menus, co-op and multiplayer, tests |
| `Source/OutlawsXR` | Headset refresh-rate support |
| `Source/OutlawsUE` | The game module stub |
| `Content/Outlaws` | The start map (`/Game/Outlaws/Maps/L_Runtime`) and our runtime materials |
| `Content/HeldGuns`, `Content/Item3d` | Shapes and paint recipes for held guns and 3D items (no game pixels) |
| `Content/GrabPoints` | Where hands grab each enemy type |
| `Content/Credits` | The VR credits text |
| `tools/olue/item3d` | Python scripts that make the held-gun recipes |
| `tools/olue/questperf` | Quest performance capture helper |
| `tools/olue/upnp` | `mock_igd.py`, a stand-in router for testing the port opening |

## Building

### PC (Win64)
1. Install **Unreal Engine 5.1** and Visual Studio 2022 (MSVC v143).
2. Clone, right-click `OutlawsUE.uproject`, *Generate Visual Studio project files*, and build `OutlawsUEEditor` (Development Editor, Win64), or open the `.uproject` and let the editor compile it.
3. Run in VR: `UnrealEditor.exe OutlawsUE.uproject /Game/Outlaws/Maps/L_Runtime -game -vr`, adding `-OLInstall="<your Outlaws Remastered folder>"` if it can't find the install. Without `-vr` you get a desktop mode (W A S D, mouse, Space, C, Shift, E) for testing.

### Quest (Android)
You need the Android SDK, NDK r25b and JDK 11. Set `ANDROID_HOME`, `NDKROOT` and `JAVA_HOME`, or copy `Config/UserEngine.ini.example` to `Config/UserEngine.ini` (per user, git-ignored) and point it at them. Package for Android (ASTC) from the editor or with RunUAT `BuildCookRun`. The headset also needs the game's files copied across, which the release's `Install on Quest.bat` does.

### OpenXR note
A build from this repo uses the engine's own OpenXR plugin. The released builds carry a small fix to Epic's OpenXR plugin (the swapchain wait and an early recenter no longer end the game). It can't be published here because it is Epic's engine source.

### Python tools
Python 3.10 or later, and `pip install numpy scipy pillow shapely scikit-image mapbox_earcut`. FFmpeg is not in the repo; the release's installer downloads it on first run to convert cutscenes and music.

## Command-line switches

All of ours start with `-OL`. The useful ones:

| Switch | Does |
| --- | --- |
| `-OLInstall=<folder>` | Where Outlaws Remastered is installed |
| `-OLMenuOverlay=<path>`, `-OLNoMenuOverlay` | Use a menu theme overlay KPF, or none |
| `-OLItemDir=<dir>` (or `Config/UserGame.ini.example`, `OUTLAWSVR_ITEMDIR`) | A local folder of placeholder art for a dev build; nothing in it ships |
| `-OLOld<Fix>` | Most fixes keep the old behaviour behind a switch (for example `-OLOldExpLoop`, `-OLOldInkSlots`), for A/B checks |
| `-OL<Name>Probe`, `-OL<Name>Test`, `-OLUpnpSelfTest` | Automated checks that log a PASS or FAIL line; run them with `-unattended -nosound` |

## Contributing

- **Never commit anything from the game:** no textures, sprites, sounds, levels or menu files, not even converted ones. Read them from the install at runtime. Shipped shapes and recipes must rebuild from the player's own files.
- **Fixes match the original.** Check behaviour against the original game first, then The Force Engine and LawMaker.
- **Keep the old way switchable** with an `-OLOld<Name>` switch, and add a probe that logs PASS or FAIL where you can.
- **Mind the Quest.** Every change has to build for Android and hold the frame rate on a Quest 2.
- Open an issue before a large change.

## Credits and licence

- Made by T4H_Dingle. Tested by Dotard and T4H_Dingle. Code under the MIT licence (see `LICENSE`).
- Built with [Unreal Engine 5.1](https://www.unrealengine.com/) by Epic Games (Unreal Engine EULA; the engine is not included) and [Claude](https://claude.ai/) by Anthropic.
- earcut by Mapbox (ISC), ported to C++. libsndfile (LGPL-2.1) for music decoding. FFmpeg, downloaded on first run, for cutscene conversion. Neither libsndfile's source nor FFmpeg is in this repository.
- On the shoulders of The Force Engine by Lucius and LawMaker by Alexei Novikov and David Lovejoy.
- Outlaws is by LucasArts. It is not included; you need your own copy of Outlaws Remastered.

See `THIRD_PARTY_NOTICES.md` for the full notices.
