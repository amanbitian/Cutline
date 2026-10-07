#pragma once

// Paints the latest result from render::AsyncScopes. Measurement stays in the
// render worker; this item only turns already-bounded density arrays into pixels.

#include "app/Session.h"

#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

namespace cutline::app {

class ScopeItem : public QQuickPaintedItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(ScopeItem)
  Q_PROPERTY(cutline::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)

 public:
  explicit ScopeItem(QQuickItem* parent = nullptr);
  [[nodiscard]] Session* session() const { return session_; }
  void setSession(Session* session);
  void paint(QPainter* painter) override;

 signals:
  void sessionChanged();

 private:
  Session* session_{nullptr};
};

}  // namespace cutline::app
