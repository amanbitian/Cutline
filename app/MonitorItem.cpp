#include "app/MonitorItem.h"

#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QtQuick/qsgtexture_platform.h>
#include <QWheelEvent>

namespace cutline::app {

namespace {

class LeasedTextureNode final : public QSGSimpleTextureNode {
 public:
  std::shared_ptr<void> lease;
};

}  // namespace

class MonitorVideoItem final : public QQuickItem {
 public:
  explicit MonitorVideoItem(MonitorItem* owner) : QQuickItem(owner), owner_(owner) {
    setFlag(ItemHasContents, true);
    setZ(-1.0);
  }

 protected:
  QSGNode* updatePaintNode(QSGNode* old_node, UpdatePaintNodeData*) override {
    delete old_node;
    auto* root = new QSGNode;
    root->appendChildNode(new QSGSimpleRectNode(boundingRect(), QColor("#0b0c0c")));
    if (owner_->session_ == nullptr || window() == nullptr) return root;

    const auto target = owner_->PictureRect();
    const auto background = QString::fromStdString(owner_->session_->prefs().GetText("monitor.background"));
    if (background == "checker") {
      QImage checker(24, 24, QImage::Format_RGBA8888_Premultiplied);
      checker.fill(QColor("#1d1f1f"));
      QPainter painter(&checker);
      painter.fillRect(0, 0, 12, 12, QColor("#2a2c2c"));
      painter.fillRect(12, 12, 12, 12, QColor("#2a2c2c"));
      auto* checker_texture = window()->createTextureFromImage(checker, QQuickWindow::TextureHasAlphaChannel);
      checker_texture->setHorizontalWrapMode(QSGTexture::Repeat);
      checker_texture->setVerticalWrapMode(QSGTexture::Repeat);
      auto* checker_node = new QSGSimpleTextureNode;
      checker_node->setOwnsTexture(true);
      checker_node->setTexture(checker_texture);
      checker_node->setRect(target);
      checker_node->setSourceRect(QRectF(0, 0, target.width(), target.height()));
      root->appendChildNode(checker_node);
    } else {
      root->appendChildNode(new QSGSimpleRectNode(target, background == "grey" ? QColor("#777777") : QColor("#000000")));
    }

    QSGTexture* texture = nullptr;
    std::shared_ptr<void> lease;
    const auto gpu = owner_->session_->currentGpuFrame();
    auto* renderer = window()->rendererInterface();
    if (gpu.valid() && renderer != nullptr && renderer->graphicsApi() == QSGRendererInterface::Direct3D11 &&
        renderer->getResource(window(), QSGRendererInterface::DeviceResource) == gpu.device) {
      texture = QNativeInterface::QSGD3D11Texture::fromNative(
          gpu.texture, window(), QSize(gpu.width, gpu.height), QQuickWindow::TextureHasAlphaChannel);
      lease = gpu.lifetime;
    } else {
      const auto image = owner_->session_->currentFrame();
      if (!image.isNull()) texture = window()->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
    }
    if (texture != nullptr) {
      auto* node = new LeasedTextureNode;
      node->setOwnsTexture(true);
      node->setTexture(texture);
      node->setRect(target);
      node->setFiltering(QSGTexture::Linear);
      node->lease = std::move(lease);
      root->appendChildNode(node);
    }
    return root;
  }

 private:
  MonitorItem* owner_;
};

MonitorItem::MonitorItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
  setAntialiasing(true);
  setOpaquePainting(false);
  setFillColor(Qt::transparent);
  video_item_ = new MonitorVideoItem(this);
  connect(this, &QQuickItem::windowChanged, this, &MonitorItem::AttachWindow);
}

void MonitorItem::setSession(Session* session) {
  if (session_ == session) return;
  if (session_ != nullptr) disconnect(session_, nullptr, this, nullptr);
  session_ = session;
  announced_device_ = nullptr;
  if (session_ != nullptr) {
    connect(session_, &Session::frameReady, this, [this] {
      video_item_->update();
      update();
    });
    connect(session_, &Session::playheadChanged, this, [this] { update(); });
    connect(session_, &Session::maskChanged, this, [this] { update(); });
    connect(session_, &Session::sequenceChanged, this, [this] { update(); });
    connect(session_, &Session::monitorChanged, this, [this] {
      UpdateDisplay();
      update();
    });
    connect(session_, &Session::monitorAction, this, [this](const QString& action) {
      if (action == "zoom_fit") setZoomMode("fit");
      else if (action == "zoom_100") setZoomMode("actual");
    });
  }
  UpdateDisplay();
  video_item_->update();
  if (window() != nullptr) AttachWindow(window());
  emit sessionChanged();
}

