# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version)
endif()

foreach(
  name
  MediaPacketTests
  OpusCodecTests
  ReceiveStreamTests
  TlsChannelTests
  AudioPeerTests
  AudioSettingsSyncTests
  PlaybackMixerTests
)
  create_test(
    NAME ${name}
    DEPENDS audio
    LIBS base arch mt io ${extra_libs}
    SOURCE ${name}.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/audio"
  )
endforeach()
