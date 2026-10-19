/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardBitmapConverter.h"

#include "base/Log.h"

#include <QtEndian>

#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
constexpr uint32_t kRedMask = 0x00ff0000;
constexpr uint32_t kGreenMask = 0x0000ff00;
constexpr uint32_t kBlueMask = 0x000000ff;
constexpr uint32_t kAlphaMask = 0xff000000;

uint8_t channel(uint32_t pixel, uint32_t mask)
{
  if (mask == 0) {
    return 0;
  }
  const auto shift = std::countr_zero(mask);
  const uint64_t max = mask >> shift;
  return static_cast<uint8_t>(static_cast<uint64_t>((pixel & mask) >> shift) * 255 / max);
}

// Most Windows applications only decode a CF_DIB that has a BITMAPINFOHEADER,
// bottom-up rows and BI_RGB pixels. macOS sends a V5 header, top-down rows and
// BI_BITFIELDS, so rewrite anything else into that form (32 bit, with alpha).
bool normaliseDib(const std::string &data, std::string &normalisedData)
{
  BITMAPINFOHEADER header;
  if (data.size() < sizeof(header)) {
    return false;
  }
  memcpy(&header, data.data(), sizeof(header));
  if (header.biSize < sizeof(header) || header.biWidth <= 0 || header.biHeight == 0 || header.biPlanes != 1 ||
      (header.biBitCount != 24 && header.biBitCount != 32) ||
      (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) {
    return false;
  }
  if (header.biSize == sizeof(header) && header.biHeight > 0 && header.biCompression == BI_RGB) {
    // already in canonical form
    return false;
  }

  const auto width = static_cast<size_t>(header.biWidth);
  const auto height = static_cast<size_t>(std::abs(static_cast<int64_t>(header.biHeight)));
  const auto bytesPerPixel = static_cast<size_t>(header.biBitCount / 8);
  const auto stride = (width * header.biBitCount + 31) / 32 * 4;
  if (height > std::numeric_limits<size_t>::max() / stride / 2) {
    return false;
  }

  uint32_t masks[] = {kRedMask, kGreenMask, kBlueMask, 0};
  size_t offset = header.biSize;
  if (header.biBitCount == 32 && header.biSize > sizeof(header) && data.size() == sizeof(header) + stride * height) {
    // macOS can describe an INFOHEADER-sized 32-bit pixel payload as a V5 DIB.
    // Windows then interprets the first pixels as V5 colour masks. The pixel
    // bytes are ordinary BGRA.
    offset = sizeof(header);
    masks[3] = kAlphaMask;
  } else {
    if (header.biSize > data.size()) {
      return false;
    }

    // colour masks follow an INFOHEADER, and are in the same place inside the
    // bigger headers, which also have an alpha mask
    if (header.biCompression == BI_BITFIELDS) {
      if (data.size() < sizeof(header) + 3 * sizeof(DWORD)) {
        return false;
      }
      for (size_t i = 0; i < 3; ++i) {
        masks[i] = qFromLittleEndian<quint32>(data.data() + sizeof(header) + i * sizeof(DWORD));
      }
      if (header.biSize == sizeof(header)) {
        offset += 3 * sizeof(DWORD);
      }
    }
    if (header.biBitCount == 32 && header.biSize >= sizeof(header) + 4 * sizeof(DWORD)) {
      masks[3] = qFromLittleEndian<quint32>(data.data() + sizeof(header) + 3 * sizeof(DWORD));
    }
    offset += static_cast<size_t>(header.biClrUsed) * sizeof(RGBQUAD);
  }
  if (offset > data.size() || stride * height > data.size() - offset) {
    return false;
  }

  BITMAPINFOHEADER normalisedHeader = header;
  normalisedHeader.biSize = sizeof(normalisedHeader);
  normalisedHeader.biHeight = static_cast<LONG>(height);
  normalisedHeader.biBitCount = 32;
  normalisedHeader.biCompression = BI_RGB;
  normalisedHeader.biSizeImage = static_cast<DWORD>(width * height * 4);
  normalisedHeader.biClrUsed = 0;
  normalisedHeader.biClrImportant = 0;
  normalisedData.assign(reinterpret_cast<const char *>(&normalisedHeader), sizeof(normalisedHeader));
  normalisedData.resize(sizeof(normalisedHeader) + width * height * 4);
  auto *pixels = reinterpret_cast<uint8_t *>(&normalisedData[sizeof(normalisedHeader)]);

  const bool bgra = header.biBitCount == 32 && masks[0] == kRedMask && masks[1] == kGreenMask &&
                    masks[2] == kBlueMask && (masks[3] == kAlphaMask || masks[3] == 0);
  for (size_t row = 0; row < height; ++row) {
    const auto *src = reinterpret_cast<const uint8_t *>(data.data()) + offset +
                      (header.biHeight < 0 ? height - 1 - row : row) * stride;
    auto *dst = pixels + row * width * 4;
    if (bgra) {
      memcpy(dst, src, width * 4);
      continue;
    }
    for (size_t x = 0; x < width; ++x, src += bytesPerPixel, dst += 4) {
      uint32_t pixel = src[0] | (src[1] << 8) | (src[2] << 16);
      if (bytesPerPixel == 4) {
        pixel |= static_cast<uint32_t>(src[3]) << 24;
      }
      dst[0] = channel(pixel, masks[2]);
      dst[1] = channel(pixel, masks[1]);
      dst[2] = channel(pixel, masks[0]);
      dst[3] = channel(pixel, masks[3]);
    }
  }

  // without any alpha the image would be fully transparent, so make it opaque
  const auto end = pixels + width * height * 4;
  bool hasAlpha = false;
  for (auto *alpha = pixels + 3; masks[3] != 0 && alpha < end && !hasAlpha; alpha += 4) {
    hasAlpha = *alpha != 0;
  }
  if (!hasAlpha) {
    for (auto *alpha = pixels + 3; alpha < end; alpha += 4) {
      *alpha = 0xff;
    }
  }

  LOG_INFO(
      "normalised clipboard image %zux%zu from header=%u depth=%d comp=%u%s", width, height,
      static_cast<unsigned>(header.biSize), header.biBitCount, static_cast<unsigned>(header.biCompression),
      header.biHeight < 0 ? " top-down" : ""
  );
  return true;
}
} // namespace

