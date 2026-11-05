/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ClientProxyTests.h"

#include "../deskflow/MockEventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/Clipboard.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/ClientProxy1_1.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QTest>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class CapturingStream : public deskflow::IStream
{
public:
  QByteArray take()
  {
    auto bytes = m_buffer;
    m_buffer.clear();
    return bytes;
  }

  void write(const void *buffer, uint32_t n) override
  {
    m_buffer.append(static_cast<const char *>(buffer), n);
  }

  void close() override
  {
  }

  uint32_t read(void *, uint32_t) override
  {
    return 0;
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<CapturingStream *>(this);
  }

  bool isReady() const override
  {
    return false;
  }

  uint32_t getSize() const override
  {
    return 0;
  }

  qsizetype getBufferedSize() const
  {
    return m_buffer.size();
  }

private:
  QByteArray m_buffer;
};

std::unique_ptr<ClientProxy> makeProxy(int minor, deskflow::IStream *stream, IEventQueue *events)
{
  // the 1.4 and later constructors assert the server pointer is non-null but only store it
  auto *server = reinterpret_cast<Server *>(0x1);

  std::unique_ptr<ClientProxy> proxy;
  switch (minor) {
  case 0:
    proxy = std::make_unique<ClientProxy1_0>("client", stream, events);
    break;
  case 1:
    proxy = std::make_unique<ClientProxy1_1>("client", stream, events);
    break;
  case 6:
    proxy = std::make_unique<ClientProxy1_6>("client", stream, server, events);
    break;
  case 7:
    proxy = std::make_unique<ClientProxy1_7>("client", stream, server, events);
    break;
  case 8:
    proxy = std::make_unique<ClientProxy1_8>("client", stream, server, events);
    break;
  default:
    break;
  }
  return proxy;
}

struct ProxyUnderTest
{
  MockEventQueue events;
  CapturingStream *stream = new CapturingStream;
  std::unique_ptr<ClientProxy> proxy;

  explicit ProxyUnderTest(int minor) : proxy(makeProxy(minor, stream, &events))
  {
    // drop the query-info and layout-sync messages the constructors send
    stream->take();
  }
};

class ClipboardEventQueue : public MockEventQueue
{
public:
  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase({type, target});
  }

  void fire(EventTypes type, void *target)
  {
    // copied because a handler can remove itself
    auto handler = m_handlers.at({type, target});
    handler(Event(type, target));
  }

  void fireTimer()
  {
    fire(EventTypes::Timer, &m_timerStorage);
  }

private:
  int m_timerStorage = 0;
  std::map<std::pair<EventTypes, void *>, EventHandler> m_handlers;
};

struct ClipboardMessage
{
  int id = 0;
  int mark = 0;
  qsizetype size = 0;
};

// "DCLP", 1 byte id, 4 byte sequence, 1 byte mark, 4 byte length, then the data
std::vector<ClipboardMessage> parseClipboardMessages(const QByteArray &bytes)
{
  const qsizetype headerSize = 14;
  std::vector<ClipboardMessage> messages;
  qsizetype position = 0;
  while (position < bytes.size()) {
    const auto header = reinterpret_cast<const uint8_t *>(bytes.constData() + position);
    const qsizetype size = header[10] << 24 | header[11] << 16 | header[12] << 8 | header[13];
    messages.push_back({.id = header[4], .mark = header[9], .size = size});
    position += headerSize + size;
  }
  return messages;
}

struct ClipboardProxyUnderTest
{
  ClipboardEventQueue events;
  CapturingStream *stream = new CapturingStream;
  std::unique_ptr<ClientProxy> proxy;

  ClipboardProxyUnderTest() : proxy(makeProxy(6, stream, &events))
  {
    stream->take();
  }

  void set(ClipboardID id, const std::string &text)
  {
    Clipboard clipboard;
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::Format::Text, text);
    clipboard.close();
    proxy->setClipboard(id, &clipboard);
  }

  void flush()
  {
    events.fire(EventTypes::StreamOutputFlushed, stream);
  }

  // flushes until nothing new is written, and returns everything written
  QByteArray finishTransfers()
  {
    auto bytes = stream->take();
    for (auto written = bytes.size(); written != 0; bytes += stream->take()) {
      flush();
      written = stream->getBufferedSize();
    }
    return bytes;
  }
};

const int kChunkStart = 1;
const int kChunkData = 2;
const int kChunkEnd = 3;
const size_t kExpectedChunkSize = 64 * 1024;

const KeyID kKey = 0x61;
const KeyModifierMask kMask = 0;
const KeyButton kButton = 0x1e;
const int32_t kCount = 3;
const std::string kLang = "en";

} // namespace

void ClientProxyTests::initTestCase()
{
  // the 1.8 constructor reads the keyboard layouts through AppUtil::instance()
  static TestAppUtil appUtil;
}

