#include "app/ScopeItem.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace cutline::app {
namespace {

float Maximum(const std::vector<float>& values) {
  return values.empty() ? 0.0f : *std::max_element(values.begin(), values.end());
}

int Alpha(float value, float maximum) {
  if (!(value > 0.0f) || !(maximum > 0.0f)) return 0;
  return std::clamp(static_cast<int>(std::sqrt(value / maximum) * 255.0f), 0, 255);
}

}  // namespace

ScopeItem::ScopeItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAntialiasing(true);
  setOpaquePainting(true);
}

void ScopeItem::setSession(Session* session) {
  if (session_ == session) return;
  if (session_ != nullptr) disconnect(session_, nullptr, this, nullptr);
  session_ = session;
  if (session_ != nullptr) connect(session_, &Session::scopeChanged, this, [this] { update(); });
  emit sessionChanged();
  update();
}

void ScopeItem::paint(QPainter* painter) {
  painter->fillRect(boundingRect(), QColor("#090a0a"));
  painter->setPen(QPen(QColor(70, 78, 76, 110), 1));
  for (int i = 1; i < 4; ++i) {
    const auto x = boundingRect().left() + boundingRect().width() * i / 4.0;
    const auto y = boundingRect().top() + boundingRect().height() * i / 4.0;
    painter->drawLine(QPointF(x, 0), QPointF(x, height()));
    painter->drawLine(QPointF(0, y), QPointF(width(), y));
  }
  if (session_ == nullptr || !session_->scopeResult()) {
    painter->setPen(QColor("#5c6462"));
    painter->drawText(boundingRect(), Qt::AlignCenter, tr("Waiting for a frame"));
    return;
  }
  const auto& result = *session_->scopeResult();
  const auto mode = session_->scopeMode();

  if ((mode == "waveform" || mode == "parade") && result.waveform) {
    const auto& scope = *result.waveform;
    QImage image(scope.width, scope.height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const auto maximum = Maximum(scope.density);
    const std::array<QColor, 3> colors{QColor(215, 255, 98), QColor(95, 225, 120), QColor(80, 145, 255)};
    for (int channel = 0; channel < scope.channels; ++channel) {
      const auto color = scope.channels == 1 ? QColor(215, 255, 98) : colors[static_cast<std::size_t>(channel)];
      for (int y = 0; y < scope.height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < scope.width; ++x) {
          const auto alpha = Alpha(scope.at(channel, y, x), maximum);
          if (alpha == 0) continue;
          const QColor before = QColor::fromRgba(row[x]);
          row[x] = qRgba(std::max(before.red(), color.red() * alpha / 255),
                         std::max(before.green(), color.green() * alpha / 255),
                         std::max(before.blue(), color.blue() * alpha / 255),
                         std::max(before.alpha(), alpha));
        }
      }
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(boundingRect(), image);
    return;
  }

  if (mode == "vectorscope" && result.vectorscope) {
    const auto& scope = *result.vectorscope;
    QImage image(scope.size, scope.size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const auto maximum = Maximum(scope.density);
    for (int y = 0; y < scope.size; ++y) {
      auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
      for (int x = 0; x < scope.size; ++x) {
        const auto alpha = Alpha(scope.at(y, x), maximum);
        row[x] = qRgba(120 * alpha / 255, 255 * alpha / 255, 190 * alpha / 255, alpha);
      }
    }
    const auto side = std::min(width(), height()) * 0.92;
    const QRectF target((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    painter->setPen(QPen(QColor(110, 120, 118, 150), 1));
    painter->drawEllipse(target);
    painter->drawLine(QPointF(target.center().x(), target.top()), QPointF(target.center().x(), target.bottom()));
    painter->drawLine(QPointF(target.left(), target.center().y()), QPointF(target.right(), target.center().y()));
    painter->drawImage(target, image);
    return;
  }

  if (mode == "histogram" && result.histogram) {
    const auto& scope = *result.histogram;
    const std::array<QColor, 4> colors{QColor(255, 90, 90), QColor(90, 255, 125), QColor(90, 140, 255), QColor(225, 225, 215)};
    float maximum = 0.0f;
    for (const auto& channel : scope.channels) maximum = std::max(maximum, Maximum(channel));
    if (!(maximum > 0.0f)) return;
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (int channel = 3; channel >= 0; --channel) {
      QPainterPath path;
      const auto& values = scope.channels[static_cast<std::size_t>(channel)];
      for (int x = 0; x < scope.bins; ++x) {
        const auto px = scope.bins > 1 ? width() * x / static_cast<double>(scope.bins - 1) : 0.0;
        const auto py = height() - height() * std::sqrt(values[static_cast<std::size_t>(x)] / maximum);
        if (x == 0) path.moveTo(px, py); else path.lineTo(px, py);
      }
      painter->setPen(QPen(colors[static_cast<std::size_t>(channel)], channel == 3 ? 1.0 : 1.4));
      painter->drawPath(path);
    }
  }
}

}  // namespace cutline::app