void MonitorItem::AttachWindow(QQuickWindow* quick_window) {
  if (quick_window == nullptr || quick_window == attached_window_) return;
  if (window_sync_connection_) disconnect(window_sync_connection_);
  attached_window_ = quick_window;
  announced_device_ = nullptr;
  window_sync_connection_ = connect(quick_window, &QQuickWindow::beforeSynchronizing, this, [this, quick_window] {
    auto* renderer = quick_window->rendererInterface();
    if (renderer == nullptr || renderer->graphicsApi() != QSGRendererInterface::Direct3D11) return;
    void* device = renderer->getResource(quick_window, QSGRendererInterface::DeviceResource);
    void* context = renderer->getResource(quick_window, QSGRendererInterface::DeviceContextResource);
    if (device == nullptr || device == announced_device_) return;
    announced_device_ = device;
    QMetaObject::invokeMethod(this, [this, device, context] {
      if (session_ != nullptr) session_->SetPresentationDevice(device, context);
    }, Qt::QueuedConnection);
  }, Qt::DirectConnection);
}

QString MonitorItem::zoomMode() const {
  switch (view_.zoom) {
    case ui::ZoomMode::Fit: return "fit";
    case ui::ZoomMode::Fill: return "fill";
    case ui::ZoomMode::Actual: return "actual";
    case ui::ZoomMode::Custom: return "custom";
  }
  return "fit";
}

void MonitorItem::setZoomMode(const QString& mode) {
  view_.zoom = mode == "fill" ? ui::ZoomMode::Fill : mode == "actual" ? ui::ZoomMode::Actual : mode == "custom" ? ui::ZoomMode::Custom : ui::ZoomMode::Fit;
  view_.pan_x = view_.pan_y = 0;
  UpdateDisplay();
  update();
  emit zoomChanged();
}

void MonitorItem::geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) {
  QQuickPaintedItem::geometryChange(new_geometry, old_geometry);
  if (video_item_ != nullptr) {
    video_item_->setWidth(new_geometry.width());
    video_item_->setHeight(new_geometry.height());
  }
  UpdateDisplay();
}

void MonitorItem::UpdateDisplay() {
  if (session_ == nullptr || width() <= 0 || height() <= 0) return;
  const auto size = session_->currentFrameSequenceSize();
  const auto ratio = window() != nullptr ? window()->devicePixelRatio() : 1.0;
  const auto rect = ui::FrameRect({size.width(), size.height()}, 1.0, {static_cast<int>(width()), static_cast<int>(height())}, view_);
  // The picture as many device pixels as it covers on screen, which is what Auto quality renders to.
  session_->SetMonitorDisplay(static_cast<int>(rect.width * ratio), static_cast<int>(rect.height * ratio));
}

