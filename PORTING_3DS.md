# DOOM 3 BFG → New Nintendo 3DS

Base: vanilla id-Software/DOOM-3-BFG (32-bit, console-oriented, binary resource formats).

## Build

Real build (devkitPro with devkitARM, libctru, citro3d installed):

    cmake -S neo -B build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
    cmake --build build

Compile check without devkitPro (host clang 18 + newlib/libctru/citro3d headers in ~/sdk):

    cmake -S neo -B build-check -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/neo/cmake/3ds-clang-check.cmake
    cmake --build build-check

Target: armv6k / MPCore, VFPv2, hard-float, C++14 (gnu), exceptions ON
(the engine unwinds `common->Error()` through `idException`).

## Platform conventions

- `ID_3DS` (set from `__3DS__` in `idlib/sys/sys_defines.h`) is the only platform
  define used in code. It also sets `ID_CONSOLE` and `ID_LITTLE_ENDIAN`.
- `ID_OPENGL` is NOT defined on 3DS. `renderer/OpenGL/qgl.h` pulls in
  `renderer/Citro3D/gl_types_3ds.h` (GL scalar types + a few enums only);
  no GL function is ever called. The GPU backend will live in `renderer/Citro3D`.
- SSE paths stay behind `ID_WIN_X86_SSE*_INTRIN` (never defined here), so the
  generic C paths written for PS3/360 are used.
- Never include the full `<3ds.h>` next to engine headers (name clashes, e.g.
  `DS_FINISHED`). Include only the needed `3ds/*.h`.
- File handles (`idFileHandle`) are `FILE *` (sdmc:/ and romfs:/ via newlib).

## Status

- [x] idlib compiles for ARM11 (`idlib/sys/3ds/3ds_thread.cpp`: threads, LightEvent
      signals, RecursiveLock mutexes, atomics, TLS slots)
- [ ] framework + `sys/3ds` (main loop, filesystem, timers, input stubs)
- [ ] null renderer / null sound, headless map load (RAM + tick time on hardware)
- [ ] offline asset pipeline (ETC1, downscaled textures)
- [ ] citro3d renderer
- [ ] ndsp sound, input, saves, menus

## Known issues to revisit

- Job system creates ~50 threads with 256 KB stacks; Horizon allows ~32 threads
  per process and memory is tight. Needs a 3DS-specific worker count / stack size.
- Thread cores: CORE_1x → core 2 (New 3DS extra core, needs osSetSpeedupEnable),
  CORE_2x → core 1 (syscore, needs APT_SetAppCpuTimeLimit). Falls back to core 0.
- Only checked with clang so far; first devkitARM GCC build may surface more.
