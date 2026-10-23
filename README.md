# DOOM 3 BFG Edition for New Nintendo 3DS

A fan-made port of the DOOM 3 BFG Edition GPL source code to the New Nintendo 2DS/3DS.

## Installing

### What you need

- **A New Nintendo 3DS, New 3DS XL or New 2DS XL.** The original 3DS / 2DS will not work: the port
  needs the New models' 178 MB memory mode and 804 MHz CPU.
- **Custom firmware** (Luma3DS) with **FBI** to install CIA files.
- **Your own copy of DOOM 3 BFG Edition for PC** (Steam or GOG). The port ships no game data.
- **A PC (Windows)** to convert the game data with `n3dstex`.
- **The DSP firmware dump** for sound (made once with DSP1, see step 3).
- **Free space on the SD card** for the converted game folder.

### 1. Convert the game data on the PC

The console cannot use the PC textures, sounds and videos as they are. `n3dstex` converts them into
formats the 3DS hardware reads directly and makes an SD-ready copy of the game folder.

1. Run `n3dstex.exe` (no arguments: it opens a window).
2. **Game folder:** your DOOM 3 BFG Edition folder, or its `base` folder (the one with
   `_common.resources` and `maps\`).
3. Tick **textures**, **sounds**, **videos** and **complete SD folder**.
4. Press **Start** and wait. Converting everything takes a while.

The result is written to `outputDoom3` next to the game's `base` folder, already laid out like the
SD card:

    outputDoom3/doom3/base/_n3ds_textures.pak
    outputDoom3/doom3/base/_n3ds_sounds.pak
    outputDoom3/doom3/base/video/...
    outputDoom3/doom3/base/... (the rest of the game files, slimmed down)

Your original game files are only read, never changed.

Command line alternative (same tool):

    n3dstex <game or base folder> -sd

### 2. Copy the game to the SD card

Copy the **contents** of `outputDoom3` to the **root** of the SD card, so you end up with:

    sdmc:/doom3/base/_common.resources
    sdmc:/doom3/base/_ordered.resources
    sdmc:/doom3/base/_sound_pc.resources
    sdmc:/doom3/base/_sound_pc_<language>.resources   (e.g. _sound_pc_en.resources)
    sdmc:/doom3/base/_n3ds_textures.pak
    sdmc:/doom3/base/_n3ds_sounds.pak
    sdmc:/doom3/base/default.cfg
    sdmc:/doom3/base/strings/...
    sdmc:/doom3/base/maps/...
    sdmc:/doom3/base/video/...

### 3. Dump the DSP firmware (for sound)

Run **DSP1** once on the console. It writes `sdmc:/3ds/dspfirm.cdc`. Without that file the game runs
but is silent.

### 4. Install the game

Copy `doom3ds.cia` to the SD card, open **FBI** → SD → `doom3ds.cia` → **Install CIA**.
DOOM 3 BFG then appears on the HOME menu (title ID `000400000D3BF000`).

Use the CIA. The `.3dsx` build from the Homebrew Launcher only gets the 124 MB memory mode, and the
biggest maps do not fit in it.

### 5. Play

Start it from the HOME menu.

#### Controls

| Button | In game | In menus |
|---|---|---|
| Circle Pad | move | navigate |
| C-Stick | look around | |
| ZR | fire | |
| ZL | flashlight | |
| A | jump | select |
| B | crouch | back |
| X | reload | |
| Y / D-pad up | next weapon | |
| L / D-pad down | previous weapon | |
| R (hold) | run | |
| SELECT | PDA | |
| START | pause menu | |
| START + SELECT | quit the game | |

During a cutscene, A or ZR fast-forwards it.

The bottom screen is a touch panel:

- **GYRO AIM**: aim by turning the console. Hold the console still for a moment after switching it on
  while it calibrates.
- **FPS COUNTER**: frame rate in the top right corner of the top screen.
- **HIDE SCREEN**: switches the bottom screen off. Tap anywhere to switch it back on.
- **C-STICK SENSITIVITY**: tap or drag the slider.

Saves go to `sdmc:/doom3/save/`. Autosaves are off; save from the pause menu.

### Optional settings

Create `sdmc:/doom3/cmdline.txt` to add settings to the command line, for example:

    +set n3ds_uiMinTextPx 12 +set n3ds_gyroInvertPitch 1

All settings are listed in `PORTING_3DS.md` (section "Runtime switches").

### Diagnostics (for testers and bug reports)

Everything below is off for normal play and is switched on in `sdmc:/doom3/cmdline.txt`.

| Setting | What it does |
|---|---|
| `+set n3ds_debug 1` | Writes performance and memory diagnostics to `sdmc:/doom3/log.txt`: `PERF` lines every 2 seconds (frame rate, CPU and GPU times, draws, textures, sound), `SPIKE` lines for every frame longer than 36 ms with a breakdown of where its time went, `SLOW` lines, memory snapshots at map load and save, and which CPU core each thread runs on. Also shows the **GPU TEST** button on the bottom screen. |
| `+set n3ds_spikeMs 40` | With `n3ds_debug`: the frame length (ms) from which a `SPIKE` line is written (default 36, `0` = none). |
| **GPU TEST** button | With `n3ds_debug`: stand still and tap it. For ~20 seconds the same view is drawn in six modes (normal, almost no pixels, no light passes, no textures, one triangle per draw, normal again); the log gets the GPU time of each and a summary of what the GPU time goes to. The picture looks odd meanwhile. |
| `+set n3ds_c3dFrameRing 8` | With `n3ds_debug`: keeps the last 8 frames (GPU command lists, pictures, draw lists) in memory; a touch on the lower half of the bottom screen writes them to `sdmc:/doom3/ringN.bin` / `ringN.fb`. For rendering bugs that last a frame or two (flicker). |
| `+set n3ds_c3dFrameDump 3` | With `n3ds_debug`: a touch on the lower half of the bottom screen writes the next 3 frames' GPU command lists to `sdmc:/doom3/frameN.bin`. The game stalls while they are written. |
| `+set n3ds_gyroDebug 1` | Logs the gyroscope's rates once a second (to check the axes and directions). |

Example `cmdline.txt` for a performance report:

    +set n3ds_debug 1

Without `n3ds_debug` the log still gets the engine's normal messages, and on a fatal error the error
message with a memory snapshot.

### If something goes wrong

- **No sound:** check that `sdmc:/3ds/dspfirm.cdc` exists (step 3).
- **Missing or blurry textures, very long loading:** check that `_n3ds_textures.pak` is in
  `sdmc:/doom3/base/`.
- **Crash or error:** the game writes `sdmc:/doom3/log.txt`. Attach it to your bug report; for
  performance problems run once with `+set n3ds_debug 1` in `cmdline.txt` first.
  A fatal error is also shown on the bottom screen; press START to exit.
- The largest maps are close to the console's memory limit and can still run out of memory.

## License

This port is based on the DOOM 3 BFG Edition GPL Source Code and is distributed under the same
terms: the GNU GPL v3 with id Software's additional terms (see `COPYING.txt`). The game data is not
included and stays under the original EULA.

The original README of the source release, with the license notices of the third-party code, follows
unchanged:

<details>
<summary>Original Doom 3 BFG Edition README</summary>

```text
Doom 3 BFG Edition GPL Source Code
==================================

This file contains the following sections:

GENERAL NOTES
LICENSE

GENERAL NOTES
=============

Game data and patching:
-----------------------

This source release does not contain any game data, the game data is still
covered by the original EULA and must be obeyed as usual.

You must patch the game to the latest version.

Note that Doom 3 BFG Edition is available from the Steam store at
http://store.steampowered.com/app/208200/


Compiling on win32:
-------------------

A project file for Microsoft Visual Studio 2010 is provided in neo\doom3.sln
We expect the solution file is compatible with the Express releases

You will need the Microsoft DirectX SDK installed as well.
If it does not reside in "C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)"
you will need to update the project files accordingly.


Steam:
------
The Doom 3 BFG Edition GPL Source Code release does not include functionality for integrating with 
Steam.  This includes roaming profiles, achievements, leaderboards, matchmaking, the overlay, or
any other Steam features.


Bink:
-----

The Doom 3 BFG Edition GPL Source Code release does not include functionality for rendering Bink Videos.


Back End Rendering of Stencil Shadows:
--------------------------------------

The Doom 3 BFG Edition GPL Source Code release does not include functionality enabling rendering
of stencil shadows via the "depth fail" method, a functionality commonly known as "Carmack's Reverse".


Other platforms, updated source code, security issues:
------------------------------------------------------

If you have obtained this source code several weeks after the time of release,
it is likely that you can find modified and improved versions of the engine in
various open source projects across the internet.

Depending what is your interest with the source code, those may be a better starting point.


LICENSE
=======

See COPYING.txt for the GNU GENERAL PUBLIC LICENSE

ADDITIONAL TERMS:  The Doom 3 BFG Edition GPL Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU GPL which accompanied the Doom 3 BFG Edition GPL Source Code.  If not, please request a copy in writing from id Software at id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

EXCLUDED CODE:  The code described below and contained in the Doom 3 BFG Edition GPL Source Code release is not part of the Program covered by the GPL and is expressly excluded from its terms.  You are solely responsible for obtaining from the copyright holder a license for such code and complying with the applicable license terms.

JPEG library
-----------------------------------------------------------------------------
neo/renderer/jpeg-6/*

Copyright (C) 1991-1995, Thomas G. Lane

Permission is hereby granted to use, copy, modify, and distribute this
software (or portions thereof) for any purpose, without fee, subject to these
conditions:
(1) If any part of the source code for this software is distributed, then this
README file must be included, with this copyright and no-warranty notice
unaltered; and any additions, deletions, or changes to the original files
must be clearly indicated in accompanying documentation.
(2) If only executable code is distributed, then the accompanying
documentation must state that "this software is based in part on the work of
the Independent JPEG Group".
(3) Permission for use of this software is granted only if the user accepts
full responsibility for any undesirable consequences; the authors accept
NO LIABILITY for damages of any kind.

These conditions apply to any software derived from or based on the IJG code,
not just to the unmodified library.  If you use our work, you ought to
acknowledge us.

NOTE: unfortunately the README that came with our copy of the library has
been lost, so the one from release 6b is included instead. There are a few
'glue type' modifications to the library to make it easier to use from
the engine, but otherwise the dependency can be easily cleaned up to a
better release of the library.

zlib library
---------------------------------------------------------------------------
neo/framework/zlib/*

Copyright (C) 1995-2005 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
 claim that you wrote the original software. If you use this software
 in a product, an acknowledgment in the product documentation would be
 appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
 misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

Base64 implementation
---------------------------------------------------------------------------
neo/idlib/Base64.cpp

Copyright (c) 1996 Lars Wirzenius.  All rights reserved.

June 14 2003: TTimo <ttimo@idsoftware.com>
	modified + endian bug fixes
	http://bugs.debian.org/cgi-bin/bugreport.cgi?bug=197039

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

IO for uncompress .zip files using zlib
---------------------------------------------------------------------------
neo/framework/Unzip.cpp
neo/framework/Unzip.h

Copyright (C) 1998 Gilles Vollant
zlib is Copyright (C) 1995-1998 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

MD4 Message-Digest Algorithm
-----------------------------------------------------------------------------
neo/idlib/hashing/MD4.cpp
Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All
rights reserved.

License to copy and use this software is granted provided that it
is identified as the "RSA Data Security, Inc. MD4 Message-Digest
Algorithm" in all material mentioning or referencing this software
or this function.

License is also granted to make and use derivative works provided
that such works are identified as "derived from the RSA Data
Security, Inc. MD4 Message-Digest Algorithm" in all material
mentioning or referencing the derived work.

RSA Data Security, Inc. makes no representations concerning either
the merchantability of this software or the suitability of this
software for any particular purpose. It is provided "as is"
without express or implied warranty of any kind.

These notices must be retained in any copies of any part of this
documentation and/or software.

MD5 Message-Digest Algorithm
-----------------------------------------------------------------------------
neo/idlib/hashing/MD5.cpp
This code implements the MD5 message-digest algorithm.
The algorithm is due to Ron Rivest.  This code was
written by Colin Plumb in 1993, no copyright is claimed.
This code is in the public domain; do with it what you wish.

CRC32 Checksum
-----------------------------------------------------------------------------
neo/idlib/hashing/CRC32.cpp
Copyright (C) 1995-1998 Mark Adler

OpenGL headers
---------------------------------------------------------------------------
neo/renderer/OpenGL/glext.h
neo/renderer/OpenGL/wglext.h

Copyright (c) 2007-2012 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.

Timidity
---------------------------------------------------------------------------
doomclassic/timidity/*

Copyright (c) 1995 Tuukka Toivonen 

From http://www.cgs.fi/~tt/discontinued.html :

If you'd like to continue hacking on TiMidity, feel free. I'm
hereby extending the TiMidity license agreement: you can now 
select the most convenient license for your needs from (1) the
GNU GPL, (2) the GNU LGPL, or (3) the Perl Artistic License.
```

</details>
