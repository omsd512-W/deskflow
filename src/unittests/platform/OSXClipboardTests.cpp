/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardTests.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardPNGConverter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <ImageIO/ImageIO.h>
#include <QtEndian>

namespace {

std::string makeDib(qint32 width, qint32 height, quint16 bpp, quint8 r, quint8 g, quint8 b, quint8 a = 0)
{
  const auto absHeight = static_cast<quint32>(height < 0 ? -height : height);
  const auto stride = ((static_cast<quint32>(width) * bpp + 31u) / 32u) * 4u;
  const auto pixelBytes = stride * absHeight;
  std::string dib(40 + pixelBytes, '\0');
  auto *hdr = reinterpret_cast<uchar *>(dib.data());

  qToLittleEndian<quint32>(40, hdr + 0);
  qToLittleEndian<qint32>(width, hdr + 4);
  qToLittleEndian<qint32>(height, hdr + 8);
  qToLittleEndian<quint16>(1, hdr + 12);
  qToLittleEndian<quint16>(bpp, hdr + 14);
  qToLittleEndian<quint32>(0, hdr + 16); // BI_RGB
  qToLittleEndian<quint32>(pixelBytes, hdr + 20);

  const auto bytesPerPixel = static_cast<quint32>(bpp / 8);
  for (quint32 y = 0; y < absHeight; ++y) {
    auto *row = hdr + 40 + y * stride;
    for (quint32 x = 0; x < static_cast<quint32>(width); ++x) {
      auto *px = row + x * bytesPerPixel;
      px[0] = b;
      px[1] = g;
      px[2] = r;
      if (bytesPerPixel == 4) {
        px[3] = a;
      }
    }
  }
  return dib;
}

bool decodePngSize(const std::string &png, size_t &width, size_t &height)
{
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(png.data()), png.size());
  if (data == nullptr) {
    return false;
  }
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (source == nullptr) {
    return false;
  }
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (image == nullptr) {
    return false;
  }
  width = CGImageGetWidth(image);
  height = CGImageGetHeight(image);
  CGImageRelease(image);
  return true;
}

} // namespace

void OSXClipboardTests::open()
{
  OSXClipboard clipboard;
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.close();
}

void OSXClipboardTests::singleFormat()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), m_testString);
}

void OSXClipboardTests::bitmapFormat()
{
  using enum IClipboard::Format;

  const std::string dib = makeDib(4, 3, 32, 10, 20, 30, 0);
  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Bitmap, dib);
  QVERIFY(clipboard.has(Bitmap));
  QCOMPARE(clipboard.get(Bitmap), dib);

  // Verify the pasteboard item explicitly advertises public.png alongside com.microsoft.bmp
  // so WebKit/Safari sees the pasted image on NSPasteboardItem.types.
  PasteboardRef pboard = nullptr;
  QCOMPARE(PasteboardCreate(kPasteboardClipboard, &pboard), noErr);
  PasteboardSynchronize(pboard);
  PasteboardItemID item = nullptr;
  QCOMPARE(PasteboardGetItemIdentifier(pboard, 1, &item), noErr);

  PasteboardFlavorFlags flags = 0;
  QCOMPARE(PasteboardGetItemFlavorFlags(pboard, item, CFSTR("com.microsoft.bmp"), &flags), noErr);
  QCOMPARE(PasteboardGetItemFlavorFlags(pboard, item, CFSTR("public.png"), &flags), noErr);

  CFDataRef pngData = nullptr;
  QCOMPARE(PasteboardCopyItemFlavorData(pboard, item, CFSTR("public.png"), &pngData), noErr);
  QVERIFY(pngData != nullptr);
  const std::string png(reinterpret_cast<const char *>(CFDataGetBytePtr(pngData)), CFDataGetLength(pngData));
  CFRelease(pngData);
  CFRelease(pboard);

  size_t width = 0;
  size_t height = 0;
  QVERIFY(decodePngSize(png, width, height));
  QCOMPARE(width, size_t{4});
  QCOMPARE(height, size_t{3});
}

void OSXClipboardTests::formatConvert_UTF8()
{
  OSXClipboardUTF8Converter converter;
  QCOMPARE(IClipboard::Format::Text, converter.getFormat());
  QCOMPARE(converter.getOSXFormat(), CFSTR("public.utf8-plain-text"));
  QCOMPARE(converter.fromIClipboard("test data\n"), "test data\r");
  QCOMPARE(converter.toIClipboard("test data\r"), "test data\n");
}

void OSXClipboardTests::formatConvert_PNG()
{
  OSXClipboardPNGConverter converter;
  QCOMPARE(converter.getFormat(), IClipboard::Format::Bitmap);
  QVERIFY(CFEqual(converter.getOSXFormat(), CFSTR("public.png")));

  // 32bpp bottom-up BI_RGB (alpha=0, typical Windows CF_DIB)
  const std::string png32 = converter.fromIClipboard(makeDib(8, 6, 32, 12, 34, 56, 0));
  QVERIFY(png32.size() > 8);
  QCOMPARE(png32.substr(0, 8), std::string("\x89PNG\r\n\x1a\n", 8));
  size_t width = 0;
  size_t height = 0;
  QVERIFY(decodePngSize(png32, width, height));
  QCOMPARE(width, size_t{8});
  QCOMPARE(height, size_t{6});

  // 32bpp top-down BI_RGB
  const std::string pngTopDown = converter.fromIClipboard(makeDib(5, -7, 32, 90, 80, 70, 0));
  QVERIFY(decodePngSize(pngTopDown, width, height));
  QCOMPARE(width, size_t{5});
  QCOMPARE(height, size_t{7});

  // 24bpp bottom-up BI_RGB
  const std::string png24 = converter.fromIClipboard(makeDib(3, 2, 24, 200, 100, 50));
  QVERIFY(decodePngSize(png24, width, height));
  QCOMPARE(width, size_t{3});
  QCOMPARE(height, size_t{2});

  // Roundtrip PNG -> DIB
  const std::string roundtripDib = converter.toIClipboard(png32);
  QVERIFY(roundtripDib.size() >= 40);
  QCOMPARE(qFromLittleEndian<qint32>(roundtripDib.data() + 4), 8);
  QCOMPARE(std::abs(qFromLittleEndian<qint32>(roundtripDib.data() + 8)), 6);

  // Invalid inputs return empty string rather than crashing or emitting garbage
  QVERIFY(converter.fromIClipboard("").empty());
  QVERIFY(converter.fromIClipboard("not-a-dib").empty());
  QVERIFY(converter.toIClipboard("").empty());
  QVERIFY(converter.toIClipboard("not-a-png").empty());
}

QTEST_MAIN(OSXClipboardTests)
