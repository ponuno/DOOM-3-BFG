# Compile-check toolchain: host clang targeting the New 3DS ARM11 (MPCore),
# using newlib + libctru + citro3d headers. Produces objects ABI-compatible
# with devkitARM (armv6k, VFPv2, hard-float). Not used for real builds —
# on a machine with devkitPro use: -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv6k)
set(NINTENDO_3DS TRUE)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(SDK_ROOT "$ENV{HOME}/sdk" CACHE PATH "Compile-check SDK root")
execute_process(COMMAND clang -print-resource-dir OUTPUT_VARIABLE CLANG_RES OUTPUT_STRIP_TRAILING_WHITESPACE)

set(_3DS_FLAGS "--target=arm-none-eabihf -mcpu=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft -ffunction-sections -ffreestanding -nostdinc -D__3DS__ -D_3DS -D__DEVKITARM__ -ferror-limit=0")
set(_3DS_INC "-isystem ${SDK_ROOT}/sysroot/include -isystem ${CLANG_RES}/include -I${SDK_ROOT}/libctru/libctru/include -I${SDK_ROOT}/citro3d/include")

set(CMAKE_C_FLAGS_INIT "${_3DS_FLAGS} ${_3DS_INC}")
set(CMAKE_CXX_FLAGS_INIT "${_3DS_FLAGS} -nostdinc++ -isystem ${SDK_ROOT}/sysroot/c++ ${_3DS_INC}")
set(DOOM3DS_COMPILE_CHECK TRUE)