//
// MSWindowsClipboardBitmapConverter
//

IClipboard::Format MSWindowsClipboardBitmapConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

UINT MSWindowsClipboardBitmapConverter::getWin32Format() const
{
  return CF_DIB;
}

HANDLE
MSWindowsClipboardBitmapConverter::fromIClipboard(const std::string &data) const
{
  std::string normalisedData;
  const auto *clipboardData = &data;
  if (normaliseDib(data, normalisedData)) {
    clipboardData = &normalisedData;
  }

  // copy to memory handle
  HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, clipboardData->size());
  if (gData != nullptr) {
    // get a pointer to the allocated memory
    char *dst = (char *)GlobalLock(gData);
    if (dst != nullptr) {
      memcpy(dst, clipboardData->data(), clipboardData->size());
      GlobalUnlock(gData);
    } else {
      GlobalFree(gData);
      gData = nullptr;
    }
  }

  return gData;
}

std::string MSWindowsClipboardBitmapConverter::toIClipboard(HANDLE data) const
{
  // get datator
  LPVOID src = GlobalLock(data);
  if (src == nullptr) {
    return std::string();
  }
  uint32_t srcSize = (uint32_t)GlobalSize(data);

  // check image type
  const BITMAPINFO *bitmap = static_cast<const BITMAPINFO *>(src);
  LOG(
      (CLOG_INFO "bitmap: %dx%d %d", bitmap->bmiHeader.biWidth, bitmap->bmiHeader.biHeight,
       (int)bitmap->bmiHeader.biBitCount)
  );
  if (bitmap->bmiHeader.biPlanes == 1 && (bitmap->bmiHeader.biBitCount == 24 || bitmap->bmiHeader.biBitCount == 32) &&
      bitmap->bmiHeader.biCompression == BI_RGB) {
    // already in canonical form
    std::string image(static_cast<char const *>(src), srcSize);
    GlobalUnlock(data);
    return image;
  }

  // create a destination DIB section
  LOG_INFO("convert image from: depth=%d comp=%d", bitmap->bmiHeader.biBitCount, bitmap->bmiHeader.biCompression);
  void *raw;
  BITMAPINFOHEADER info;
  LONG w = bitmap->bmiHeader.biWidth;
  LONG h = bitmap->bmiHeader.biHeight;
  const LONG absHeight = (h < 0) ? -h : h;
  info.biSize = sizeof(BITMAPINFOHEADER);
  info.biWidth = w;
  info.biHeight = h;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  info.biSizeImage = 0;
  info.biXPelsPerMeter = 1000;
  info.biYPelsPerMeter = 1000;
  info.biClrUsed = 0;
  info.biClrImportant = 0;
  HDC dc = GetDC(nullptr);
  HBITMAP dst = CreateDIBSection(dc, (BITMAPINFO *)&info, DIB_RGB_COLORS, &raw, nullptr, 0);
  if (dst == nullptr || raw == nullptr) {
    LOG_WARN("failed to allocate destination bitmap for clipboard image");
    ReleaseDC(nullptr, dc);
    GlobalUnlock(data);
    return std::string();
  }

  // find the start of the pixel data
  const char *srcBits = (const char *)bitmap + bitmap->bmiHeader.biSize;
  if (bitmap->bmiHeader.biBitCount >= 16) {
    if (bitmap->bmiHeader.biCompression == BI_BITFIELDS &&
        (bitmap->bmiHeader.biBitCount == 16 || bitmap->bmiHeader.biBitCount == 32)) {
      srcBits += 3 * sizeof(DWORD);
    }
  } else if (bitmap->bmiHeader.biClrUsed != 0) {
    srcBits += bitmap->bmiHeader.biClrUsed * sizeof(RGBQUAD);
  } else {
    // http://msdn.microsoft.com/en-us/library/ke55d167(VS.80).aspx
    srcBits += (1i64 << bitmap->bmiHeader.biBitCount) * sizeof(RGBQUAD);
  }

  // copy source image to destination image
  HDC dstDC = CreateCompatibleDC(dc);
  HGDIOBJ oldBitmap = SelectObject(dstDC, dst);
  SetDIBitsToDevice(dstDC, 0, 0, w, absHeight, 0, 0, 0, absHeight, srcBits, bitmap, DIB_RGB_COLORS);
  SelectObject(dstDC, oldBitmap);
  DeleteDC(dstDC);
  GdiFlush();

  // extract data
  std::string image((const char *)&info, info.biSize);
  image.append((const char *)raw, static_cast<size_t>(4) * static_cast<size_t>(w) * static_cast<size_t>(absHeight));

  // clean up GDI
  DeleteObject(dst);
  ReleaseDC(nullptr, dc);

  // release handle
  GlobalUnlock(data);

  return image;
}
