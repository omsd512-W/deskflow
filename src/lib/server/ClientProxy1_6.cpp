/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2015 - 2016 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/ClientProxy1_6.h"

#include "base/Log.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "server/Server.h"

#include <algorithm>
#include <memory>

namespace {

// Mouse moves share this connection, so a move written during a transfer waits
// for whatever is ahead of it. A chunk this size takes about 6 ms at 100 Mbps.
constexpr size_t kClipboardChunkSize = 64 * 1024;

// Keeps a transfer moving if the stream never reports that it flushed.
constexpr double kClipboardFlushTimeout = 0.05;

} // namespace

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string &name, deskflow::IStream *stream, Server *server, IEventQueue *events)
    : ClientProxy1_5(name, stream, server, events),
      m_events(events)
{
  m_events->addHandler(EventTypes::StreamOutputFlushed, getStream()->getEventTarget(), [this](const auto &) {
    if (m_clipboardFlushTimer != nullptr) {
      sendNextClipboardChunk();
    }
  });
}

ClientProxy1_6::~ClientProxy1_6()
{
  stopClipboardFlushTimer();
  m_events->removeHandler(EventTypes::StreamOutputFlushed, getStream()->getEventTarget());
}

void ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard *clipboard)
{
  // ignore if this clipboard is already clean
  if (m_clipboard[id].m_dirty) {
    // this clipboard is now clean
    m_clipboard[id].m_dirty = false;
    Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

    std::string data = m_clipboard[id].m_clipboard.marshall();

    // Windows and macOS have one clipboard and report it under both IDs, so the
    // selection is usually a copy of what this client already has.
    if (id == kClipboardSelection && !m_clipboard[kClipboardClipboard].m_dirty &&
        data == m_clipboard[kClipboardClipboard].m_clipboard.marshall()) {
      LOG_DEBUG(
          "not sending clipboard %d to \"%s\", it matches clipboard %d", id, getName().c_str(), kClipboardClipboard
      );
      return;
    }

    LOG_DEBUG("sending clipboard %d to \"%s\"", id, getName().c_str());

    // Only the newest copy needs to reach the client, so this replaces any older
    // transfer. The client drops a partial transfer when a new one starts.
    std::erase_if(m_clipboardTransfers, [id](const ClipboardTransfer &transfer) { return transfer.id == id; });
    m_clipboardTransfers.push_back({.id = id, .data = std::move(data)});

    if (m_clipboardFlushTimer == nullptr) {
      sendNextClipboardChunk();
    }
  }
}

void ClientProxy1_6::sendNextClipboardChunk()
{
  stopClipboardFlushTimer();

  if (m_clipboardTransfers.empty()) {
    return;
  }

  auto &transfer = m_clipboardTransfers.front();
  std::unique_ptr<ClipboardChunk> chunk;
  if (!transfer.started) {
    transfer.started = true;
    chunk.reset(ClipboardChunk::start(transfer.id, 0, std::to_string(transfer.data.size())));
  } else if (transfer.sent < transfer.data.size()) {
    const auto size = std::min(kClipboardChunkSize, transfer.data.size() - transfer.sent);
    chunk.reset(ClipboardChunk::data(transfer.id, 0, transfer.data.substr(transfer.sent, size)));
    transfer.sent += size;
  } else {
    chunk.reset(ClipboardChunk::end(transfer.id, 0));
    m_clipboardTransfers.pop_front();
  }

  ClipboardChunk::send(getStream(), chunk.get());

  // wait for this chunk to leave so that input written now is not stuck behind more of the clipboard
  if (!m_clipboardTransfers.empty()) {
    m_clipboardFlushTimer = m_events->newOneShotTimer(kClipboardFlushTimeout, nullptr);
    m_events->addHandler(EventTypes::Timer, m_clipboardFlushTimer, [this](const auto &) { sendNextClipboardChunk(); });
  }
}

void ClientProxy1_6::stopClipboardFlushTimer()
{
  if (m_clipboardFlushTimer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_clipboardFlushTimer);
    m_events->deleteTimer(m_clipboardFlushTimer);
    m_clipboardFlushTimer = nullptr;
  }
}

bool ClientProxy1_6::recvClipboard()
{
  // parse message
  ClipboardID id;
  uint32_t seq;

  auto r = ClipboardChunk::assemble(
      getStream(), m_clipboardDataCached, id, seq, m_clipboardChunkState, m_server->getMaximumClipboardSizeBytes()
  );

  if (r == TransferState::Started) {
    size_t size = ClipboardChunk::getExpectedSize(m_clipboardChunkState);
    LOG_DEBUG("receiving clipboard %d size=%zu", id, size);
  } else if (r == TransferState::Finished) {
    LOG(
        (CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%zu", getName().c_str(), id, seq,
         m_clipboardDataCached.size())
    );
    // save clipboard
    m_clipboard[id].m_clipboard.unmarshall(m_clipboardDataCached, 0);
    m_clipboard[id].m_sequenceNumber = seq;
    m_clipboardDataCached.clear();
    m_clipboardDataCached.shrink_to_fit();

    // notify
    auto *info = new ClipboardInfo;
    info->m_id = id;
    info->m_sequenceNumber = seq;
    m_events->addEvent(Event(EventTypes::ClipboardChanged, getEventTarget(), info));
  } else if (r == TransferState::Error) {
    return false;
  }

  return true;
}
