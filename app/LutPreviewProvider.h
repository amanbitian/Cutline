#pragma once

// Pictures for the look-up table browser: the test chart seen through a LUT, as image://lutpreview/<url-encoded path>.
// The chart is drawn once; a result is kept until the file changes, so scrolling a list of looks costs nothing after the
// first view of each.

#include "media/VideoFrame.h"
#include "ui/LutLibrary.h"

#include <QFileInfo>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QUrl>

#include <map>
#include <string>

namespace cutline::app {

class LutPreviewProvider final : public QQuickImageProvider {
 public:
  LutPreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

  QImage requestImage(const QString& id, QSize* size, const QSize&) override {
    constexpr int kWidth = 192, kHeight = 108;
    const auto path = QUrl::fromPercentEncoding(id.toUtf8());
    const QFileInfo info(path);
    const auto key = path + "|" + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
      const QMutexLocker lock(&mutex_);
      if (const auto found = cache_.find(key.toStdString()); found != cache_.end()) {
        if (size != nullptr) *size = found->second.size();
        return found->second;
      }
    }
    QImage image;
    try {
      const auto chart = ui::TestChart(kWidth, kHeight);
      const auto graded = path == "original" ? chart.Clone() : ui::ApplyLutTo(chart, std::filesystem::path(path.toStdString()));
      const auto rgba = media::ConvertFrame(graded, media::PixelFormat::Rgba8);
      image = QImage(rgba.width(), rgba.height(), QImage::Format_RGBA8888);
      for (int y = 0; y < rgba.height(); ++y) std::memcpy(image.scanLine(y), rgba.row_u8(y), static_cast<std::size_t>(rgba.width()) * 4);
    } catch (const std::exception&) {
      image = QImage(kWidth, kHeight, QImage::Format_RGBA8888);
      image.fill(QColor(60, 30, 30));   // a table that cannot be read shows as a dull red block
    }
    {
      const QMutexLocker lock(&mutex_);
      if (cache_.size() > 200) cache_.clear();
      cache_[key.toStdString()] = image;
    }
    if (size != nullptr) *size = image.size();
    return image;
  }

 private:
  QMutex mutex_;
  std::map<std::string, QImage> cache_;
};

}  // namespace cutline::app
