/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "audio/AudioTypes.h"

namespace deskflow::audio {

StreamFormat defaultFormat(StreamKind kind)
{
  StreamFormat format;
  format.kind = kind;
  switch (kind) {
  case StreamKind::System:
    format.channels = 2;
    format.frameSamples = 480;
    format.bitrate = 192000;
    break;
  case StreamKind::Microphone:
    // CELT like the system stream; 64 kbps is near transparent for one voice.
    format.channels = 1;
    format.frameSamples = 480;
    format.bitrate = 64000;
    break;
  }
  return format;
}

const char *toString(StreamKind kind)
{
  switch (kind) {
  case StreamKind::System:
    return "system";
  case StreamKind::Microphone:
    return "microphone";
  }
  return "unknown";
}

} // namespace deskflow::audio
