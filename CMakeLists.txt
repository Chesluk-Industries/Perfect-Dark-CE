cmake_minimum_required(VERSION 3.16)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if(APPLE)
  if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    # ARM Macs start at OS 11.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X deployment version")
  else()
    # SDL2 requires at least this version for everything to work properly.
    # MOD: ragdoll physics - Jolt needs std::shared_mutex (10.12) and aligned new (10.13)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum OS X deployment version")
  endif()
endif()

project(pd C CXX)

include(TargetArch)
include(FindPkgConfig)
include(AssetUtils)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# On first build set the build type to RelWithDebInfo if it's not already set
if (NOT EXISTS ${CMAKE_BINARY_DIR}/CMakeCache.txt)
  if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
  endif()
endif()

# MOD: ragdoll physics (Jolt)
# Jolt is configured here, before the game's global compile flags and
# definitions below, so it builds with clean flags. The C++ wrapper is a
# separate static library (pd_physics) for the same reason.

option(PD_PHYSICS "Build with Jolt physics (ragdolls)" ON)

if(PD_PHYSICS)
  # Looked for in ../../_sources/JoltPhysics-master, then ../../JoltPhysics-master
  get_filename_component(JOLT_DIR_DEFAULT "${CMAKE_SOURCE_DIR}/../../_sources/JoltPhysics-master" ABSOLUTE)
  if(NOT EXISTS "${JOLT_DIR_DEFAULT}/Build/CMakeLists.txt")
    get_filename_component(JOLT_DIR_DEFAULT "${CMAKE_SOURCE_DIR}/../../JoltPhysics-master" ABSOLUTE)
  endif()
  set(JOLT_DIR "${JOLT_DIR_DEFAULT}" CACHE PATH "Path to a local JoltPhysics source tree (downloaded if there's none)")

  # A build folder made before Jolt was moved remembers the old place
  if(NOT EXISTS "${JOLT_DIR}/Build/CMakeLists.txt" AND EXISTS "${JOLT_DIR_DEFAULT}/Build/CMakeLists.txt")
    message(STATUS "Physics: Jolt not at ${JOLT_DIR} any more, using ${JOLT_DIR_DEFAULT}")
    set(JOLT_DIR "${JOLT_DIR_DEFAULT}" CACHE PATH "Path to the JoltPhysics source tree" FORCE)
  endif()

  # No local copy: download the release this build is tested with (needs git)
  set(PD_JOLT_TAG "v5.6.0" CACHE STRING "Jolt Physics release downloaded when there's no local copy")

  if(EXISTS "${JOLT_DIR}/Build/CMakeLists.txt")
    set(PD_JOLT_SOURCE "${JOLT_DIR}")
  else()
    message(STATUS "Physics: no local Jolt at ${JOLT_DIR}, downloading Jolt ${PD_JOLT_TAG}")
    include(FetchContent)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD) # FetchContent_Populate: Jolt is added below with its own options
    endif()
    FetchContent_Declare(joltphysics
      GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
      GIT_TAG ${PD_JOLT_TAG}
      GIT_SHALLOW TRUE)
    FetchContent_GetProperties(joltphysics)
    if(NOT joltphysics_POPULATED)
      FetchContent_Populate(joltphysics)
    endif()
    set(PD_JOLT_SOURCE "${joltphysics_SOURCE_DIR}")
  endif()

  if(NOT EXISTS "${PD_JOLT_SOURCE}/Build/CMakeLists.txt")
    message(FATAL_ERROR "JoltPhysics not found at ${PD_JOLT_SOURCE}. Set -DJOLT_DIR=... or -DPD_PHYSICS=OFF")
  endif()

  # Jolt options: plain static library, no extras, conservative CPU features
  set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
  set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
  set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
  set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
  set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
  set(USE_AVX OFF CACHE BOOL "" FORCE)
  set(USE_AVX2 OFF CACHE BOOL "" FORCE)
  set(USE_AVX512 OFF CACHE BOOL "" FORCE)
  set(USE_LZCNT OFF CACHE BOOL "" FORCE)
  set(USE_TZCNT OFF CACHE BOOL "" FORCE)
  set(USE_F16C OFF CACHE BOOL "" FORCE)
  set(USE_FMADD OFF CACHE BOOL "" FORCE)

  add_subdirectory("${PD_JOLT_SOURCE}/Build" "${CMAKE_BINARY_DIR}/jolt" EXCLUDE_FROM_ALL)

  # 32-bit Windows (MinGW): the C runtime's malloc only aligns to 8 bytes,
  # while GCC assumes new/malloc give 16, so Jolt's SSE data (bounding boxes,
  # bodies, shapes) landed misaligned and the 32-bit build crashed building
  # the level collision (an access violation at ffffffff in MeshShape).
  # -faligned-new=8 makes anything aligned to more than 8 use aligned new,
  # Jolt is told the real alignment (its containers then ask for aligned
  # memory), and since Windows only keeps the stack 4-byte aligned, functions
  # realign it for SSE. (PUBLIC: the physics wrapper sees the same settings.)
  if(WIN32 AND NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(Jolt PUBLIC JPH_DEFAULT_ALLOCATE_ALIGNMENT=8)
    target_compile_options(Jolt PUBLIC -mstackrealign $<$<COMPILE_LANGUAGE:CXX>:-faligned-new=8>)
  endif()

  # Own directory scope, so none of the game's definitions/flags reach it
  add_subdirectory(physics)

  message(STATUS "Physics: Jolt from ${PD_JOLT_SOURCE}")
endif()

# Options

set(ROMID "ntsc-final" CACHE STRING "Which ROM to build an executable for: ntsc-final, pal-final or jpn-final")
option(FORCE_CROSSCOMPILE "Force cross compiling" NO)

# Determine target arch

target_architecture(TARGET_ARCH)
if(TARGET_ARCH STREQUAL "i386")
  # for historical reasons
  set(TARGET_ARCH "i686")
endif()

# TODO
set(TARGET_IS_BIG_ENDIAN FALSE)

if((TARGET_ARCH MATCHES "unknown") OR (TARGET_ARCH STREQUAL ""))
  message(FATAL_ERROR "Unknown target arch")
elseif(TARGET_ARCH MATCHES "64")
  set(TARGET_IS_64BIT TRUE)
else()
  set(TARGET_IS_64BIT FALSE)
endif()

message(STATUS "Target arch: ${TARGET_ARCH}")

# Determine target OS and platform identifier ($arch-$os)

if(WIN32)
  # Windows
  set(TARGET_OS "windows")
elseif(APPLE)
  # OSX
  set(TARGET_OS "osx")
elseif(NINTENDO_SWITCH)
  # Switch
  set(TARGET_OS "nswitch")
elseif(UNIX)
  # Generic *nix; assume Linux
  set(TARGET_OS "linux")
else()
  # ???
  message(FATAL_ERROR "Unknown target OS or compiler")
endif()

set(TARGET_PLATFORM "${TARGET_ARCH}-${TARGET_OS}")

message(STATUS "Target platform: ${TARGET_PLATFORM}")

# Calculate version info and binary name

set(VERSION_IS_PAL 0)

if(ROMID STREQUAL "ntsc-final")
  set(VERSION 2)
  set(BIN_REGION "")
elseif(ROMID STREQUAL "pal-final")
  set(VERSION 4)
  set(BIN_REGION ".pal")
  set(VERSION_IS_PAL 1)
elseif(ROMID STREQUAL "jpn-final")
  set(VERSION 5)
  set(BIN_REGION ".jpn")
else()
  message(FATAL_ERROR "Unknown ROM ID: ${ROMID}")
endif()

# git commit hash
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# git branch name
execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE VERSION_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(VERSION_TARGET "${TARGET_PLATFORM}")
set(VERSION_ARCH "${TARGET_ARCH}")
set(VERSION_ROMID "${ROMID}")
set(VERSION_BUILD "${CMAKE_BUILD_TYPE}")

if((VERSION_BRANCH STREQUAL "") OR (VERSION_BRANCH STREQUAL "HEAD"))
  set(VERSION_BRANCH "port-custom")
endif()

message(STATUS "Version: ${VERSION_ROMID}, hash ${VERSION_HASH}, branch ${VERSION_BRANCH}")

configure_file("${CMAKE_SOURCE_DIR}/port/include/versioninfo.h.in" "${CMAKE_BINARY_DIR}/port/include/versioninfo.h" @ONLY)

set(ASSET_DIR "src/assets/${ROMID}")
set(GENERATED_DIR "src/generated/${ROMID}")

set(ENV{ROMID} ${ROMID})

# Build folder postfix

set(BUILD_ID "${VERSION_ROMID}-${VERSION_TARGET}")

# Compiler defs

add_definitions(
  -DMATCHING=0
  -DPIRACYCHECKS=0
  -DROM_SIZE=32
  -DAVOID_UB=1
  -D_LANGUAGE_C=1
  -DPAL=${VERSION_IS_PAL}
  -DVERSION=${VERSION}
)

if(TARGET_IS_64BIT)
  add_definitions(-DPLATFORM_64BIT=1)
endif()

# MOD: ragdoll physics
if(PD_PHYSICS)
  add_definitions(-DPD_PHYSICS=1)
  include_directories("${CMAKE_SOURCE_DIR}/physics")
endif()

# MOD: clipping checks - drawn floors/ceilings for player clipping (port/src/clipcheck.c, needs PD_PHYSICS)
option(PD_CLIPCHECK "Clipping checks against the level's render geometry" ON)
if(PD_CLIPCHECK AND PD_PHYSICS)
  add_definitions(-DPD_CLIPCHECK=1)
endif()

# MOD: gravity - the gravity setting (port/src/gravity.c)
option(PD_GRAVITY "Gravity setting (falling, ragdolls, debris)" ON)
if(PD_GRAVITY)
  add_definitions(-DPD_GRAVITY=1)
endif()

# Compiler flags

# Include directories
include_directories("${CMAKE_SOURCE_DIR}/include")
include_directories("${CMAKE_SOURCE_DIR}/include/PR")
include_directories("${CMAKE_SOURCE_DIR}/src/include")
include_directories("${CMAKE_BINARY_DIR}/src/generated/${ROMID}")
include_directories("${CMAKE_SOURCE_DIR}/src/lib/ultra/audio")
include_directories("${CMAKE_SOURCE_DIR}/port/include")
include_directories("${CMAKE_BINARY_DIR}/port/include")

# Set Release optimization to -Og until I fix the -O2 issues
if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  add_compile_options(-Og)
endif()

# Common CFLAGS
add_compile_options(
  -fno-inline-functions
  -fno-strict-aliasing
  -funsigned-char
  -fwrapv
  -include "versioninfo.h"
)

# Common warning flags
add_compile_options(
  -Wall
  -Wno-address
  -Wno-int-in-bool-context
  -Wno-misleading-indentation
  -Wno-missing-braces
  -Wno-multichar
  -Wno-tautological-compare
  -Wno-unused-but-set-variable
  -Wno-unused-value
  -Wno-unused-variable
  -fdiagnostics-color=always
)

# C specific flags
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Wno-pointer-sign>)

