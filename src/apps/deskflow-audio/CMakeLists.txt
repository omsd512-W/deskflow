# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

set(target ${CMAKE_PROJECT_NAME}-audio)

# The audio helper: a background service started by the GUI (macOS) or the
# daemon (Windows), and a command line test tool for the audio link.
add_executable(${target}
  "${target}.cpp"
  AudioService.h
  AudioService.cpp
  TestHarness.h
  TestHarness.cpp
)

target_link_libraries(${target} audio net common base arch Qt6::Core Qt6::Network)

if(WIN32)
  set(EXE_DESCRIPTION "${CMAKE_PROJECT_PROPER_NAME} audio sharing helper")
  set(EXE_ICON "IDI_DESKFLOW ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/deskflow.ico\"")
  configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in ${target}.rc)
  target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc)
endif()

install(
  TARGETS ${target}
  RUNTIME_DEPENDENCY_SET audioDeps
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_OSX_BUNDLE)
  # Inside Deskflow.app, so that macOS asks for (and remembers) the permission
  # to record system audio on behalf of Deskflow, which starts it.
  set_target_properties(${target} PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_PROPER_NAME}>/MacOS
    XCODE_ATTRIBUTE_ENABLE_HARDENED_RUNTIME YES
  )
  if(APPLE_CODESIGN_DEV)
    configure_mac_codesign(${target})
  endif()
elseif(WIN32)
  install(RUNTIME_DEPENDENCY_SET audioDeps
    ${WIN32_RUNTIME_DEPENDENCY_SET_ARGS}
    PRE_EXCLUDE_REGEXES ${WIN32_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES ${WIN32_POST_EXCLUDE_REGEXES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()
