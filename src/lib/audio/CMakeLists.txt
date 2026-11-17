# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

# Opus: vcpkg ships a CMake package, Homebrew and Linux distros a pkg-config file.
find_package(Opus CONFIG QUIET)
if(TARGET Opus::opus)
  set(DESKFLOW_OPUS_TARGET Opus::opus)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
  set(DESKFLOW_OPUS_TARGET PkgConfig::OPUS)
endif()

# The speexdsp resampler, built standalone (see third_party/speex/README.deskflow).
add_library(speex_resampler STATIC
  third_party/speex/resample.c
  third_party/speex/speex_resampler.h
  third_party/speex/arch.h
)
target_compile_definitions(speex_resampler PUBLIC OUTSIDE_SPEEX RANDOM_PREFIX=deskflow_speex FLOATING_POINT)
target_include_directories(speex_resampler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/speex)
if(NOT MSVC)
  target_compile_options(speex_resampler PRIVATE -w)
endif()

# miniaudio: sound card input and output (see third_party/miniaudio/README.deskflow).
add_library(miniaudio STATIC MiniaudioImpl.c MiniaudioConfig.h)
target_include_directories(miniaudio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio)
if(NOT MSVC)
  target_compile_options(miniaudio PRIVATE -w)
endif()
if(APPLE)
  target_link_libraries(miniaudio PUBLIC "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(UNIX)
  target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} pthread m)
endif()

find_package(Qt6 ${REQUIRED_QT_VERSION} REQUIRED COMPONENTS Core Network)

add_library(audio STATIC
  AudioDevice.cpp
  AudioDevice.h
  AudioPeer.cpp
  AudioPeer.h
  AudioTypes.cpp
  AudioTypes.h
  ControlProtocol.cpp
  ControlProtocol.h
  MediaPacket.cpp
  MediaPacket.h
  OpusCodec.cpp
  OpusCodec.h
  ReceiveStream.cpp
  ReceiveStream.h
  SendStream.cpp
  SendStream.h
  SpscQueue.h
  TlsChannel.cpp
  TlsChannel.h
  UdpSocket.cpp
  UdpSocket.h
)

target_link_libraries(audio
  PUBLIC Qt6::Core Qt6::Network net common ${DESKFLOW_OPUS_TARGET}
  PRIVATE speex_resampler miniaudio
)

if(APPLE)
  set(audio_macos_sources
    macos/CoreAudioUtil.h
    macos/CoreAudioUtil.mm
    macos/SystemAudioCapture.h
    macos/SystemAudioCapture.mm
    macos/VirtualMicrophone.h
    macos/VirtualMicrophone.mm
  )
  target_sources(audio PRIVATE ${audio_macos_sources})
  set_source_files_properties(${audio_macos_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(audio PUBLIC "-framework Foundation" "-framework CoreAudio")
endif()

if(WIN32)
  target_link_libraries(audio PRIVATE ws2_32)
endif()
