# DOOM 3 BFG → New Nintendo 3DS

Base: vanilla id-Software/DOOM-3-BFG (32-bit, console-oriented, binary resource formats).

## Build

Real build (devkitPro with devkitARM, libctru, citro3d installed), from the MSYS2/devkitPro shell:

    cmake -S neo -B build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
    cmake --build build -- -k -j8

Output: `build/doom3ds.3dsx`, and `build/doom3ds.cia` when `makerom` and `bannertool` are found
in `$DEVKITPRO/tools/bin` or on PATH (Windows: put `makerom.exe` from
github.com/3DSGuy/Project_CTR releases and `bannertool.exe` from github.com/diasurgical/bannertool
releases into `C:\devkitPro\tools\bin`, then reconfigure).

The CIA is the real build: its RSF (`neo/sys/3ds/cia/doom3ds.rsf`) asks for the 178 MB New 3DS
memory mode (`SystemModeExt: 178MB`, 804 MHz, L2 cache). A .3dsx started from the Homebrew
Launcher runs in the 124 MB mode of the title it borrows, which the heaviest maps (phobos3,
mc_underground) do not fit. Install the CIA with FBI; title id 000400000D3BF000.

On Linux without access to the devkitPro package servers, `tools/n3ds/build-devkitarm-linux.sh`
builds the same toolchain from GitHub sources into `/opt/devkitpro` (binutils 2.46, GCC 16.1.0 with
the devkitARM patch, devkitPro newlib, libctru, citro3d, picasso, 3dsxtool; only the default and
`armv6k/fpu` multilibs) and the CIA tools makerom and bannertool. Then:

    . ~/devkitpro-env.sh
    cmake -S neo -B build -G "Unix Makefiles" -DCMAKE_MAKE_PROGRAM=/usr/bin/make -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
    cmake --build build -- -k -j3

Compile check without devkitPro (host clang 18 + newlib/libctru/citro3d headers in ~/sdk;
compiles everything, links nothing):

    cmake -S neo -B build-check -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/neo/cmake/3ds-clang-check.cmake
    cmake --build build-check

Target: armv6k / MPCore, VFPv2, hard-float, C++14 (gnu), exceptions ON
(the engine unwinds `common->Error()` through `idException`).

## SD card layout

    sdmc:/3ds/doom3ds/doom3ds.3dsx   (dev build, 124 MB; or install doom3ds.cia with FBI)
    sdmc:/3ds/dspfirm.cdc            (DSP firmware dump; without it the game runs silent)
    sdmc:/doom3/base/default.cfg
    sdmc:/doom3/base/strings/english.lang
    sdmc:/doom3/base/_common.resources
    sdmc:/doom3/base/_ordered.resources
    sdmc:/doom3/base/_sound_pc.resources
    sdmc:/doom3/base/_sound_pc_<language>.resources   (e.g. _sound_pc_en.resources)
    sdmc:/doom3/base/maps/<map>.resources
    sdmc:/doom3/base/_n3ds_textures.pak  (optional, made on the PC by n3dstex: see "Texture pack")
    sdmc:/doom3/cmdline.txt          (optional: extra command line, e.g. "+map game/mars_city1")
    sdmc:/doom3/log.txt              (written by the game)
    sdmc:/doom3/save/                (fs_savepath)

The `_sound_pc*.resources` hold the sample data: without them every sound is an empty
placeholder and the game is silent (sound itself stays on; `+set s_noSound 1` turns it off).
Built-in defaults (`Sys_BuildCommandLine`): `com_skipIntroVideos 1`, `com_disableAutoSaves 1`,
`r_cullDynamicLightTriangles 0`.

## Structure

Libraries: `idlib`, `external` (zlib, jpeg-6), `engine` (framework, renderer, sound, swf, ui, cm,
aas, sys + sys/3ds), `game` (d3xp, linked statically as on consoles), `doomclassic`.
They reference each other and are linked as a group into `doom3ds`.

Platform layer `neo/sys/3ds`:
- `3ds_main.cpp` — `N3DS_Main` (called from `main` in `3ds_exe.cpp`), time, paths, files,
  events, logging (bottom screen + log.txt), libctru stack/heap sizes.
- `3ds_input.cpp` — 3DS buttons as one Xbox-style pad (A/B/X/Y, L/R shoulders, ZL/ZR triggers,
  Circle Pad = left stick, C-Stick = right stick). START+SELECT quits.
  Bindings are set in code after every config (PlayerProfile.cpp): Circle Pad move, C-Stick look,
  ZR fire, ZL flashlight, A jump, B crouch, X reload, Y / D-pad up next weapon, L / D-pad down
  previous weapon, R run, SELECT PDA.