// These formats are frozen because shipped third-party clients parse them byte
// for byte: Synergy 1.4 through 1.14.1 negotiate 1.4 through 1.7, Barrier and
// Input Leap negotiate 1.6, and Synergy 1.14.2 onwards and Deskflow negotiate 1.8.
void ClientProxyTests::keyDown_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKDN" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKDN" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKDL" + QByteArray::fromHex("0061 0000 001e 00000002") + "en";
}

void ClientProxyTests::keyDown()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyDown(kKey, kMask, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyRepeat_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKRP" + QByteArray::fromHex("0061 0000 0003");
  QTest::newRow("1.1") << 1 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.6") << 6 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.7") << 7 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e");
  QTest::newRow("1.8") << 8 << "DKRP" + QByteArray::fromHex("0061 0000 0003 001e 00000002") + "en";
}

void ClientProxyTests::keyRepeat()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyRepeat(kKey, kMask, kCount, kButton, kLang);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::keyUp_data()
{
  QTest::addColumn<int>("minor");
  QTest::addColumn<QByteArray>("expected");

  QTest::newRow("1.0") << 0 << "DKUP" + QByteArray::fromHex("0061 0000");
  QTest::newRow("1.1") << 1 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.6") << 6 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.7") << 7 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
  QTest::newRow("1.8") << 8 << "DKUP" + QByteArray::fromHex("0061 0000 001e");
}

void ClientProxyTests::keyUp()
{
  QFETCH(int, minor);
  QFETCH(QByteArray, expected);

  ProxyUnderTest test(minor);
  test.proxy->keyUp(kKey, kMask, kButton);
  QCOMPARE(test.stream->take(), expected);
}

void ClientProxyTests::clipboardSentOneChunkAtATime()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, std::string(kExpectedChunkSize + 100, 'x'));

  auto messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkStart);

  test.flush();
  messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkData);
  QCOMPARE(messages[0].size, qsizetype(kExpectedChunkSize));

  test.flush();
  messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkData);
  QVERIFY(messages[0].size > 100 && messages[0].size < qsizetype(kExpectedChunkSize));

  test.flush();
  messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkEnd);

  test.flush();
  QVERIFY(test.stream->take().isEmpty());
}

void ClientProxyTests::clipboardKeepsMovingWithoutFlush()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, "text");
  test.stream->take();

  test.events.fireTimer();
  auto messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkData);

  test.events.fireTimer();
  messages = parseClipboardMessages(test.stream->take());
  QCOMPARE(messages.size(), size_t(1));
  QCOMPARE(messages[0].mark, kChunkEnd);
}

void ClientProxyTests::inputIsNotHeldBehindClipboard()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, std::string(kExpectedChunkSize * 3, 'x'));
  test.stream->take();

  test.proxy->mouseMove(1, 2);

  QCOMPARE(test.stream->take(), "DMMV" + QByteArray::fromHex("0001 0002"));
}

void ClientProxyTests::selectionMatchingClipboardIsNotSent()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, "same");
  test.set(kClipboardSelection, "same");

  const auto messages = parseClipboardMessages(test.finishTransfers());

  QCOMPARE(messages.size(), size_t(3));
  for (const auto &message : messages) {
    QCOMPARE(message.id, int(kClipboardClipboard));
  }
}

void ClientProxyTests::selectionDifferentFromClipboardIsSent()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, "clipboard");
  test.set(kClipboardSelection, "selection");

  const auto messages = parseClipboardMessages(test.finishTransfers());

  QCOMPARE(messages.size(), size_t(6));
  QCOMPARE(messages[0].id, int(kClipboardClipboard));
  QCOMPARE(messages[3].id, int(kClipboardSelection));
  QCOMPARE(messages[3].mark, kChunkStart);
}

void ClientProxyTests::onlyNewestQueuedClipboardIsSent()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, "clipboard");
  test.set(kClipboardSelection, "older");
  test.proxy->setClipboardDirty(kClipboardSelection, true);
  test.set(kClipboardSelection, "newest");

  const auto messages = parseClipboardMessages(test.finishTransfers());

  QCOMPARE(messages.size(), size_t(6));
  QCOMPARE(messages[4].mark, kChunkData);
  const qsizetype marshallOverhead = 12; // format count, format id and size
  QCOMPARE(messages[4].size, qsizetype(std::string("newest").size()) + marshallOverhead);
}

void ClientProxyTests::newerClipboardReplacesTransferInProgress()
{
  ClipboardProxyUnderTest test;
  test.set(kClipboardClipboard, std::string(kExpectedChunkSize * 3, 'x'));
  test.flush();
  test.proxy->setClipboardDirty(kClipboardClipboard, true);
  test.set(kClipboardClipboard, "newest");

  const auto messages = parseClipboardMessages(test.finishTransfers());

  QCOMPARE(messages.size(), size_t(5));
  QCOMPARE(messages[0].mark, kChunkStart);
  QCOMPARE(messages[1].mark, kChunkData);
  QCOMPARE(messages[2].mark, kChunkStart);
  QCOMPARE(messages[3].mark, kChunkData);
  QCOMPARE(messages[4].mark, kChunkEnd);
}

QTEST_MAIN(ClientProxyTests)