# C++ specific flags
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-function>)

# Platform-specific flags
if (APPLE)
  # Make it look for libs in the executable folder manually since CMake's rpath system is wack
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rpath @executable_path")
else()
  # This warning is not recognized by XCode/Clang for some reason.
  add_compile_options(-Wno-format-truncation)
endif()

# Dependencies

set(LIBS )

# SDL2
find_package(SDL2 REQUIRED)
if(NINTENDO_SWITCH)
  if(NOT SDL2_LIBRARY OR NOT SDL2_INCLUDE_DIR)
    # sometimes FindSDL2 just wants us to suffer and does not set these
    set(SDL2_LIBRARY "SDL2")
    set(SDL2_INCLUDE_DIR "${DEVKITPRO}/portlibs/switch/include/SDL2")
  endif()
endif()
include_directories(SYSTEM "${SDL2_INCLUDE_DIR}")
list(APPEND LIBS "${SDL2_LIBRARY}")

# ZLib
find_package(ZLIB REQUIRED)
if(ZLIB_INCLUDE_DIR)
  include_directories(SYSTEM "${ZLIB_INCLUDE_DIR}")
endif()
list(APPEND LIBS "${ZLIB_LIBRARY}")

# GL
if(NOT GL_LIBRARY)
  if(WIN32)
    set(GL_LIBRARY opengl32)
  elseif(NINTENDO_SWITCH)
    set(GL_LIBRARY EGL glapi drm_nouveau)
  elseif(APPLE)
    set(GL_LIBRARY "-framework OpenGL")
  else()
    set(GL_LIBRARY GL)
  endif()