- `3ds_glimp.cpp` — "window" (400x240 top screen).
- `3ds_net.cpp` — no networking (loopback only).
- `3ds_session_local.cpp`, `3ds_savegame.cpp`, `3ds_localuser/signin/achievements` — from win32.
- `idlib/sys/3ds/3ds_thread.cpp` — threads, LightEvent signals, RecursiveLock mutexes, atomics, TLS.

Renderer: compiled unchanged against a **null GL driver** (`renderer/OpenGL/gl_null`):
Mesa `gl.h` for declarations, generated stubs for every GL entry point the engine uses
(`gen_gl_null.py` + `gl_null.cpp.in` → `gl_null.cpp`). Buffer objects are emulated in RAM
because the vertex cache writes through mapped pointers. The citro3d backend will replace
`tr_backend_draw`/`gl_*` piece by piece.

Sound: `sound/3DS` — NDSP output with a software mixer, see "Sound" below.

## Platform conventions

- `ID_3DS` (set from `__3DS__` in `idlib/sys/sys_defines.h`) is the platform define.
  It also sets `ID_CONSOLE` and `ID_LITTLE_ENDIAN`.
- SSE paths stay behind `ID_WIN_X86_SSE*_INTRIN` (never defined here), so the
  generic C paths written for PS3/360 are used.
- Never include the full `<3ds.h>` next to engine headers (name clashes). Include only the
  needed `3ds/*.h`, **inside `extern "C" { }`** (only `<3ds.h>` adds that itself).
- File handles (`idFileHandle`) are `FILE *` with a 64 KB stdio buffer.

## Status

- [x] idlib, external, engine, game, doomclassic compile for ARM11 (clang check + devkitARM for idlib)
- [x] sys/3ds platform layer, null GL
- [x] first run on hardware: boot, map load, memory / tick numbers
- [x] citro3d renderer: diffuse textures, PICA fragment lighting, ambient stages, GUIs
- [x] NDSP sound with a software mixer (PCM16, MS ADPCM)
- [ ] normal/specular maps, shadows, cube/screen texgens, cinematics
- [ ] offline asset pipeline (DXT → ETC1, downscaled textures)
- [ ] saves, menus on the touch screen

## Memory decisions (first pass, to be tuned from logs)

- Vertex cache: static 16 MB vertex + 6 MB index, per frame 3 MB + 1 MB (x2). PC: ~186 MB total.
- libctru: main stack 2 MB (default 32 KB is too small), linear heap 8 MB (default: half of RAM).
- Threads: 2 job workers, Game/Draw (core 2 on New 3DS), AutoRender, savegame workers.

## citro3d backend

`renderer/3DS/`: `n3ds_gpu.c` (the only file including `<3ds.h>`/`<citro3d.h>`, built with short enums),
`n3ds_backend.cpp` (walks the frontend's draw surfaces), `n3ds_vshader.v.pica` (built with picasso
into a C array by `cmake/bin2c.cmake`). Vertex/index data are read straight from the vertex cache
buffers, which the null GL layer keeps in linear memory. Build option `-DDOOM3DS_APP_HEAP_MB=86`
(default) caps the application heap; the rest becomes linear memory. mars_city1 needs ~90 MB of
heap: build with 96 and `+set n3ds_renderer 0` for it until the asset converter exists.

Textures (`n3ds_textures.cpp`, `n3ds_texconv.cpp`): at image load one level no larger than
`n3ds_texMaxSize` of every diffuse/default/coverage image is chosen. With `n3ds_texStream 1`
(default) only its place in the `.resources` container is remembered; a loader thread reads it
(own unbuffered FILE, ~10 KB) the first time the image is drawn, and the surface is untextured for
those few frames. Uploaded levels stay in a RAM LRU cache of `n3ds_texRAMCacheKB`. Images without a
file behind them (generated, tiny files read into memory) keep their level in RAM as stored.
When a surface is drawn, that level is decoded, mipmapped, tiled (8x8 Morton tiles) and copied to
VRAM as RGB565 / RGBA5551 / RGBA4 / A8, a few per frame; VRAM is an LRU cache. The shader decodes
the half-float st itself (table of powers of two in a uniform array). Perforated materials use
their coverage image as an alpha-test mask on texture unit 1. Generated images (`_white`,
`_noFalloff`, `_quadratic`, ...) are kept from `idImage::GenerateImage`.

