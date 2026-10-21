# DOOM 3 BFG → New Nintendo 3DS

Base: vanilla id-Software/DOOM-3-BFG (32-bit, console-oriented, binary resource formats).

## Build

Real build (devkitPro with devkitARM, libctru, citro3d installed), from the MSYS2/devkitPro shell:

    cmake -S neo -B build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
    cmake --build build -- -k -j8

Output: `build/doom3ds.3dsx`.

Compile check without devkitPro (host clang 18 + newlib/libctru/citro3d headers in ~/sdk;
compiles everything, links nothing):

    cmake -S neo -B build-check -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/neo/cmake/3ds-clang-check.cmake
    cmake --build build-check

Target: armv6k / MPCore, VFPv2, hard-float, C++14 (gnu), exceptions ON
(the engine unwinds `common->Error()` through `idException`).

## SD card layout

    sdmc:/3ds/doom3ds/doom3ds.3dsx
    sdmc:/doom3/base/default.cfg
    sdmc:/doom3/base/strings/english.lang
    sdmc:/doom3/base/_common.resources
    sdmc:/doom3/base/_ordered.resources
    sdmc:/doom3/base/maps/<map>.resources
    sdmc:/doom3/cmdline.txt          (optional: extra command line, e.g. "+map game/mars_city1")
    sdmc:/doom3/log.txt              (written by the game)
    sdmc:/doom3/save/                (fs_savepath)

The `_sound_pc*.resources` are not needed while sound is null (`s_noSound 1` is a built-in default, as are `com_skipIntroVideos 1` and `com_disableAutoSaves 1`, `r_cullDynamicLightTriangles 0`).

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

Sound: `sound/3DS` — sample loader from the XAudio2 version, null hardware/voices (voices
"play" for the sample length so scripts waiting on sounds progress).

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
- [x] sys/3ds platform layer, null GL, null sound
- [ ] first run on hardware: boot, map load, memory / tick numbers
- [ ] offline asset pipeline (DXT → ETC1, downscaled textures)
- [ ] citro3d renderer
- [ ] ndsp sound, saves, menus on the touch screen

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
`n3ds_texMaxSize` of every diffuse/default/coverage image is kept in RAM as stored (DXT etc.).
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
transform and premultiply alpha like gui.pixel. GUI/font images keep a level up to
`n3ds_texGuiMaxSize` (512). Not yet: TG_SCREEN/cube texgens, cinematics, program stages, scissor.
Skinned models are skinned on the CPU (`r_useGPUSkinning` forced to 0 with citro3d).

## Sound

`sound/3DS/`: `n3ds_audio.c` (NDSP, the only sound file including `<3ds.h>`) plays one stereo
PCM16 channel; a mixer thread on the system core (priority 0x18) fills 4 buffers of
`n3ds_soundBufferFrames` through `idSoundHardware_3DS::MixBlock`. Voices are mixed in software:
16-bit PCM and MS ADPCM (bit-exact with ffmpeg's decoder), mono/stereo, linear resampling to
`n3ds_soundRate`, stereo levels from `CalculateSurround`. Sample data: the `.idwav` header is read at
level load, data of samples up to `n3ds_soundPreloadKB` too, bigger ones on first play; all data
shares `n3ds_soundMemoryKB` with least-recently-played eviction. Needs the DSP firmware dump
`sdmc:/3ds/dspfirm.cdc` (DSP1); without it the game runs silent. `+set s_noSound 1` turns sound off.

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
| `n3ds_c3dValidate` | 0 | check every draw's indexes against its buffers (bring-up) |
| `n3ds_soundRate` | 22050 | mixing rate (Hz) |
| `n3ds_soundMemoryKB` | 8192 | sound sample data budget |
| `n3ds_soundPreloadKB` | 48 | samples up to this size are read at level load |
| `n3ds_soundVolume` | 1 | mixer master volume |
| `n3ds_ambientStages` | 1 | ambient/blended stages and fullscreen GUIs |
| `n3ds_texGuiMaxSize` | 512 | largest GUI / HUD / font texture |
| `n3ds_polygonOffset` | 4 | depth bias per material polygonOffset unit (decals) |
| `n3ds_debugPhysics` | 0 | heavy physics diagnostics: NaN checks per AF step, LCP dumps, trace counters, SLOW AF lines |
| `n3ds_staticVertexKB` / `n3ds_staticIndexKB` | 0 | static vertex/index cache size; 0 = 6144/2048 with citro3d, 10240/6144 budget-only with null GL |
| `n3ds_core1Percent` | 30 | `APT_SetAppCpuTimeLimit` for the system core |

Per-subsystem heap breakdown (`Mem_LogTags`): create an empty file `sdmc:/doom3/memtags`. It costs a 16-byte header per block (~4 MB on a loaded map), so it is off by default.

Once a map runs, a `PERF` line is logged every 2 s: fps, game ms per tick, draw/frontend/backend ms.

## Known issues to revisit

- `ALIGNTYPE16 struct decal_t` — the attribute is ignored by GCC/clang in that position.
- Only the idlib part has been through devkitARM GCC so far.
