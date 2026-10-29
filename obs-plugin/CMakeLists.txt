# SPDX-License-Identifier: GPL-2.0-or-later
# VocalDress の声（OBS の音声ソース、GPL-2.0-or-later、このフォルダの LICENSE）。docs/design/OBS_PLUGIN_RELEASE.md
# 製品の repo の中では、一番上の CMake で KV_BUILD_OBS_PLUGIN=ON のときに読む。公開用に書き出したもの
# （tools/export_obs_plugin.sh）でも、このフォルダを直接 cmake -S obs-plugin で読む。
#   Linux：Ubuntu の libobs-dev（find_package(libobs)）。届くかの簡単な試し vocaldress_voice_smoke も作る
#   Windows：OBS_SOURCE_DIR（obs-studio のソース。対象の一番古い版、案では 32.0.0）と
#            OBS_IMPORT_LIB（obs.dll から作る obs.lib、README.md）
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(vocaldress_voice LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  enable_testing()
endif()
find_package(Threads REQUIRED)

set(VOCALDRESS_VOICE_VERSION "0.1.0" CACHE STRING "プラグインの版（アプリと同じにする）")
set(VOCALDRESS_VOICE_LINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shared/voice-link" CACHE PATH "受け渡しの約束（MIT）")
set(OBS_SOURCE_DIR "" CACHE PATH "Windows：obs-studio のソース")
set(OBS_IMPORT_LIB "" CACHE FILEPATH "Windows：obs.dll から作った obs.lib")

set(VD_OBS_TARGET "")
if(OBS_SOURCE_DIR AND OBS_IMPORT_LIB)
  add_library(vd_obs_libobs SHARED IMPORTED)
  # obs-config.h が読む obsconfig.h は OBS のビルドで作られるので、プラグインに要る分だけをここで作る
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obsconfig/obsconfig.h"
    "#pragma once\n#define OBS_DATA_PATH \"data\"\n#define OBS_PLUGIN_PATH \"obs-plugins\"\n"
    "#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
  set_target_properties(vd_obs_libobs PROPERTIES IMPORTED_IMPLIB "${OBS_IMPORT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${CMAKE_CURRENT_BINARY_DIR}/obsconfig")
  set(VD_OBS_TARGET vd_obs_libobs)
else()
  find_package(libobs CONFIG QUIET)
  if(TARGET OBS::libobs)
    set(VD_OBS_TARGET OBS::libobs)
  endif()
endif()

if(NOT VD_OBS_TARGET)
  message(STATUS "vocaldress-voice：libobs が見つからないので、OBS のプラグインはビルドしません")
  return()
endif()

add_library(vocaldress-voice MODULE vocaldress-voice.cpp)
target_include_directories(vocaldress-voice PRIVATE "${VOCALDRESS_VOICE_LINK_DIR}")
target_compile_definitions(vocaldress-voice PRIVATE VOCALDRESS_VOICE_VERSION="${VOCALDRESS_VOICE_VERSION}")
target_link_libraries(vocaldress-voice PRIVATE ${VD_OBS_TARGET} Threads::Threads)
set_target_properties(vocaldress-voice PROPERTIES PREFIX "")
if(MSVC)
  target_compile_options(vocaldress-voice PRIVATE /utf-8 /W4 /permissive-)
  # 利用者の PC に Visual C++ ランタイムが無くても動くよう静的リンクする
  set_property(TARGET vocaldress-voice PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_link_libraries(vocaldress-voice PRIVATE winmm)
else()
  target_compile_options(vocaldress-voice PRIVATE -Wall -Wextra)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(vocaldress-voice PRIVATE rt)
  endif()
endif()
message(STATUS "vocaldress-voice：OBS のプラグインをビルドします（${VD_OBS_TARGET}、版 ${VOCALDRESS_VOICE_VERSION}）")

# Linux の libobs で、プラグインを読み込んで、書いた声が届くかを見る試し。Windows の OBS の代わりにはならない
if(TARGET OBS::libobs)
  add_executable(vocaldress_voice_smoke tests/smoke.cpp)
  target_include_directories(vocaldress_voice_smoke PRIVATE "${VOCALDRESS_VOICE_LINK_DIR}")
  target_link_libraries(vocaldress_voice_smoke PRIVATE OBS::libobs Threads::Threads)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(vocaldress_voice_smoke PRIVATE rt)
  endif()
  # libobs の obs_startup は X の画面を要るので、xvfb-run があれば仮の画面の中で動かす
  find_program(VD_XVFB_RUN xvfb-run)
  if(VD_XVFB_RUN)
    add_test(NAME vocaldress_voice_smoke COMMAND ${VD_XVFB_RUN} -a $<TARGET_FILE:vocaldress_voice_smoke> $<TARGET_FILE:vocaldress-voice>)
  else()
    add_test(NAME vocaldress_voice_smoke COMMAND vocaldress_voice_smoke $<TARGET_FILE:vocaldress-voice>)
  endif()
endif()
