#pragma once

// The multicam angle monitor as a Qt Quick item: every angle's picture tiled in one frame (the session reads the
// pictures on its own thread and delivers the tiled frame), with each angle's name and the key that cuts to it drawn on
// its tile and the live angle marked. A click on a tile cuts to that angle at the group's clock, also while it plays.

#include "app/Session.h"

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace cutline::app {

class MulticamItem : public QQuickPaintedItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(MulticamItem)
  Q_PROPERTY(cutline::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)

 public:
  explicit MulticamItem(QQuickItem* parent = nullptr);
  [[nodiscard]] Session* session() const { return session_; }
  void setSession(Session* session);
  void paint(QPainter* painter) override;
  // Where the tiled frame lands in the item (kept in the picture's shape), for painting and for resolving clicks.
  [[nodiscard]] QRectF PictureRect() const;

 signals:
  void sessionChanged();

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  Session* session_{nullptr};
};

}  // namespace cutline::app