void MonitorItem::paint(QPainter* painter) {
  painter->setCompositionMode(QPainter::CompositionMode_Source);
  painter->fillRect(boundingRect(), Qt::transparent);
  painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
  if (session_ == nullptr) return;
  const auto image = session_->currentFrame();
  const auto gpu = session_->currentGpuFrame();
  const auto sequence_size = session_->currentFrameSequenceSize();
  const auto window_size = ui::SizePx{static_cast<int>(width()), static_cast<int>(height())};
  const auto rect = ui::FrameRect({sequence_size.width(), sequence_size.height()}, 1.0, window_size, view_);
  const QRectF target(rect.x, rect.y, rect.width, rect.height);
  if (image.isNull() && !gpu.valid()) {
    painter->setPen(QColor("#5c6462"));
    painter->drawText(boundingRect(), Qt::AlignCenter, session_->projectOpen() ? tr("Nothing at the playhead") : tr("No project open"));
  }

  ui::OverlayOptions overlays;
  overlays.safe_margins = session_->safeMargins();
  overlays.action_percent = session_->prefs().GetReal("monitor.action_safe_percent");
  overlays.title_percent = session_->prefs().GetReal("monitor.title_safe_percent");
  const ui::Rect2 frame_rect{rect.x, rect.y, rect.width, rect.height};
  painter->setBrush(Qt::NoBrush);
  for (const auto& shape : ui::BuildOverlays(frame_rect, overlays)) {
    QPen pen(shape.tag == "title_safe" ? QColor(120, 220, 140, 200) : QColor(240, 200, 80, 200));
    pen.setWidthF(1.0);
    painter->setPen(pen);
    if (shape.kind == ui::OverlayShape::Kind::Rectangle) painter->drawRect(QRectF(shape.rect.x, shape.rect.y, shape.rect.width, shape.rect.height));
    else painter->drawLine(QPointF(shape.x0, shape.y0), QPointF(shape.x1, shape.y1));
  }

  // The mask being drawn or edited, over the picture, with its handles.
  {
    const auto mask = session_->maskOverlay();
    const auto map_point = [&](double x, double y) { return QPointF(target.x() + x * target.width(), target.y() + y * target.height()); };
    const auto outline = mask.value("outline").toList();
    if (mask.value("active").toBool() && outline.size() >= 2) {
      QPolygonF polygon;
      for (const auto& p : outline) polygon << map_point(p.toList()[0].toDouble(), p.toList()[1].toDouble());
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing, true);
      painter->setPen(QPen(QColor(0, 0, 0, 160), 3.0));
      painter->drawPolygon(polygon);
      painter->setPen(QPen(QColor("#d9ff62"), 1.5));
      painter->setBrush(QColor(217, 255, 98, 28));
      painter->drawPolygon(polygon);
      painter->setBrush(Qt::NoBrush);
      // Tangent lines run from the selected vertex to its handles.
      const auto handles = mask.value("handles").toList();
      QPointF selected;
      bool have_selected = false;
      for (const auto& h : handles) {
        const auto m = h.toMap();
        if (m.value("selected").toBool()) {
          selected = map_point(m.value("x").toDouble(), m.value("y").toDouble());
          have_selected = true;
        }
      }
      using Kind = ui::MaskHandle::Kind;
      for (const auto& h : handles) {
        const auto m = h.toMap();
        const auto kind = static_cast<Kind>(m.value("kind").toInt());
        const auto at = map_point(m.value("x").toDouble(), m.value("y").toDouble());
        const bool tangent = kind == Kind::TangentIn || kind == Kind::TangentOut;
        if (tangent && have_selected) {
          painter->setPen(QPen(QColor("#d9ff62"), 1.0));
          painter->drawLine(selected, at);
        }
        painter->setPen(QPen(QColor("#101111"), 1.0));
        painter->setBrush(m.value("selected").toBool() ? QColor("#d9ff62") : QColor("#ffffff"));
        if (tangent || kind == Kind::Rotate) painter->drawEllipse(at, 4.0, 4.0);
        else painter->drawRect(QRectF(at.x() - 4, at.y() - 4, 8, 8));
      }
      painter->restore();
    }
    // A path being drawn: the vertices so far.
    const auto pen_points = mask.value("penPoints").toList();
    if (!pen_points.isEmpty()) {
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing, true);
      painter->setPen(QPen(QColor("#d9ff62"), 1.5));
      QPointF previous;
      for (int i = 0; i < pen_points.size(); ++i) {
        const auto at = map_point(pen_points[i].toList()[0].toDouble(), pen_points[i].toList()[1].toDouble());
        if (i > 0) painter->drawLine(previous, at);
        painter->setBrush(i == 0 ? QColor("#d9ff62") : QColor("#ffffff"));
        painter->drawEllipse(at, 4.0, 4.0);
        previous = at;
      }
      painter->restore();
    }
  }

  if (session_->prefs().GetBool("monitor.show_timecode")) {
    QFont font("Consolas", 10);
    painter->setFont(font);
    const QFontMetricsF metrics(font);
    const auto left_text = session_->timecode();
    const auto right_text = session_->renderInfo() + "  " + session_->monitorQuality();
    const double left_width = metrics.horizontalAdvance(left_text) + 16.0;
    const double right_width = metrics.horizontalAdvance(right_text) + 16.0;
    const QRectF left_badge(target.left() + 10, target.bottom() - 32, left_width, 22);
    const QRectF right_badge(target.right() - right_width - 10, target.bottom() - 32, right_width, 22);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(10, 12, 15, 205));
    painter->drawRoundedRect(left_badge, 4, 4);
    painter->drawRoundedRect(right_badge, 4, 4);
    painter->setPen(QColor("#c4ed6a"));
    painter->drawText(left_badge, Qt::AlignCenter, left_text);
    painter->setPen(QColor("#aab4bf"));
    painter->drawText(right_badge, Qt::AlignCenter, right_text);
  }
}

