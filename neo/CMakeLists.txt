cmake_minimum_required(VERSION 3.13)

# Doom 3 BFG Edition — New Nintendo 3DS port
#
# Real build (devkitPro):
#   cmake -S neo -B build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake
# Compile check without devkitPro (clang + newlib/libctru headers):
#   cmake -S neo -B build-check -DCMAKE_TOOLCHAIN_FILE=neo/cmake/3ds-clang-check.cmake

project(doom3ds C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
	# arm-none-eabi GCC defaults to variable-size ("short") enums. idTech's binary
	# formats (.bswf, .bmodel, .bimage, .bcmodel, AAS, ...) read enum fields with
	# sizeof(field), written by MSVC as 4 bytes, so every load would desync.
	-fno-short-enums
	-fno-strict-aliasing		# idTech code type-puns freely
	-Wno-unused-parameter
	-Wno-invalid-offsetof
	"$<$<COMPILE_LANGUAGE:CXX>:-Wno-register>"
)
add_compile_definitions(
	"$<$<CONFIG:Debug>:_DEBUG>"
	"$<$<NOT:$<CONFIG:Debug>>:NDEBUG>"
)

#--------------------------------------------------------------------------
# idlib
#--------------------------------------------------------------------------

set(IDLIB_SOURCES
	idlib/bv/Bounds.cpp
	idlib/bv/Box.cpp
	idlib/bv/Sphere.cpp
	idlib/CommandLink.cpp
	idlib/containers/HashIndex.cpp
	idlib/geometry/DrawVert.cpp
	idlib/geometry/JointTransform.cpp
	idlib/geometry/RenderMatrix.cpp
	idlib/geometry/Surface.cpp
	idlib/geometry/Surface_Patch.cpp
	idlib/geometry/Surface_Polytope.cpp
	idlib/geometry/Surface_SweptSpline.cpp
	idlib/geometry/TraceModel.cpp
	idlib/geometry/Winding.cpp
	idlib/geometry/Winding2D.cpp
	idlib/hashing/CRC32.cpp
	idlib/hashing/MD4.cpp
	idlib/hashing/MD5.cpp
	idlib/math/Angles.cpp
	idlib/math/Complex.cpp
	idlib/math/Lcp.cpp
	idlib/math/Math.cpp
	idlib/math/Matrix.cpp
	idlib/math/MatX.cpp
	idlib/math/Ode.cpp
	idlib/math/Plane.cpp
	idlib/math/Pluecker.cpp
	idlib/math/Polynomial.cpp
	idlib/math/Quat.cpp
	idlib/math/Rotation.cpp
	idlib/math/Simd.cpp
	idlib/math/Simd_Generic.cpp
	idlib/math/Vector.cpp
	idlib/math/VecX.cpp
	idlib/Base64.cpp
	idlib/CmdArgs.cpp
	idlib/Lexer.cpp
	idlib/ParallelJobList.cpp
	idlib/Parser.cpp
	idlib/RectAllocator.cpp
	idlib/SoftwareCache.cpp
	idlib/Str.cpp
	idlib/sys/sys_assert.cpp
	idlib/Thread.cpp
	idlib/Token.cpp
	idlib/BitMsg.cpp
	idlib/Dict.cpp
	idlib/Heap.cpp
	idlib/LangDict.cpp
	idlib/Lib.cpp
	idlib/MapFile.cpp
	idlib/Timer.cpp
)

if(NINTENDO_3DS)
	list(APPEND IDLIB_SOURCES idlib/sys/3ds/3ds_thread.cpp)
else()
	list(APPEND IDLIB_SOURCES idlib/math/Simd_SSE.cpp idlib/sys/win32/win_thread.cpp)
endif()

add_library(idlib STATIC ${IDLIB_SOURCES})
target_include_directories(idlib PUBLIC idlib)

#--------------------------------------------------------------------------
# external (zlib, jpeg-6)
#--------------------------------------------------------------------------

include(cmake/sources.cmake)

add_library(external STATIC ${EXTERNAL_SOURCES})
target_include_directories(external PUBLIC framework/zlib)

#--------------------------------------------------------------------------
# engine (framework, renderer frontend, sound, swf, ui, cm, aas, sys)
#--------------------------------------------------------------------------

add_library(engine STATIC ${ENGINE_SOURCES} ${ENGINE_3DS_SOURCES})
target_compile_definitions(engine PRIVATE __DOOM__)
target_include_directories(engine PRIVATE . ..)
target_link_libraries(engine PUBLIC idlib external)

