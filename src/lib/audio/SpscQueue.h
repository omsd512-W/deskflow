/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace deskflow::audio {

//! Fixed-capacity queue for exactly one producer thread and one consumer
//! thread. Neither side ever blocks or allocates, so the consumer can be a
//! real-time audio callback.
template <typename T, std::size_t Capacity> class SpscQueue
{
  static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

public:
  //! Producer: copy item in. Returns false when full.
  bool push(const T &item)
  {
    const auto head = m_head.load(std::memory_order_relaxed);
    const auto tail = m_tail.load(std::memory_order_acquire);
    if (head - tail == Capacity) {
      return false;
    }
    m_items[head & (Capacity - 1)] = item;
    m_head.store(head + 1, std::memory_order_release);
    return true;
  }

  //! Consumer: peek at the oldest item, or nullptr when empty.
  T *front()
  {
    const auto tail = m_tail.load(std::memory_order_relaxed);
    if (tail == m_head.load(std::memory_order_acquire)) {
      return nullptr;
    }
    return &m_items[tail & (Capacity - 1)];
  }

  //! Consumer: drop the item returned by front().
  void pop()
  {
    m_tail.store(m_tail.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  }

private:
  std::array<T, Capacity> m_items{};
  std::atomic<std::size_t> m_head{0};
  std::atomic<std::size_t> m_tail{0};
};

} // namespace deskflow::audio