void MonitorItem::wheelEvent(QWheelEvent* event) {
  if (view_.zoom == ui::ZoomMode::Fit) return;
  const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
  if (view_.zoom != ui::ZoomMode::Custom) {
    const auto size = session_ != nullptr ? session_->currentFrameSequenceSize() : QSize(1920, 1080);
    const auto rect = ui::FrameRect({size.width(), size.height()}, 1.0, {static_cast<int>(width()), static_cast<int>(height())}, view_);
    view_.custom_zoom = rect.width / size.width();
    view_.zoom = ui::ZoomMode::Custom;
  }
  view_.custom_zoom *= factor;
  UpdateDisplay();
  update();
}

QRectF MonitorItem::PictureRect() const {
  const auto size = session_ != nullptr ? session_->currentFrameSequenceSize() : QSize(1920, 1080);
  const auto rect = ui::FrameRect({size.width(), size.height()}, 1.0, {static_cast<int>(width()), static_cast<int>(height())}, view_);
  return QRectF(rect.x, rect.y, rect.width, rect.height);
}

QPointF MonitorItem::PicturePoint(const QPointF& position) const {
  const auto rect = PictureRect();
  if (rect.width() <= 0 || rect.height() <= 0) return {0, 0};
  return {(position.x() - rect.x()) / rect.width(), (position.y() - rect.y()) / rect.height()};
}

void MonitorItem::mousePressEvent(QMouseEvent* event) {
  forceActiveFocus();
  last_ = event->position();
  event->accept();
  if (session_ != nullptr && event->button() == Qt::LeftButton && session_->maskInteractive()) {
    const auto p = PicturePoint(event->position());
    // A grab radius of seven pixels on the screen, in the picture's own pixels.
    const auto rect = PictureRect();
    const double scale = rect.width() > 0 ? static_cast<double>(session_->currentFrameSequenceSize().width()) / rect.width() : 1.0;
    mask_gesture_ = session_->maskPress(p.x(), p.y(), 7.0 * scale, static_cast<int>(event->modifiers()));
    update();
  }
}

void MonitorItem::mouseReleaseEvent(QMouseEvent* event) {
  event->accept();   // the release belongs to the press that was taken, or a double-click is never recognised
  if (session_ != nullptr && mask_gesture_) {
    const auto p = PicturePoint(event->position());
    (void)session_->maskRelease(p.x(), p.y(), static_cast<int>(event->modifiers()));
    mask_gesture_ = false;
    update();
  }
}

void MonitorItem::keyPressEvent(QKeyEvent* event) {
  if (session_ == nullptr || !session_->maskInteractive()) {
    event->ignore();
    return;
  }
  if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
    if (session_->maskDeletePoint()) return;
  } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    session_->maskFinishPen();
    return;
  } else if (event->key() == Qt::Key_Escape) {
    session_->maskCancelPen();
    session_->maskSetTool("none");
    return;
  }
  event->ignore();
}

void MonitorItem::mouseMoveEvent(QMouseEvent* event) {
  if (mask_gesture_ && session_ != nullptr) {
    const auto p = PicturePoint(event->position());
    (void)session_->maskMove(p.x(), p.y(), static_cast<int>(event->modifiers()));
    last_ = event->position();
    update();
    return;
  }
  if (view_.zoom != ui::ZoomMode::Fit) {
    view_.pan_x += event->position().x() - last_.x();
    view_.pan_y += event->position().y() - last_.y();
    update();
  }
  last_ = event->position();
}

void MonitorItem::mouseDoubleClickEvent(QMouseEvent* event) {
  mask_gesture_ = false;
  if (session_ != nullptr && session_->maskInteractive()) {
    const auto p = PicturePoint(event->position());
    const auto rect = PictureRect();
    const double scale = rect.width() > 0 ? static_cast<double>(session_->currentFrameSequenceSize().width()) / rect.width() : 1.0;
    if (session_->maskDoubleClick(p.x(), p.y(), 7.0 * scale)) return;
    // A double-click on nothing, with a mask tool in hand, does not change the zoom either.
    if (session_->maskTool() != "none") return;
  }
  // Double-click returns to the fitted view.
  setZoomMode("fit");
}

}  // namespace cutline::app
