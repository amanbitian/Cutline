#pragma once

// The program monitor as a Qt Quick item: draws the picture the session last received, fitted or zoomed, with the
// safe-margin and guide overlays, the timecode and the render size. Where the picture goes and what is drawn over it is
// ui/Monitor.h; this only paints it and passes the mouse on.

#include "app/Session.h"
#include "ui/Monitor.h"

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace cutline::app {

class MonitorVideoItem;

class MonitorItem : public QQuickPaintedItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(MonitorItem)
  Q_PROPERTY(cutline::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
  Q_PROPERTY(QString zoomMode READ zoomMode WRITE setZoomMode NOTIFY zoomChanged)

 public:
  explicit MonitorItem(QQuickItem* parent = nullptr);
  [[nodiscard]] Session* session() const { return session_; }
  void setSession(Session* session);
  [[nodiscard]] QString zoomMode() const;
  void setZoomMode(const QString& mode);
  void paint(QPainter* painter) override;

 signals:
  void sessionChanged();
  void zoomChanged();

 protected:
  void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;
  void wheelEvent(QWheelEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private:
  friend class MonitorVideoItem;
  void UpdateDisplay();
  void AttachWindow(QQuickWindow* window);
  // The pointer in the picture's own coordinates (0 to 1 across and down), whether or not it is over the picture.
  [[nodiscard]] QPointF PicturePoint(const QPointF& position) const;
  [[nodiscard]] QRectF PictureRect() const;
  bool mask_gesture_{false};

  Session* session_{nullptr};
  ui::MonitorView view_;
  QPointF last_;
  MonitorVideoItem* video_item_{nullptr};
  QQuickWindow* attached_window_{nullptr};
  QMetaObject::Connection window_sync_connection_;
  void* announced_device_{nullptr};
};

}  // namespace cutline::app