endif()
list(APPEND LIBS "${GL_LIBRARY}")

# Platform-specific extra libs
if(NOT EXTRA_LIBRARIES)
  if(WIN32)
    set(EXTRA_LIBRARIES dbghelp -static-libstdc++)
  elseif(NINTENDO_SWITCH)
    set(EXTRA_LIBRARIES stdc++ pthread)
  else()
    set(EXTRA_LIBRARIES stdc++ m dl)
  endif()
endif()
list(APPEND LIBS "${EXTRA_LIBRARIES}")

# Sources

file(GLOB_RECURSE SRC_PORT "${CMAKE_SOURCE_DIR}/port/*.c")
file(GLOB_RECURSE SRC_PORT_CXX "${CMAKE_SOURCE_DIR}/port/*.cpp")

file(GLOB_RECURSE SRC_GAME "${CMAKE_SOURCE_DIR}/src/game/*.c")

file(GLOB SRC_LIB_MP3 "${CMAKE_SOURCE_DIR}/src/lib/mp3/*.c")
file(GLOB SRC_LIB_NAUDIO "${CMAKE_SOURCE_DIR}/src/lib/naudio/*.c")
file(GLOB SRC_LIB_ULTRA_AUDIO "${CMAKE_SOURCE_DIR}/src/lib/ultra/audio/*.c")
file(GLOB SRC_LIB_GU "${CMAKE_SOURCE_DIR}/src/lib/ultra/gu/*.c")
set(SRC_LIB
  src/lib/ultra/io/vimodentsclan1.c
  src/lib/ultra/io/vimodepallan1.c
  src/lib/ultra/io/vimodempallan1.c
  src/lib/ultra/io/vitbl.c
  src/lib/ailist.c
  src/lib/anim.c
  src/lib/args.c
  src/lib/audiodma.c
  src/lib/audiomgr.c
  src/lib/base.c
  src/lib/collision.c
  src/lib/debughud.c
  src/lib/dma.c
  src/lib/fault.c
  src/lib/joy.c
  src/lib/lib_2f490_c.c
  src/lib/lib_2fc60.c
  src/lib/lib_3e3e0.c
  src/lib/lib_04f60nb.c
  src/lib/lib_17ce0.c
  src/lib/lib_034d0.c
  src/lib/lib_39c80.c
  src/lib/lib_15850.c
  src/lib/mema.c
  src/lib/memp.c
  src/lib/model.c
  src/lib/modelasm_c.c
  src/lib/mp3.c
  src/lib/mtx_c.c
  src/lib/mtx.c
  src/lib/music.c
  src/lib/path.c
  src/lib/profile.c
  src/lib/rdp.c
  src/lib/rmon.c
  src/lib/rng_c.c
  src/lib/rzip_c.c
  src/lib/snd.c
  src/lib/speaker.c
  src/lib/varsinit.c
  src/lib/vi.c
  src/textureconfig.c
)
list(PREPEND SRC_LIB "${CMAKE_SOURCE_DIR}")

