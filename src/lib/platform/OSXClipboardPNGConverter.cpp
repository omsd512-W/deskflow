/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboardPNGConverter.h"

#include "base/Log.h"
#include "platform/OSXClipboardBMPConverter.h"

#include <ImageIO/ImageIO.h>

namespace {

template <typename T>
class ScopedCFRef
{
public:
  explicit ScopedCFRef(T ref = nullptr) : m_ref(ref)
  {
  }
  ~ScopedCFRef()
  {
    if (m_ref != nullptr) {
      CFRelease(m_ref);
    }
  }
  ScopedCFRef(const ScopedCFRef &) = delete;
  ScopedCFRef &operator=(const ScopedCFRef &) = delete;

  T get() const
  {
    return m_ref;
  }
  explicit operator bool() const
  {
    return m_ref != nullptr;
  }

private:
  T m_ref;
};

std::string transcodeImage(const std::string &input, CFStringRef sourceType, CFStringRef destinationType)
{
  if (input.empty()) {
    return {};
  }

  ScopedCFRef<CFDataRef> sourceData(CFDataCreateWithBytesNoCopy(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(input.data()), static_cast<CFIndex>(input.size()),
      kCFAllocatorNull
  ));
  if (!sourceData) {
    return {};
  }

  const void *keys[] = {kCGImageSourceTypeIdentifierHint};
  const void *values[] = {sourceType};
  ScopedCFRef<CFDictionaryRef> options(CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
  ));

  ScopedCFRef<CGImageSourceRef> source(CGImageSourceCreateWithData(sourceData.get(), options.get()));
  if (!source) {
    return {};
  }

  ScopedCFRef<CGImageRef> image(CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr));
  if (!image) {
    return {};
  }

  ScopedCFRef<CFMutableDataRef> output(CFDataCreateMutable(kCFAllocatorDefault, 0));
  if (!output) {
    return {};
  }

  ScopedCFRef<CGImageDestinationRef> destination(
      CGImageDestinationCreateWithData(output.get(), destinationType, 1, nullptr)
  );
  if (!destination) {
    return {};
  }

  CGImageDestinationAddImage(destination.get(), image.get(), nullptr);
  if (!CGImageDestinationFinalize(destination.get())) {
    return {};
  }

  const auto *bytes = reinterpret_cast<const char *>(CFDataGetBytePtr(output.get()));
  const auto length = static_cast<size_t>(CFDataGetLength(output.get()));
  if (bytes == nullptr || length == 0) {
    return {};
  }

  return std::string(bytes, length);
}

} // namespace

IClipboard::Format OSXClipboardPNGConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

CFStringRef OSXClipboardPNGConverter::getOSXFormat() const
{
  return CFSTR("public.png");
}

std::string OSXClipboardPNGConverter::fromIClipboard(const std::string &data) const
{
  OSXClipboardBMPConverter bmpConverter;
  const std::string bmp = bmpConverter.fromIClipboard(data);
  if (bmp.empty()) {
    return {};
  }

  std::string png = transcodeImage(bmp, bmpConverter.getOSXFormat(), getOSXFormat());
  if (png.empty()) {
    LOG_DEBUG("failed to convert clipboard bitmap to png, size: %zu bytes", data.size());
  }
  return png;
}

std::string OSXClipboardPNGConverter::toIClipboard(const std::string &data) const
{
  if (data.empty()) {
    return {};
  }

  OSXClipboardBMPConverter bmpConverter;
  const std::string bmp = transcodeImage(data, getOSXFormat(), bmpConverter.getOSXFormat());
  if (bmp.empty()) {
    LOG_DEBUG("failed to convert clipboard png to bitmap, size: %zu bytes", data.size());
    return {};
  }

  return bmpConverter.toIClipboard(bmp);
}
