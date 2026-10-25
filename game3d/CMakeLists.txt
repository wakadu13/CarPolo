cmake_minimum_required(VERSION 3.16)
project(carpolo3d C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# raylib: use a local install if RAYLIB_DIR is given (dev machine), otherwise fetch 5.5 from GitHub.
option(CARPOLO_FETCH_RAYLIB "Fetch raylib 5.5 with FetchContent" ON)
if(DEFINED RAYLIB_DIR)
  add_library(raylib STATIC IMPORTED)
  set_target_properties(raylib PROPERTIES
    IMPORTED_LOCATION ${RAYLIB_DIR}/lib/libraylib.a
    INTERFACE_INCLUDE_DIRECTORIES ${RAYLIB_DIR}/include)
elseif(CARPOLO_FETCH_RAYLIB)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  if(ANDROID)
    set(PLATFORM "Android" CACHE STRING "" FORCE)
  endif()
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
endif()

file(GLOB CARPOLO_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

if(ANDROID)
  # NativeActivity (fr.carpolo.cactus3d.GameActivity, see android/AndroidManifest.xml): the game is
  # libmain.so. Built by android/build-apk.sh (NDK toolchain in CI, or the offline toolchain).
  set(CARPOLO_ANDROID_DIR ${CMAKE_CURRENT_SOURCE_DIR}/android)
  if(NOT CARPOLO_NATIVE_APP_GLUE)
    set(CARPOLO_NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
  endif()
  add_library(main SHARED ${CARPOLO_SOURCES} ${CARPOLO_ANDROID_DIR}/android_bridge.c)
  target_include_directories(main PRIVATE ${CARPOLO_ANDROID_DIR} ${CARPOLO_NATIVE_APP_GLUE})
  target_link_libraries(main PRIVATE raylib log android EGL GLESv2 OpenSLES m)
  target_compile_definitions(main PRIVATE PLATFORM_ANDROID)
  # Keep ANativeActivity_onCreate (raylib's native_app_glue) exported, fail at link time on any
  # missing symbol (instead of a crash on the phone), 16 KB page alignment (Android 15+ devices).
  target_link_options(main PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined
                      -Wl,-z,max-page-size=16384 -Wl,--build-id)
else()
  add_executable(carpolo3d ${CARPOLO_SOURCES})
  target_link_libraries(carpolo3d PRIVATE raylib m)
  if(UNIX AND NOT APPLE)
    target_link_libraries(carpolo3d PRIVATE pthread dl)
  endif()
  # run from game3d/ so that "assets/world.bin" resolves
  set_target_properties(carpolo3d PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build-bin)
endif()