set(SRC
  ${SRC_GAME}
  ${SRC_PORT}
  ${SRC_PORT_CXX}
  ${SRC_LIB_MP3}
  ${SRC_LIB_NAUDIO}
  ${SRC_LIB_ULTRA_AUDIO}
  ${SRC_LIB_GU}
  ${SRC_LIB}
)

if(WIN32)
  list(APPEND SRC "${CMAKE_SOURCE_DIR}/dist/windows/icon.rc")
endif()

# Asset files

generate_asset_headers("lang/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mklang" "en" JSON_HEADERS)
generate_asset_headers("pads/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mkpads" "" JSON_HEADERS)
generate_asset_headers("tiles/" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mktiles" "" JSON_HEADERS)
generate_asset_headers("animations.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mkanims" "" JSON_HEADERS)
generate_asset_headers("sequences.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mksequences" "" JSON_HEADERS)
generate_asset_headers("textures.json" "${CMAKE_SOURCE_DIR}/tools/assetmgr/mktextures" "" JSON_HEADERS)

add_custom_target(pd_headers ALL DEPENDS ${JSON_HEADERS})

# Game target

set(BIN_NAME "pd${BIN_REGION}.${TARGET_ARCH}")
add_executable(pd ${SRC})
add_dependencies(pd pd_headers)
target_link_libraries(pd ${LIBS})
if(PD_PHYSICS)
  target_link_libraries(pd pd_physics) # MOD: ragdoll physics
  if(WIN32)
    target_link_libraries(pd pthread) # MOD: ragdoll physics - the physics step's worker threads
  endif()
endif()
set_target_properties(pd PROPERTIES OUTPUT_NAME "${BIN_NAME}")
if(WIN32)
  set_target_properties(pd PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Additional commands for specific platforms.

if(NINTENDO_SWITCH)
  add_custom_command(TARGET pd POST_BUILD
    COMMAND nacptool --create "Perfect Dark" "fgsfds and contributors" "${VERSION_ROMID}/${VERSION_BRANCH}/${VERSION_HASH}" "${BIN_NAME}.nacp"
    COMMAND aarch64-none-elf-strip -o ${CMAKE_BINARY_DIR}/${BIN_NAME}.stripped.elf ${CMAKE_BINARY_DIR}/${BIN_NAME}.elf
    COMMAND elf2nro ${CMAKE_BINARY_DIR}/${BIN_NAME}.stripped.elf ${CMAKE_BINARY_DIR}/${BIN_NAME}.nro --icon=${CMAKE_SOURCE_DIR}/dist/nswitch/icon.jpg --nacp=${BIN_NAME}.nacp
  )
endif()
