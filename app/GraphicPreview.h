#pragma once

// Pictures of graphics for the interface: the designer's canvas, the library's thumbnails and the template browser, as
// image://graphicpreview/<key>@<width>. The session puts a document (its JSON) in the store under a key and the QML asks
// for the picture; a key that names a different document is a different picture, so the thumbnail of a graphic that was
// edited is asked for again by name. The document is drawn by the same code as the monitor and the export
// (render/Graphics.h) over a chequerboard, at the moment its entrance has finished.

#include "effects/GraphicsDocument.h"
#include "render/Graphics.h"

#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>

namespace cutline::app {

class GraphicPreviewStore final {
 public:
  [[nodiscard]] static GraphicPreviewStore& Instance() {
    static GraphicPreviewStore store;
    return store;
  }

  // The newest few hundred are kept; asking for one that has gone gives a blank picture.
  void Put(const QString& key, const std::string& document_json) {
    const QMutexLocker lock(&mutex_);
    if (documents_.find(key) == documents_.end()) {
      order_.push_back(key);
      while (order_.size() > 400) {
        documents_.erase(order_.front());
        order_.pop_front();
      }
    }
    documents_[key] = document_json;
  }

  [[nodiscard]] QImage Render(const QString& key, int width) {
    width = std::clamp(width, 16, 2048);
    std::string json;
    {
      const QMutexLocker lock(&mutex_);
      const auto found = documents_.find(key);
      if (found != documents_.end()) json = found->second;
    }
    render::graphics::Document document;
    try {
      document = render::graphics::ParseDocument(json);
    } catch (const std::exception&) {
      QImage blank(width, width * 9 / 16, QImage::Format_RGB32);
      blank.fill(QColor(60, 30, 30));
      return blank;
    }
    const auto height = std::max(1, static_cast<int>(std::lround(static_cast<double>(width) * document.height / document.width)));
    render::Layer canvas;
    canvas.Reset(width, height);
    // Where its entrances have finished, so a title that slides in is seen where it comes to rest.
    double settled = 0.0;
    for (const auto& element : document.elements) {
      for (const auto& animation : element.animations) {
        if (!animation.keys.empty()) settled = std::max(settled, animation.keys.back().time);
      }
    }
    {
      const QMutexLocker lock(&draw_mutex_);
      (void)render::graphics::Draw(canvas, document, stills_.Resolver(), settled);
    }
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
      auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
      for (int x = 0; x < width; ++x) {
        const bool light = ((x / 12) + (y / 12)) % 2 == 0;
        const float base = light ? 0.30f : 0.22f;
        const auto& pixel = canvas.at(x, y);
        const auto keep = 1.0f - pixel.a;
        const auto to_byte = [](float v) { return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255); };
        row[x] = qRgb(to_byte(pixel.r + base * keep), to_byte(pixel.g + base * keep), to_byte(pixel.b + base * keep));
      }
    }
    return image;
  }

 private:
  QMutex mutex_;
  QMutex draw_mutex_;
  std::map<QString, std::string> documents_;
  std::deque<QString> order_;
  render::graphics::StillCache stills_;
};

class GraphicPreviewProvider final : public QQuickImageProvider {
 public:
  GraphicPreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize& requested) override {
    const auto at = id.lastIndexOf('@');
    const auto key = at < 0 ? id : id.left(at);
    auto width = at < 0 ? 0 : id.mid(at + 1).toInt();
    if (width <= 0) width = requested.width() > 0 ? requested.width() : 320;
    const auto image = GraphicPreviewStore::Instance().Render(key, width);
    if (size != nullptr) *size = image.size();
    return image;
  }
};

}  // namespace cutline::app
