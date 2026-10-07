#include "app/MulticamItem.h"

#include "render/AngleMonitor.h"

#include <QFont>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace cutline::app {

namespace {
constexpr int kGridWidth = 960;   // what the session renders the tiled frame at
constexpr int kGridHeight = 540;
}  // namespace

MulticamItem::MulticamItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton);
  setAntialiasing(true);
  setOpaquePainting(true);
}

void MulticamItem::setSession(Session* session) {
  if (session_ == session) return;
  if (session_ != nullptr) disconnect(session_, nullptr, this, nullptr);
  session_ = session;
  if (session_ != nullptr) {
    connect(session_, &Session::multicamFrameReady, this, [this] { update(); });
    connect(session_, &Session::multicamPositionChanged, this, [this] { update(); });
    connect(session_, &Session::multicamChanged, this, [this] { update(); });
  }
  emit sessionChanged();
  update();
}

QRectF MulticamItem::PictureRect() const {
  const double scale = std::min(width() / kGridWidth, height() / kGridHeight);
  const double w = kGridWidth * scale, h = kGridHeight * scale;
  return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

void MulticamItem::paint(QPainter* painter) {
  painter->fillRect(boundingRect(), QColor("#0c0d0d"));
  if (session_ == nullptr || session_->multicamGroup().isEmpty()) {
    painter->setPen(QColor("#5c6462"));
    painter->drawText(boundingRect(), Qt::AlignCenter, tr("No multicam group open"));
    return;
  }
  const auto rect = PictureRect();
  const auto frame = session_->multicamFrame();
  if (!frame.isNull()) painter->drawImage(rect, frame);
  // Names and keys over the tiles, in the layout the frame was composed with.
  const auto angles = session_->multicamAngles();
  render::AngleMonitorConfig config;
  config.width = kGridWidth;
  config.height = kGridHeight;
  std::vector<render::TileRect> tiles;
  try {
    tiles = render::AngleMonitorLayout(static_cast<std::size_t>(angles.size()), config);
  } catch (const std::exception&) {
    return;
  }
  const double scale = rect.width() / kGridWidth;
  QFont font("Segoe UI", 9);
  font.setBold(true);
  painter->setFont(font);
  for (int i = 0; i < angles.size() && i < static_cast<int>(tiles.size()); ++i) {
    const auto angle = angles[i].toMap();
    const auto& tile = tiles[static_cast<std::size_t>(i)];
    const QRectF r(rect.left() + tile.x * scale, rect.top() + tile.y * scale, tile.width * scale, tile.height * scale);
    const QString key = angle["key"].toString();
    const QString label = key.isEmpty() ? angle["name"].toString() : key + "  " + angle["name"].toString();
    const QRectF strip(r.left() + 6, r.top() + 6, std::min(r.width() - 12, QFontMetricsF(font).horizontalAdvance(label) + 14), 18);
    painter->fillRect(strip, QColor(0, 0, 0, 170));
    painter->setPen(angle["active"].toBool() ? QColor("#ff6b6b") : QColor("#f4f5f1"));
    painter->drawText(strip, Qt::AlignCenter, label);
    if (angle["active"].toBool()) {
      painter->setPen(QColor("#ff6b6b"));
      painter->drawText(QRectF(r.right() - 60, r.top() + 6, 54, 18), Qt::AlignRight | Qt::AlignVCenter, tr("LIVE"));
    }
    if (!angle["present"].toBool()) {
      painter->setPen(QColor("#8a9290"));
      painter->drawText(r, Qt::AlignCenter, tr("no picture here"));
    }
  }
}

void MulticamItem::mousePressEvent(QMouseEvent* event) {
  if (session_ == nullptr) return;
  const auto rect = PictureRect();
  if (!rect.contains(event->position())) return;
  (void)session_->multicamCutAtPoint(event->position().x() - rect.left(), event->position().y() - rect.top(), rect.width(), rect.height());
}

}  // namespace cutline::app
