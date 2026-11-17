/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsClipboardTests.h"

#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"

#include <QtEndian>

#include <initializer_list>

void MSWindowsClipboardTests::initTestCase()
{
  m_log.setFilter(LogLevel::Level::Verbose);

  MSWindowsClipboard clipboard(NULL);

  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
}

void MSWindowsClipboardTests::cleanupTestCase()
{
  initTestCase();
}

void MSWindowsClipboardTests::emptyUnusedClipboard()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.emptyUnowned());
}

void MSWindowsClipboardTests::emptyOpenCalled()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
}

void MSWindowsClipboardTests::emptySingleFormat()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));

  clipboard.add(IClipboard::Format::Text, m_testString);
  QVERIFY(clipboard.empty());
  QVERIFY(!clipboard.has(IClipboard::Format::Text));
}

void MSWindowsClipboardTests::addValue()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));

  clipboard.add(IClipboard::Format::Text, m_testString);
  QCOMPARE(clipboard.get(IClipboard::Format::Text), m_testString);
}

void MSWindowsClipboardTests::replaceValue()
{
  using enum IClipboard::Format;

  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));

  clipboard.add(Text, m_testString);
  clipboard.add(Text, m_testString2);

  QCOMPARE(clipboard.get(Text), m_testString2);
}

void MSWindowsClipboardTests::openTimeIsOne()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(1));
}

void MSWindowsClipboardTests::closeIsOpen()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(1));
  clipboard.close();
}

void MSWindowsClipboardTests::getTimeOpenWithNoEmpty()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(1));
  // this behavior is different to that of Clipboard which only
  // returns the value passed into open(t) after empty() is called.
  QCOMPARE(clipboard.getTime(), 1);
}

void MSWindowsClipboardTests::getTimeOpenAndEmpty()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(1));
  QVERIFY(clipboard.empty());
  QCOMPARE(clipboard.getTime(), 1);
}

void MSWindowsClipboardTests::has_withFormatAdded()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());

  clipboard.add(IClipboard::Format::Text, m_testString);
  QVERIFY(clipboard.has(IClipboard::Format::Text));
}

void MSWindowsClipboardTests::has_withNoFormatAdded()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  QCOMPARE(clipboard.get(IClipboard::Format::Text), "");
}

void MSWindowsClipboardTests::getNonEmptyText()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());

  clipboard.add(IClipboard::Format::Text, m_testString);
  QCOMPARE(clipboard.get(IClipboard::Format::Text), m_testString);
}

void MSWindowsClipboardTests::isOwnedByDeskflow()
{
  MSWindowsClipboard clipboard(NULL);
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.isOwnedByDeskflow());
}

void MSWindowsClipboardTests::normalisesMalformedMacBitmap()
{
  // A 1x1 top-down macOS DIB that incorrectly declares a V5 header.
  constexpr qsizetype headerSize = sizeof(BITMAPINFOHEADER);
  std::string dib(headerSize + 4, '\0');
  auto *raw = reinterpret_cast<quint8 *>(&dib[0]);
  qToLittleEndian<quint32>(sizeof(BITMAPV5HEADER), raw); // claims V5 but only has an INFOHEADER
  qToLittleEndian<quint32>(1, raw + 4);
  qToLittleEndian<quint32>(-1, raw + 8);
  qToLittleEndian<quint16>(1, raw + 12);
  qToLittleEndian<quint16>(32, raw + 14);
  qToLittleEndian<quint32>(BI_BITFIELDS, raw + 16);
  raw[headerSize + 3] = 0xff;

  MSWindowsClipboardBitmapConverter converter;
  const auto handle = converter.fromIClipboard(dib);
  QVERIFY(handle != nullptr);
  QCOMPARE(GlobalSize(handle), SIZE_T(headerSize + 4));
  const auto *result = static_cast<const quint8 *>(GlobalLock(handle));
  QVERIFY(result != nullptr);
  QCOMPARE(qFromLittleEndian<quint32>(result), quint32(headerSize));
  QCOMPARE(qFromLittleEndian<quint32>(result + 16), quint32(BI_RGB));
  QCOMPARE(result[headerSize + 3], quint8(0xff));
  GlobalUnlock(handle);
  GlobalFree(handle);
}

