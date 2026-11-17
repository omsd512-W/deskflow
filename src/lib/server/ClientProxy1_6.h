/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardChunk.h"
#include "server/ClientProxy1_5.h"

#include <cstddef>
#include <deque>
#include <string>

class EventQueueTimer;
class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5
{
public:
  ClientProxy1_6(const std::string &name, deskflow::IStream *adoptedStream, Server *server, IEventQueue *events);
  ~ClientProxy1_6() override;

  void setClipboard(ClipboardID id, const IClipboard *clipboard) override;
  bool recvClipboard() override;

private:
  //! A clipboard on its way to the client, sent a chunk at a time
  struct ClipboardTransfer
  {
    ClipboardID id = 0;
    std::string data;
    size_t sent = 0;
    bool started = false;
  };

  void sendNextClipboardChunk();
  void stopClipboardFlushTimer();

  IEventQueue *m_events;
  std::string m_clipboardDataCached;
  ClipboardChunkAssemblyState m_clipboardChunkState;
  std::deque<ClipboardTransfer> m_clipboardTransfers;
  EventQueueTimer *m_clipboardFlushTimer = nullptr;
};
