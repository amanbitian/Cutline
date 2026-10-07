#include "render/TextRaster.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace cutline::render::text {
namespace {

#ifdef _WIN32

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return {};
  const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
  if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
  return out;
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

struct Device final {
  HDC dc{nullptr};
  Device() : dc(CreateCompatibleDC(nullptr)) {}
  ~Device() {
    if (dc != nullptr) DeleteDC(dc);
  }
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
};

#endif

}  // namespace

bool Available() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

#ifdef _WIN32

Bitmap Rasterize(const std::string& utf8, const Style& style, int wrap_width) {
  Bitmap bitmap;
  const auto wide = Widen(utf8);
  if (wide.empty() || style.size < 1.0) return bitmap;
  Device device;
  if (device.dc == nullptr) return bitmap;

  const auto family = Widen(style.family.empty() ? "Arial" : style.family);
  const HFONT font = CreateFontW(-static_cast<int>(std::lround(style.size)), 0, 0, 0, style.bold ? FW_BOLD : FW_NORMAL, style.italic ? TRUE : FALSE,
                                 FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, family.c_str());
  if (font == nullptr) return bitmap;
  const auto old_font = SelectObject(device.dc, font);
  SetTextCharacterExtra(device.dc, static_cast<int>(std::lround(style.letter_spacing)));

  wchar_t face[LF_FACESIZE] = {};
  GetTextFaceW(device.dc, LF_FACESIZE, face);
  bitmap.resolved_family = Narrow(face);
  bitmap.substituted = Lower(bitmap.resolved_family) != Lower(style.family.empty() ? "Arial" : style.family);

  UINT format = DT_NOPREFIX | DT_EXPANDTABS;
  if (wrap_width > 0) format |= DT_WORDBREAK;
  switch (style.align) {
    case Align::Left: format |= DT_LEFT; break;
    case Align::Center: format |= DT_CENTER; break;
    case Align::Right: format |= DT_RIGHT; break;
  }
  // Measure first: the box the text needs at the width it is given.
  RECT box{0, 0, wrap_width > 0 ? wrap_width : 16384, 0};
  DrawTextW(device.dc, wide.c_str(), static_cast<int>(wide.size()), &box, format | DT_CALCRECT);
  // A little slack either side, so that drawing into a box exactly as wide as the text never re-wraps it.
  const auto width = std::max<int>(1, box.right - box.left + 6);
  const auto height = std::max<int>(1, box.bottom - box.top + 2);
  if (width > 16384 || height > 16384) {
    SelectObject(device.dc, old_font);
    DeleteObject(font);
    return bitmap;
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP dib = CreateDIBSection(device.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    SelectObject(device.dc, old_font);
    DeleteObject(font);
    return bitmap;
  }
  const auto old_bitmap = SelectObject(device.dc, dib);
  std::fill_n(static_cast<std::uint8_t*>(bits), static_cast<std::size_t>(width) * height * 4, std::uint8_t{0});
  SetBkMode(device.dc, TRANSPARENT);
  SetTextColor(device.dc, RGB(255, 255, 255));
  RECT draw{1, 1, width - 1, height - 1};  // aligned within the bitmap, which is as wide as the widest line
  DrawTextW(device.dc, wide.c_str(), static_cast<int>(wide.size()), &draw, format);
  GdiFlush();

  bitmap.width = width;
  bitmap.height = height;
  bitmap.coverage.resize(static_cast<std::size_t>(width) * height);
  const auto* pixels = static_cast<const std::uint8_t*>(bits);
  float total = 0.0f;
  for (std::size_t i = 0; i < bitmap.coverage.size(); ++i) {
    // White text on black: the grey level is the coverage.
    const auto level = static_cast<float>(std::max({pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]})) / 255.0f;
    bitmap.coverage[i] = level;
    total += level;
  }
  if (total <= 0.0f) {
    bitmap.coverage.clear();
    bitmap.width = bitmap.height = 0;
  }
  TEXTMETRICW metrics{};
  GetTextMetricsW(device.dc, &metrics);
  bitmap.lines = metrics.tmHeight > 0 ? std::max(1, static_cast<int>(std::lround(static_cast<double>(box.bottom - box.top) / (metrics.tmHeight + metrics.tmExternalLeading)))) : 1;

  SelectObject(device.dc, old_bitmap);
  SelectObject(device.dc, old_font);
  DeleteObject(dib);
  DeleteObject(font);
  return bitmap;
}

namespace {

int CALLBACK CollectFamily(const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM parameter) {
  auto* out = reinterpret_cast<std::set<std::string>*>(parameter);
  const std::string name = Narrow(font->lfFaceName);
  // Vertical variants ("@Name") are the same face rotated for vertical writing.
  if (!name.empty() && name[0] != '@') out->insert(name);
  return 1;
}

}  // namespace

std::vector<std::string> InstalledFamilies() {
  std::set<std::string> names;
  Device device;
  if (device.dc == nullptr) return {};
  LOGFONTW query{};
  query.lfCharSet = DEFAULT_CHARSET;
  EnumFontFamiliesExW(device.dc, &query, CollectFamily, reinterpret_cast<LPARAM>(&names), 0);
  return {names.begin(), names.end()};
}

bool HasFamily(const std::string& family) {
  const auto wanted = Lower(family);
  for (const auto& name : InstalledFamilies()) {
    if (Lower(name) == wanted) return true;
  }
  return false;
}

#else

Bitmap Rasterize(const std::string&, const Style&, int) { return {}; }
std::vector<std::string> InstalledFamilies() { return {}; }
bool HasFamily(const std::string&) { return false; }

#endif

}  // namespace cutline::render::text
