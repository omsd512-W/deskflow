# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

# DeskflowMicrophone.driver: the virtual microphone that shows another
# computer's microphone on this Mac. Not part of Deskflow.app: it is
# installed into /Library/Audio/Plug-Ins/HAL, which needs an administrator,
# and coreaudiod has to be restarted to load it.

include(FetchContent)
# libASPL's own CMake regenerates sources with Python; build its checked-in
# sources directly instead (SOURCE_SUBDIR keeps FetchContent from adding it).
FetchContent_Declare(libaspl
  URL https://github.com/gavv/libASPL/archive/633e0f70203edd87d320fc5a3cae901e1363aac5.tar.gz
  URL_HASH SHA256=8583fb549419eab14b5c636ee4eb1f0b1df74c534eaf2f990d8bb0fb5de6e01b
  SOURCE_SUBDIR do-not-add
)
FetchContent_MakeAvailable(libaspl)

file(GLOB aspl_sources CONFIGURE_DEPENDS "${libaspl_SOURCE_DIR}/src/*.cpp")
add_library(aspl STATIC ${aspl_sources})
target_include_directories(aspl SYSTEM PUBLIC "${libaspl_SOURCE_DIR}/include")
target_compile_options(aspl PRIVATE -w)
set_target_properties(aspl PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
target_link_libraries(aspl PUBLIC "-framework CoreFoundation" "-framework CoreAudio")

set(DESKFLOW_MIC_DELAY 480 CACHE STRING "Frames between the feed and the virtual microphone")

set(target DeskflowMicrophone)
configure_file(Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
add_library(${target} MODULE Driver.cpp)
target_compile_definitions(${target} PRIVATE DESKFLOW_MIC_DELAY=${DESKFLOW_MIC_DELAY})
target_link_libraries(${target} PRIVATE aspl)
set_target_properties(${target} PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  PREFIX ""
  SUFFIX ""
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
add_custom_command(TARGET ${target} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources"
  COMMAND ${CMAKE_COMMAND} -E copy "${libaspl_SOURCE_DIR}/LICENSE" "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/libASPL-LICENSE.txt"
)
