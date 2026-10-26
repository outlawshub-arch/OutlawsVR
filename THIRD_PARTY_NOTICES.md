# Third-party notices

## earcut (ISC)

`Source/OutlawsRuntime/OutlawsTriangulate.cpp` is a C++ port of earcut.js 2.2.4 by Mapbox. The Python tools in
`tools/olue/item3d` use the `mapbox_earcut` package, which carries the same licence.

```
ISC License

Copyright (c) 2016, Mapbox

Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright notice
and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA
OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

## Unreal Engine

This project is built with Unreal Engine 5.1, which is licensed under the Unreal Engine End User License
Agreement (https://www.unrealengine.com/eula). The engine is not included in or redistributed by this
repository; get it from Epic Games. The `.uasset` and `.umap` files here are our own materials and map, saved
by the editor.

## libsndfile (LGPL-2.1)

Not in this repository. `Source/OutlawsRuntime/OutlawsRuntime.Build.cs` stages Unreal Engine's own unmodified
`libsndfile-1.dll` into Windows builds, where the game uses it as a separate DLL to decode the music of your own
Outlaws install.

- Copyright (C) 1999-2016 Erik de Castro Lopo and the libsndfile contributors.
- Licence: GNU Lesser General Public License version 2.1.
- Source: https://github.com/libsndfile/libsndfile

If you redistribute a Windows build, include the LGPL-2.1 text and keep the DLL replaceable (the game loads it as a
separate file, so a user can swap in their own build).

## Real-ESRGAN ncnn Vulkan (MIT; model and ncnn BSD-3-Clause)

Not in this repository. Windows builds can use `realesrgan-ncnn-vulkan.exe` and the `realesr-animevideov3` x2
model from the official v0.2.5.0 release, unmodified, run as a separate program on the player's PC to make sharper
copies of the frames grabbed enemies are shown in. To build with it, put the official release files in
`ThirdParty/RealESRGAN` (the build rules list them as runtime files; without them the game skips the upscale).

- Real-ESRGAN, Real-ESRGAN-ncnn-vulkan: Copyright (c) 2021 Xintao Wang. MIT License.
- realsr-ncnn-vulkan: Copyright (c) 2019 nihui. BSD 3-Clause License.
- ncnn: Copyright (C) 2017 THL A29 Limited, a Tencent company. BSD 3-Clause License.
- Source: https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan

## FFmpeg (GPL build, not included)

Not in this repository or the release. On first run, the installer script (`olvr_install.py`) downloads a Windows
build of FFmpeg from https://github.com/BtbN/FFmpeg-Builds and runs it as a separate program to convert the
cutscenes and music of your own Outlaws install. FFmpeg is licensed under the GPL (that build); see
https://ffmpeg.org/legal.html.

## Android SDK Platform-Tools (adb, not included)

Not in this repository. The Quest install script uses Google's Android SDK Platform-Tools (adb), downloaded from
https://dl.google.com/android/repository/ on first use, under the Android Software Development Kit License
Agreement.

## Unreal Engine's Python Editor Script Plugin (PythonScriptPlugin, not part of the game)

Not in this repository. The project uses Epic's PythonScriptPlugin in the editor only (its tests and tools), but two
engine plugins the game uses (Niagara and SequencerScripting) list it, so packaged builds (Windows and Quest) still
stage its `PythonScriptPlugin.uplugin` and build in its small `PythonScriptPluginPreload` module, which does nothing
in a game. Both are Epic's own unmodified files, under the Unreal Engine End User License Agreement
(https://www.unrealengine.com/eula), like the rest of the engine.

## Boost (BSL-1.0), including boost_python39

Not in this repository. Windows builds stage Unreal Engine's own unmodified Boost 1.70 DLLs next to the game
(`boost_atomic`, `boost_chrono`, `boost_iostreams`, `boost_program_options`, `boost_python39`, `boost_regex`,
`boost_system` and `boost_thread`, `-vc142-mt-x64-1_70.dll`), because the engine's Boost module lists them as runtime
files. The game does not call them. Quest builds do not include them.

- Licence: Boost Software License 1.0 (https://www.boost.org/LICENSE_1_0.txt).
- Source: https://www.boost.org/

```
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

## Unreal Engine third-party components

Builds made with Unreal Engine include libraries the engine itself ships, under their own licences (for example
Opus in the Voice module, BSD-3-Clause; Boost, BSL-1.0; oneTBB and OpenImageDenoise, Apache-2.0). They come with
the engine and are covered by Epic's own third-party notices.

## Acknowledgements (references, no code included)

- **The Force Engine** by Lucius (https://github.com/luciusDXL/TheForceEngine, GPL-2.0): a reconstruction of
  the Jedi engine that Outlaws is built on. Outlaws VR's level-script (INF), sector and actor behaviour was
  learned in part from reading it. No code from The Force Engine is included in Outlaws VR.
- **LawMaker**, the Outlaws level editor by Alexei Novikov and David Lovejoy: wall and sector flag values and level rules were checked against it.
  No code from LawMaker is included.

## Outlaws

Outlaws (1997) was made by LucasArts; Outlaws Remastered is published by Lucasfilm Games and Nightdive Studios.
None of the game's files, art, sounds, music or text are included here. Outlaws VR reads them at runtime from
the player's own copy of Outlaws Remastered. This project is not affiliated with or endorsed by Lucasfilm,
Lucasfilm Games, LucasArts, Disney or Nightdive Studios. Outlaws is a trademark of its owners.