namespace {
std::string dibHeader(quint32 size, qint32 width, qint32 height, quint16 depth, quint32 compression)
{
  std::string dib(size, '\0');
  auto *raw = reinterpret_cast<quint8 *>(&dib[0]);
  qToLittleEndian<quint32>(size, raw);
  qToLittleEndian<qint32>(width, raw + 4);
  qToLittleEndian<qint32>(height, raw + 8);
  qToLittleEndian<quint16>(1, raw + 12);
  qToLittleEndian<quint16>(depth, raw + 14);
  qToLittleEndian<quint32>(compression, raw + 16);
  return dib;
}

void appendMasks(std::string &dib, std::initializer_list<quint32> masks, size_t at)
{
  for (const auto mask : masks) {
    if (dib.size() < at + 4) {
      dib.resize(at + 4, '\0');
    }
    qToLittleEndian<quint32>(mask, reinterpret_cast<quint8 *>(&dib[at]));
    at += 4;
  }
}

std::string fromIClipboard(const std::string &dib)
{
  MSWindowsClipboardBitmapConverter converter;
  const auto handle = converter.fromIClipboard(dib);
  if (handle == nullptr) {
    return {};
  }
  const auto *data = static_cast<const char *>(GlobalLock(handle));
  std::string result(data, GlobalSize(handle));
  GlobalUnlock(handle);
  GlobalFree(handle);
  return result;
}
} // namespace

void MSWindowsClipboardTests::normalisesMacV5Bitmap()
{
  // A complete 1x2 top-down macOS V5 DIB with BGRA colour masks, top pixel first.
  auto dib = dibHeader(sizeof(BITMAPV5HEADER), 1, -2, 32, BI_BITFIELDS);
  appendMasks(dib, {0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000}, 40);
  dib += std::string("\x01\x02\x03\x80\x11\x12\x13\x90", 8);

  const auto result = fromIClipboard(dib);

  // Most Windows applications only decode a CF_DIB with an INFOHEADER, bottom-up rows and BI_RGB.
  const auto *raw = reinterpret_cast<const quint8 *>(result.data());
  QCOMPARE(result.size(), sizeof(BITMAPINFOHEADER) + 8);
  QCOMPARE(qFromLittleEndian<quint32>(raw), quint32(sizeof(BITMAPINFOHEADER)));
  QCOMPARE(qFromLittleEndian<qint32>(raw + 8), qint32(2));
  QCOMPARE(qFromLittleEndian<quint16>(raw + 14), quint16(32));
  QCOMPARE(qFromLittleEndian<quint32>(raw + 16), quint32(BI_RGB));
  QCOMPARE(result.substr(sizeof(BITMAPINFOHEADER)), std::string("\x11\x12\x13\x90\x01\x02\x03\x80", 8));
}

void MSWindowsClipboardTests::normalisesOtherColourMasks()
{
  // A bottom-up INFOHEADER DIB with RGBX colour masks after the header and no alpha.
  auto dib = dibHeader(sizeof(BITMAPINFOHEADER), 1, 1, 32, BI_BITFIELDS);
  appendMasks(dib, {0x000000ff, 0x0000ff00, 0x00ff0000}, 40);
  dib += std::string("\x01\x02\x03\x00", 4);

  const auto result = fromIClipboard(dib);

  QCOMPARE(result.size(), sizeof(BITMAPINFOHEADER) + 4);
  QCOMPARE(qFromLittleEndian<quint32>(result.data() + 16), quint32(BI_RGB));
  QCOMPARE(result.substr(sizeof(BITMAPINFOHEADER)), std::string("\x03\x02\x01\xff", 4));
}

void MSWindowsClipboardTests::keepsCanonicalBitmap()
{
  auto dib = dibHeader(sizeof(BITMAPINFOHEADER), 1, 1, 32, BI_RGB);
  dib += std::string("\x01\x02\x03\x00", 4);

  const auto result = fromIClipboard(dib);

  QCOMPARE(result, dib);
}

QTEST_MAIN(MSWindowsClipboardTests)
