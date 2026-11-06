# Native part of the Android build: SDL2 from source (libSDL2.so) and the port itself (libmain.so, loaded by
# SDLActivity, which calls its SDL_main). The emulation core, the game modules and the app are the Windows
# build's; only the platform layer differs (src/platform/android).
cmake_minimum_required(VERSION 3.18)
project(SmasAndroid C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
set(SDL_DIR ${ROOT}/third_party/SDL2-2.32.10-src)
if(NOT EXISTS ${SDL_DIR}/CMakeLists.txt)
  message(FATAL_ERROR "SDL2 source not found in ${SDL_DIR}: run android/fetch_sdl.ps1 first")
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_DIR} ${CMAKE_BINARY_DIR}/SDL)

set(SRC ${ROOT}/src)
add_library(main SHARED
  ${SRC}/core/cpu65816.cpp
  ${SRC}/core/apu.cpp
  ${SRC}/core/dsp.cpp
  ${SRC}/core/ppu.cpp
  ${SRC}/core/snes.cpp
  ${SRC}/core/rewind.cpp
  ${SRC}/core/msu1.cpp
  ${SRC}/core/png.cpp
  ${SRC}/core/fileio.cpp
  ${SRC}/core/disasm.cpp
  ${SRC}/game/smas.cpp
  ${SRC}/game/smb1.cpp
  ${SRC}/game/smb1coop.cpp
  ${SRC}/game/smb2.cpp
  ${SRC}/game/smb3.cpp
  ${SRC}/game/smb3coop.cpp
  ${SRC}/game/achievements.cpp
  ${SRC}/game/palette.cpp
  ${SRC}/app/app.cpp
  ${SRC}/app/config.cpp
  ${SRC}/app/menu.cpp
  ${SRC}/app/launcher.cpp
  ${SRC}/app/music.cpp
  ${SRC}/app/achstore.cpp
  ${SRC}/app/utf8.cpp
  ${SRC}/app/sys_posix.cpp
  ${SRC}/app/imageload.cpp
  ${SRC}/app/msupack.cpp
  ${SRC}/app/achexport.cpp
  ${SRC}/app/updatecheck.cpp
  ${SRC}/app/netplay.cpp
  ${SRC}/app/app_net.cpp
  ${SRC}/platform/android/app_android.cpp
  ${SRC}/platform/android/video_gles.cpp
  ${SRC}/platform/android/audio_sdl.cpp
  ${SRC}/platform/android/input_sdl.cpp
  ${SRC}/platform/android/touch.cpp
)
set_target_properties(main PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(main PRIVATE ${SRC} ${ROOT}/third_party/stb)
# (the emulation core needs optimising even in debug builds to run at full speed)
target_compile_options(main PRIVATE -O2 -Wno-unused-parameter -Wno-sign-compare)
target_link_libraries(main PRIVATE SDL2 GLESv3 android log)