Lighting (`n3ds_backend.cpp`): a fill pass draws every opaque/perforated surface once (diffuse at
`n3ds_ambient`, unlit materials at full brightness, depth written), then every interaction of
every light is added with depth EQUAL: light projection (unit 0, per-pixel divide with the PICA
projection mode) x falloff (unit 1) x diffuse (unit 2) x light colour x max(0, N.L) per vertex,
x4 in the last combiner stage so `r_lightScale` 3 overbright survives the 0..1 vertex colour.
No normal/specular maps and no shadows yet.

`n3ds_lighting 2` (default): second vertex program `n3ds_vshader_lit.v.pica` outputs the eye-space
normal quaternion and view vector; every surface is drawn once with the PICA fragment lighting.
Its lights come from the interaction lists (matched to the view surface by vertex cache handle and
space). Point lights -> positional lights with a shared distance attenuation table scaled by the
radius (derived from the light projection planes); projected lights -> spot lights (axis and cone
from the projection frustum, range from the falloff plane); ambient lights -> surface ambient.
Ambient stages and GUIs (`N3DS_DrawAmbientStages`, third program `n3ds_vshader_amb.v.pica`): after
the lit passes every SL_AMBIENT stage is drawn as RB_DrawShaderPasses does (blend/depth/alpha-test
state from drawStateBits, stage colour x vertex colour, texture matrix, polygon offset); unlit opaque
materials are filled black so their stages land on black. RC_DRAW_VIEW_GUI views (HUD, menus,
loading screens) are drawn the same way without depth; SWF stages (GLS_OVERRIDE) add the colour2
transform and premultiply alpha like gui.pixel; SWF masks use the stencil (cleared to 128 every
frame, test/incr/decr from the GL state bits). GUI images keep a level up to `n3ds_texGuiMaxSize`
(512); fonts up to `n3ds_texFontMaxSize` (1024: a 1024x1280 atlas keeps 512x640) as alpha-only A8
textures (white x vertex colour). Sides that are not a power of two are area-resampled to the one
below (nearest sampling used to drop whole rows of glyphs). SWF stages are laid out for 720 lines; fitted to the 240-line screen
they shrink to a third, so `n3ds_hudScale`/`n3ds_uiScale` enlarge them (edge-anchored sprites such as
`_bottomLeft` stay pinned to the screen edges); the log prints every SWF's stage size at load. Not yet: TG_SCREEN/cube texgens, cinematics, program stages, scissor.
Skinned models are skinned on the CPU (`r_useGPUSkinning` forced to 0 with citro3d).
Frame vertex cache: three sets of 2 MB vertexes + 512 KB indexes (PC: two). The game thread's
frontend of frame N+2 fills its set while the GPU may still draw frame N; the PC waits for the
GPU in `GL_BlockingSwapBuffers`, the null GL cannot, so a third set keeps the one being filled
away from any frame still on the GPU. The GUI model's block is 32768 vertexes / 49152 indexes
(16-bit indexes; the PC's 80000 vertexes could not all be addressed). Released textures are
deleted only once the frames that may sample them are finished, and are unbound first.

## Texture pack (tools/n3ds/n3dstex)

`n3dstex` converts every 2D image of the game's `.resources` files on the PC into what the
PICA200 samples directly, and writes them into one file, `_n3ds_textures.pak`
(format: `neo/renderer/3DS/n3ds_pack.h`). Each texture is its whole mip chain (down to 8 on the
short side), flipped and tiled as the backend lays textures out, so the console copies it to
VRAM as it is. With the pack the console decodes no DXT, resamples and tiles nothing, and does not
open the `.bimage` files at all at map load (the pack entry carries the header fields).

| image (usage from the `#__UUCC` in the name) | pack format | default size |
|---|---|---|
| diffuse, default, light, specular | ETC1 (4 bpp), ETC1A4 (8 bpp) when the alpha is used | world 256, specular 128, light 128 |
| bump (normal maps; BFG: DXT5 with x in alpha, y in green) | HILO8 (x hi, y lo; z rebuilt by the GPU) | 128 |
| coverage (alpha-test masks) | A4 | world |
| fonts | A8 | 1024 |
| GUI art (`guis/`, `generated/swf/`), lookup tables | RGB565 / RGBA5551 / RGBA4 | 512 / 256 |

Cube maps are left out (the backend does not draw them). Normal maps are renormalized on
every mip level; the ETC1 blocks come from rg_etc1 (zlib licence), medium quality by default.

    n3dstex <base folder> [-o file] [-world 256] [-bump 128] [-spec 128] [-light 128] [-gui 512]
            [-font 1024] [-q 0|1|2] [-j threads] [-only text] [-preview dir] [-list] [-v]

`<base folder>`: the one with `_common.resources` and `maps/` (the PC game's `base`, or a copy
of the SD card's `doom3/base`); the pack is written there unless `-o` says otherwise and goes to
`sdmc:/doom3/base/`. It must stay under 2 GB. `tools/n3ds/n3dstex/build.sh` builds the Linux
binary and, with llvm-mingw, a static `n3dstex.exe`; `CMakeLists.txt` there builds it on any PC.

On the console (`n3ds_textures.cpp`): the pack index (40 bytes per texture) is read at the first
image load; `idBinaryImage::LoadFromGeneratedFile` looks every 2D image up by the hash of its
generated file name. A packed image is streamed from the pack by the texture loader thread when
first drawn, like the kept levels of the old path, and uploaded with one copy. The chain is cut to
`n3ds_texWorldMaxSize` (256), `n3ds_texBumpMaxSize` (128), `n3ds_texSpecMaxSize` (128),
`n3ds_texGuiMaxSize`, `n3ds_texFontMaxSize` by starting further down it. Images the pack does not
have, and everything with `+set n3ds_texPack 0`, take the old path (`n3ds_texMaxSize`, decoded on
the console). The log says `citro3d: texture pack ...: N textures` or `no texture pack`, and the
`PERF citro3d: tex` line counts uploads from the pack and packed textures.

## Memory modes and the second heap

| | .3dsx (124 MB) | CIA (178 MB) |
|---|---|---|
| application heap (malloc) | 86 MB | 96 MB (the whole heap window) |
| linear heap | ~25 MB, all GPU | ~70 MB: GPU (~36 MB) + second heap (~28 MB) |
| static vertex / index cache | 8.5 / 3.5 MB | 20 / 6 MB |
| engine memory | 86 MB | ~124 MB |

The heap's virtual window is 96 MB in every mode (0x08000000-0x0E000000), so the extra
memory of the 178 MB mode can only be linear memory. `Sys_N3DSExtendedMemory()` tells the
renderer to size the static caches for it. `Sys_N3DSInitSecondHeap` (right after
`common->Init`, when every long-lived GPU buffer exists) keeps `N3DS_GPU_HEADROOM_MB` (6) of
linear memory for citro3d, takes the rest with one `linearAlloc` and gives it to
`Mem_AddHeapRegion` (idlib/Heap.cpp), which makes it a second region of newlib's malloc
itself: Heap.cpp defines `_sbrk_r` (weak in libsysbase), which hands out the heap window
and, once that is used up, the linear region. malloc handles the jump like a "foreign sbrk"
(the old top chunk is fenced off and freed). Before the region is added, malloc is made to
take the whole rest of the window (one big malloc + free, trimming off) so the jump strands
nothing. Everything that allocates uses both regions: the engine, libctru (`threadCreate`
stacks), newlib (FILE buffers). Use `Mem_MallInfo()` instead of `mallinfo()`: malloc counts
the skipped address space between the regions as in use. The MEM report shows
`second heap: malloc has taken N KB of M KB`; the tour counts the region as part of the heap.

No load step needs one block of a whole big text file: `.map` files (up to 10+ MB) are
and `.aas` files (several MB) are read by `idLexer` through a 256 KB window
(`LEXFL_STREAMFILE`, set by `idMapFile::Parse` and `idAASFileLocal::Load`),
refilled as tokens are read. After a few level changes the heap has plenty free but not
one 10 MB piece (le_exis2 failed with 52 MB free, largest 9.4 MB).

A map whose static geometry does not fit the static caches still loads to the end (the
surfaces that do not fit get no static handle) and then stops with
`static vertex/index cache too small for this map: needs N KB of vertexes (have M KB), ...`,
so one run gives the size it needs (`n3ds_staticVertexKB` / `n3ds_staticIndexKB`).

## Map tour (memory budgets)

`n3ds_mapTour [map ...]` loads maps one after another (default: the 23 biggest
`maps/*.resources`, heaviest first), lets each run `n3ds_tourFrames` frames (600) and logs
one `TOUR` line per map: malloc peak, heap footprint (what malloc took from the heap) and what
is left, least free linear memory, static vertex/index cache use, load time. A summary with
the worst map for each column ends the tour. Start it from `cmdline.txt`: `+n3ds_mapTour`.
Between maps the game goes back to the main menu as on a real "new game", so fragmentation
across level changes is part of the test. Running out of memory stops the tour with the
usual MEM report.

## Sound

`sound/3DS/`: `n3ds_audio.c` (NDSP, the only sound file including `<3ds.h>`). Every playing voice
gets one of the DSP's 24 channels: the DSP resamples it (channel rate = sample rate x pitch,
polyphase), applies its left/right levels and mixes all channels in hardware. The CPU only keeps
each channel's 3 wave buffers of 1024 frames filled (288 KB of linear memory in all): a feeder thread
on the system core (priority 0x18), woken by the DSP every ~5 ms, copies 16-bit PCM and decodes MS
ADPCM block by block into them (`idSoundVoice_3DS::Feed`). Mono voices use the DSP's levels directly
(updated at once); stereo ones get their 2x2 `CalculateSurround` matrix applied while filling.
`s_maxEmitterChannels` is 24 on 3DS, so the sound world mutes the quietest sounds beyond that.
The old software mixer (one channel, everything resampled and mixed on the CPU) cost up to 1 s of
CPU per second with 16 voices on the time-sliced system core and ran dry in fights.
Formats: 16-bit PCM and MS ADPCM (bit-exact with ffmpeg's decoder), mono/stereo. Sample data: the `.idwav` header is read at
level load, data of samples up to `n3ds_soundPreloadKB` too; bigger ones are read on first play by a
loader thread (own unbuffered FILE on the container), the voice starting in `Update()` once the data
is there (long sounds keep their timing); all data shares `n3ds_soundMemoryKB` with
least-recently-played eviction. Needs the DSP firmware dump
`sdmc:/3ds/dspfirm.cdc` (DSP1); without it the game runs silent. `+set s_noSound 1` turns sound off.

## Skipping cinematics

Not in BFG, but in the original Doom 3: during a cinematic (camera view), A (jump) or ZR (attack)
fast-forwards it with the sound muted until the camera is released (`idGameLocal::SkipCinematic`,
`RunFrame`). Game frames run for up to `g_cinematicSkipMsPerFrame` (200) ms per rendered frame so
the app stays responsive, at most `g_cinematicMaxSkipTime` (600) s of game time; a map change asked
for meanwhile ends the skip. Cameras with `instantSkip` stop at once, as in Doom 3. `g_cinematicSkip 0`
turns it off.

## Runtime switches (cmdline.txt, e.g. `+set n3ds_animQuant 0`)

| cvar | default | effect |
|---|---|---|
| `n3ds_animQuant` | 1 | MD5 anim frames stored as 16-bit; load log prints sizes and max error |
| `n3ds_noShadows` | 1 | shadow/silhouette data skipped at load, prelight shadow models dropped, no light casts shadows |
| `n3ds_mapEntitiesOnly` | 1 | `.map` brushes/patches discarded while parsing (CRC kept; collision from `.bcmodel`) |
| `n3ds_lazySaveBuffer` | 1 | savegame buffers not preallocated (4.4 MB) |
| `n3ds_skipImageData` | 1 | `.bimage` pixel data is seeked over, not read (null GL); resources opened as streams are no longer buffered whole |
| `n3ds_lazyInteractions` | 0 | light/entity interactions are not all pre-generated at map start (created when first in view) |
| `n3ds_discardStaticGeo` | 1 | null GL: static vertex/index cache uploads dropped, their buffers never get storage (-8 MB heap, -3 MB linear) |
| `n3ds_renderer` | 1 | 0 = null GL (nothing drawn), 1 = citro3d (stage 3: diffuse textures, per-vertex lights) |
| `n3ds_textures` | 1 | citro3d: 0 = flat colour per material |
| `n3ds_texMaxSize` | 128 | largest texture side; set at startup (the kept level is chosen at load) |
| `n3ds_texUploadsPerFrame` | 4 | textures decoded and copied to VRAM per frame |
| `n3ds_texFlip` | 1 | texture rows stored bottom-up; toggle if textures are upside down |
| `n3ds_texMips` | 1 | mip maps |
| `n3ds_texUploadsAfterLoad` | 256 | upload limit for the first 20 frames after a map load (at most 24 copies fit in one frame) |
| `n3ds_lighting` | 2 | 0 = full brightness, 1 = a pass per light (light images, per-vertex N.L), 2 = PICA fragment lighting: one pass, per pixel, up to 8 lights per surface |
| `n3ds_hwLightScale` | 4 | lighting 2: overbright range (2 or 4) |
| `n3ds_hwSpotFlip` | 0 | lighting 2: reverse spot light directions |
| `n3ds_hwLightRadius` | 1 | lighting 2: point light reach as a fraction of the smallest half-size of its box |
| `n3ds_ambient` | 0.1 | brightness of lit surfaces no light reaches (Doom 3: 0) |
| `n3ds_lightProjective` | 1 | light projection divided per pixel; 0 = per vertex |
| `n3ds_lightScissor` | 1 | light passes clipped to the light's screen rectangle |
| `n3ds_c3dValidate` | 0 | check every draw's indexes against its buffers; bad draws are skipped and logged |
| `n3ds_hwMaxLights` | 8 | lighting 2: most lights per surface, the brightest kept (fragment lighting costs GPU time per light per pixel) |
| `n3ds_c3dHangHunt` | 0 | from this 3D frame on, wait for the GPU after every draw and log the one it never finishes (slow) |
| `n3ds_c3dFullFlush` | 0 | 1 = clean the data cache over all linear memory at every frame end (citro3d's default; 69 MB per frame in the 178 MB mode). Off: only the command list; everything else the GPU reads is flushed where it is written |
| `n3ds_soundMemoryKB` | 4096 | sound sample data budget |
| `n3ds_soundPreloadKB` | 16 | samples up to this size are read at level load; bigger ones by the loader thread when first played |
| `n3ds_soundVolume` | 1 | DSP master volume |
| `n3ds_ambientStages` | 1 | ambient/blended stages and fullscreen GUIs |
| `n3ds_texGuiMaxSize` | 512 | largest GUI / HUD texture |
| `n3ds_texPack` | 1 | take textures from `base/_n3ds_textures.pak` (n3dstex) when it is there |
| `n3ds_texWorldMaxSize` | 256 | texture pack: largest world texture (the top of the chain is skipped) |
| `n3ds_texBumpMaxSize` | 128 | texture pack: largest normal map |
| `n3ds_texSpecMaxSize` | 128 | texture pack: largest specular map |
| `n3ds_texFontMaxSize` | 1024 | largest font atlas level kept (A8) |
| `n3ds_texLinearKB` | 3072 | textures that do not fit in VRAM go to linear memory (FCRAM) up to this much; the GPU samples them from there |
| `n3ds_texLinearReserveKB` | 2048 | linear memory the texture cache always leaves free |
| `n3ds_texStream` | 1 | texture levels read from SD when first drawn instead of all kept in RAM (applies to images loaded afterwards) |
| `n3ds_texRAMCacheKB` | 1024 | RAM for already uploaded streamed levels, so VRAM evictions come back without a read |
| `n3ds_tourFrames` | 600 | map tour: frames each map runs before the next |
| `n3ds_hudScale` | 1.8 | HUD size relative to fitting the 720-line SWF stage to the 240-line screen; edge-anchored elements stay at the edges |
| `n3ds_uiScale` | 1.35 | same for menus, PDA, loading screen and dialogs; too large crops unanchored elements |
| `n3ds_polygonOffset` | 4 | depth bias per material polygonOffset unit (decals) |
| `n3ds_debugPhysics` | 0 | heavy physics diagnostics: NaN checks per AF step, LCP dumps, trace counters, SLOW AF lines |
| `n3ds_staticVertexKB` / `n3ds_staticIndexKB` | 0 | static vertex/index cache size; 0 = 8704/3584 with citro3d (frame index cache 1 MB, no joint buffers), 10240/6144 budget-only with null GL |
| `n3ds_core1Percent` | 30 | `APT_SetAppCpuTimeLimit` for the system core |

Per-subsystem heap breakdown (`Mem_LogTags`): create an empty file `sdmc:/doom3/memtags`. It costs a 16-byte header per block (~4 MB on a loaded map), so it is off by default.

Once a map runs, a `PERF` line is logged every 2 s: fps, game ms per tick, draw/frontend/backend ms.

## Known issues to revisit

- `ALIGNTYPE16 struct decal_t` — the attribute is ignored by GCC/clang in that position.
- Only the idlib part has been through devkitARM GCC so far.