if(NINTENDO_3DS)
	# n3ds_gpu.c is the only file that includes <3ds.h>/<citro3d.h>: build it with the
	# toolchain's default short enums so libctru/citro3d struct layouts match
	set_source_files_properties(renderer/3DS/n3ds_gpu.c sound/3DS/n3ds_audio.c PROPERTIES COMPILE_OPTIONS "-fshort-enums")

	# application heap cap (MB); the rest of the memory becomes the linear heap
	set(DOOM3DS_APP_HEAP_MB 86 CACHE STRING "New 3DS application heap in MB (max 96); the rest is linear (GPU) memory")
	target_compile_definitions(engine PRIVATE N3DS_APP_HEAP_MB=${DOOM3DS_APP_HEAP_MB})

	# vertex shaders: picasso -> .shbin -> C array
	#   n3ds_vshader      textured world, per-vertex light passes (n3ds_lighting 0/1)
	#   n3ds_vshader_lit  hardware fragment lighting (n3ds_lighting 2)
	#   n3ds_vshader_amb  ambient / blended stages and GUIs
	foreach(N3DS_SHADER n3ds_vshader n3ds_vshader_lit n3ds_vshader_amb)
		set(N3DS_SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/renderer/3DS/${N3DS_SHADER}.v.pica)
		set(N3DS_SHADER_BIN ${CMAKE_CURRENT_BINARY_DIR}/${N3DS_SHADER}.shbin)
		set(N3DS_SHADER_C   ${CMAKE_CURRENT_BINARY_DIR}/${N3DS_SHADER}_shbin.c)
		if(DOOM3DS_COMPILE_CHECK)
			file(WRITE ${N3DS_SHADER_C} "#include <stdint.h>\nconst uint8_t ${N3DS_SHADER}_shbin[4] = {0};\nconst uint32_t ${N3DS_SHADER}_shbin_size = 0;\n")
		else()
			find_program(TOOL_PICASSO picasso HINTS $ENV{DEVKITPRO}/tools/bin REQUIRED)
			add_custom_command(OUTPUT ${N3DS_SHADER_BIN}
				COMMAND ${TOOL_PICASSO} -o ${N3DS_SHADER_BIN} ${N3DS_SHADER_SRC}
				DEPENDS ${N3DS_SHADER_SRC}
				COMMENT "picasso ${N3DS_SHADER}.v.pica")
			add_custom_command(OUTPUT ${N3DS_SHADER_C}
				COMMAND ${CMAKE_COMMAND} -DIN=${N3DS_SHADER_BIN} -DOUT=${N3DS_SHADER_C} -DNAME=${N3DS_SHADER}_shbin -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
				DEPENDS ${N3DS_SHADER_BIN} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake)
		endif()
		target_sources(engine PRIVATE ${N3DS_SHADER_C})
	endforeach()
endif()

#--------------------------------------------------------------------------
# game (d3xp, linked statically like the console builds)
#--------------------------------------------------------------------------

add_library(game STATIC ${GAME_SOURCES})
target_compile_definitions(game PRIVATE __DOOM__ _D3XP CTF)
target_link_libraries(game PUBLIC idlib)

#--------------------------------------------------------------------------
# doomclassic (DOOM 1/2 built into BFG; Common.cpp drives it)
#--------------------------------------------------------------------------

add_library(doomclassic STATIC ${CLASSIC_SOURCES} ${CLASSIC_3DS_SOURCES})
target_compile_definitions(doomclassic PRIVATE _D3XP CTF)
target_include_directories(doomclassic PRIVATE . .. ../doomclassic/doom)
target_link_libraries(doomclassic PUBLIC idlib)

#--------------------------------------------------------------------------
# doom3ds executable (.elf -> .3dsx)
#--------------------------------------------------------------------------

if(DOOM3DS_COMPILE_CHECK)
	# no libctru/libc to link against in the compile-check environment
	add_library(doom3ds_check OBJECT sys/3ds/3ds_exe.cpp)
elseif(NINTENDO_3DS)
	# N3DS_Main() lives in sys/3ds/3ds_main.cpp (engine library); 3ds_exe.cpp holds main().
	add_executable(doom3ds sys/3ds/3ds_exe.cpp)

	# Link the engine libraries whole: many objects (game entity classes, script
	# events, decl types, cvars, console commands) only register themselves through
	# static constructors, and nothing references them directly, so a normal
	# static-library link silently drops them ("Unknown event ..." at startup).
	# On Windows the game was a DLL, which keeps everything.
	target_link_libraries(doom3ds PRIVATE
		-Wl,--whole-archive game engine doomclassic idlib -Wl,--no-whole-archive
		external citro3d ctru m
		# our objects use 32-bit enums, libctru/newlib variable-size ones; enums only
		# cross that boundary by value, so the ABI attribute mismatch is harmless
		-Wl,--no-enum-size-warning)

	set_target_properties(doom3ds PROPERTIES OUTPUT_NAME doom3ds SUFFIX ".elf")

	if(COMMAND ctr_create_3dsx)
		ctr_generate_smdh(doom3ds.smdh
			NAME "DOOM 3 BFG"
			DESCRIPTION "New 3DS port (bring-up)"
			AUTHOR "doom3ds")
		ctr_create_3dsx(doom3ds SMDH doom3ds.smdh)
	else()
		find_program(TOOL_3DSXTOOL 3dsxtool HINTS $ENV{DEVKITPRO}/tools/bin)
		if(TOOL_3DSXTOOL)
			add_custom_command(TARGET doom3ds POST_BUILD
				COMMAND ${TOOL_3DSXTOOL} $<TARGET_FILE:doom3ds> ${CMAKE_BINARY_DIR}/doom3ds.3dsx
				COMMENT "Creating doom3ds.3dsx")
		endif()
	endif()
endif()
