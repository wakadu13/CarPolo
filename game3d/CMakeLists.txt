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
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
  if(ANDROID)
    set(PLATFORM "Android" CACHE STRING "" FORCE)
  elseif(EMSCRIPTEN)
    set(PLATFORM "Web" CACHE STRING "" FORCE)
  endif()
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
  if(ANDROID)
    # raylib 5.5 asks EGL for a 16-bit depth buffer: far buildings, windows and road markings then
    # flicker (z-fighting). Every GLES2 GPU in use offers 24 bits.
    set(_rc ${raylib_SOURCE_DIR}/src/platforms/rcore_android.c)
    file(READ ${_rc} _rcs)
    string(REPLACE "EGL_DEPTH_SIZE, 16," "EGL_DEPTH_SIZE, 24," _rcs2 "${_rcs}")
    if(NOT _rcs2 STREQUAL _rcs)
      file(WRITE ${_rc} "${_rcs2}")
    endif()
  endif()
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
elseif(EMSCRIPTEN)
  # ---- web (browser): one .html/.js/.wasm/.data bundle, see web/ and .github/workflows/web3d.yml
  set(CARPOLO_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
  add_executable(carpolo3d ${CARPOLO_SOURCES})
  target_link_libraries(carpolo3d PRIVATE raylib m)
  target_compile_definitions(carpolo3d PRIVATE PLATFORM_WEB)
  set_target_properties(carpolo3d PROPERTIES SUFFIX ".html" OUTPUT_NAME "index"
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build-web-bin)
  target_link_options(carpolo3d PRIVATE
    -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB
    -sMIN_WEBGL_VERSION=1 -sMAX_WEBGL_VERSION=1 -sASSERTIONS=0 -sENVIRONMENT=web -sFORCE_FILESYSTEM=1 -sMINIFY_HTML=0
    "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
    "--shell-file=${CARPOLO_WEB_DIR}/shell.html")
  if(EMSCRIPTEN_VERSION VERSION_GREATER_EQUAL 3.1.25)
    target_link_options(carpolo3d PRIVATE -sSTACK_SIZE=2MB)
  else()
    target_link_options(carpolo3d PRIVATE -sTOTAL_STACK=2MB)
  endif()
  set_property(TARGET carpolo3d APPEND PROPERTY LINK_DEPENDS ${CARPOLO_WEB_DIR}/shell.html ${CMAKE_CURRENT_SOURCE_DIR}/assets/world.bin)
else()
  # ---- desktop: Linux, Windows (MinGW or MSVC), macOS
  set(CARPOLO_DESKTOP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/desktop)
  set(CARPOLO_DESKTOP_EXTRA "")
  if(WIN32)
    enable_language(RC)
    list(APPEND CARPOLO_DESKTOP_EXTRA ${CARPOLO_DESKTOP_DIR}/carpolo3d.rc)
  endif()
  add_executable(carpolo3d ${CARPOLO_SOURCES} ${CARPOLO_DESKTOP_EXTRA})
  if(WIN32)
    target_link_libraries(carpolo3d PRIVATE raylib)
    set_target_properties(carpolo3d PROPERTIES WIN32_EXECUTABLE TRUE)   # no console window
    if(MSVC)
      target_link_options(carpolo3d PRIVATE /ENTRY:mainCRTStartup)
    else()
      target_link_options(carpolo3d PRIVATE -static -static-libgcc)  # single .exe, no MinGW DLLs
    endif()
  else()
    target_link_libraries(carpolo3d PRIVATE raylib m)
    if(NOT APPLE)
      target_link_libraries(carpolo3d PRIVATE pthread dl)
    endif()
  endif()
  # run from game3d/ (dev): assets are found next to the exe, in ../assets or in the macOS bundle
  set_target_properties(carpolo3d PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build-bin)
endif()
