# CAI native layer for the macOS (Aspyr, Steam) build of Civilization VI.
# Produces libcai.dylib, injected into Civ6_Exe_Child by steam_launch.sh
# through DYLD_INSERT_LIBRARIES. Apple Silicon only. Design and internals:
# mac/README.md. Build from the repo: mac/dev/build.sh.
cmake_minimum_required(VERSION 3.24)

# Forced on purpose: the dylib is loaded into an arm64 process on macOS 13 or
# later, and a build for anything else would not load. Not overridable.
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "" FORCE)

project(cai_mac LANGUAGES C CXX OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "This target only builds on macOS.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- Dependencies, all pinned downloads --------------------------------------
include(FetchContent)

# prism (speech backends, MPL-2.0): the prebuilt universal static library from
# its GitHub release. Its backends register through static initializers, so
# the archive is force-loaded below.
FetchContent_Declare(prism
  URL https://github.com/ethindp/prism/releases/download/v0.18.2/prism-macos-universal.zip
  URL_HASH SHA256=268cd7aa14e201f3281a35a0e39bd8b7f9085b9e044c2aa49e51ee1556f44062
)

# miniaudio (sound engine, MIT-0 / public domain): the single header at the
# pinned tag.
FetchContent_Declare(miniaudio
  URL https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
  URL_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
  DOWNLOAD_NO_EXTRACT TRUE
)

# SimpleIni (INI config, MIT): the single header at the pinned tag. The same
# library the Windows integration uses for its config file.
FetchContent_Declare(simpleini
  URL https://raw.githubusercontent.com/brofield/simpleini/v4.26/SimpleIni.h
  URL_HASH SHA256=a421245f1377337c65e6f6ea845ad29d21f1950303d8b43208c7f80d52203929
  DOWNLOAD_NO_EXTRACT TRUE
)

FetchContent_MakeAvailable(prism miniaudio simpleini)

set(PRISM_INCLUDE_DIR "${prism_SOURCE_DIR}/include")
set(PRISM_STATIC_LIB "${prism_SOURCE_DIR}/static/release/lib/libprism.a")

# --- miniaudio implementation unit (C) ---------------------------------------
add_library(miniaudio STATIC miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_options(miniaudio PRIVATE -fvisibility=hidden)
target_link_libraries(miniaudio PUBLIC
  "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation")

# --- libcai.dylib ------------------------------------------------------------
# The game's Lua exports are resolved with dlsym at run time (hks.cpp), so the
# dylib links against nothing but the system frameworks and prism, and the
# linker checks every remaining reference. ld64 ad-hoc signs arm64 output on
# its own; the release script re-signs with a Developer ID.
add_library(cai SHARED
  entry.cpp
  handshake.cpp
  inject.cpp
  hks.cpp
  luaapi.cpp
  speechRouter.cpp
  audio.cpp
  speech.mm
  platform.mm
  keyboard.mm
)
set_target_properties(cai PROPERTIES OUTPUT_NAME cai)
target_include_directories(cai PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PRISM_INCLUDE_DIR}"
  "${simpleini_SOURCE_DIR}"
)
target_compile_options(cai PRIVATE
  -fvisibility=hidden
  -Wall -Wextra -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_link_options(cai PRIVATE
  "LINKER:-force_load,${PRISM_STATIC_LIB}"
)
target_link_libraries(cai PRIVATE
  miniaudio
  "-framework AppKit" "-framework Foundation" "-framework AVFoundation"
  "-framework CoreGraphics" "-framework IOKit" "-framework Carbon"
)

# --- speechtest: stand-alone driver for the system-voice stream --------------
# Not part of the default build. Lists voices, speaks a few lines, and
# measures the queue timing; see speechtest.cpp for the options.
add_executable(speechtest EXCLUDE_FROM_ALL speech.mm speechtest.cpp)
target_compile_options(speechtest PRIVATE -Wall -Wextra -Wno-unused-parameter
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(speechtest PRIVATE
  "-framework AVFoundation" "-framework Foundation" "-framework AppKit")

# --- samplestest: host test for speech_samples.h -----------------------------
# Not part of the default build; exits non-zero on a failed check.
add_executable(samplestest EXCLUDE_FROM_ALL speech_samples_test.cpp)
target_compile_options(samplestest PRIVATE -Wall -Wextra)
