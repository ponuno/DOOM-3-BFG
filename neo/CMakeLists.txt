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
