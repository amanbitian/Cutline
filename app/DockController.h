#pragma once

// The window's panel arrangement for QML: hands the layout's rectangles, tab strips and splitters to the page that
// draws them, and takes the gestures back (activate a tab, close, dock one panel onto another, float, drag a splitter,
// switch workspace). All of it is ui/Layout.h; nothing here decides anything.

#include "app/Session.h"
#include "ui/Layout.h"

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace cutline::app {

class DockController : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(DockController)
  Q_PROPERTY(cutline::app::Session* session READ session WRITE setSession NOTIFY sessionChanged)
  Q_PROPERTY(QStringList workspaces READ workspaces NOTIFY changed)
  Q_PROPERTY(QString current READ current NOTIFY changed)
  Q_PROPERTY(bool modified READ modified NOTIFY changed)
  Q_PROPERTY(QStringList closedPanels READ closedPanels NOTIFY changed)

 public:
  explicit DockController(QObject* parent = nullptr) : QObject(parent) {}
  [[nodiscard]] Session* session() const { return session_; }
  void setSession(Session* session);
  [[nodiscard]] QStringList workspaces() const;
  [[nodiscard]] QString current() const;
  [[nodiscard]] bool modified() const;
  [[nodiscard]] QStringList closedPanels() const;

  // {groups: [{x, y, width, height, tabs: [{id, title}], active, path, floating}], splitters: [{x, y, width, height, horizontal, path, index, extent}]}
  Q_INVOKABLE QVariantMap arrange(double width, double height) const;
  Q_INVOKABLE void activate(const QString& panel);
  Q_INVOKABLE void closePanel(const QString& panel);
  Q_INVOKABLE void showPanel(const QString& panel);
  Q_INVOKABLE void togglePanel(const QString& panel);
  Q_INVOKABLE bool dock(const QString& panel, const QString& target, const QString& zone, double share);
  Q_INVOKABLE void floatPanel(const QString& panel, double x, double y, double width, double height);
  Q_INVOKABLE void moveSplitter(const QVariantList& path, int index, double pixels, double extent);
  Q_INVOKABLE void switchWorkspace(const QString& name);
  Q_INVOKABLE bool saveWorkspaceAs(const QString& name);
  Q_INVOKABLE void saveWorkspace();
  Q_INVOKABLE void resetWorkspace();
  Q_INVOKABLE bool deleteWorkspace(const QString& name);
  Q_INVOKABLE QString panelTitle(const QString& panel) const;
  // Handles the workspace.* and panel.* application commands.
  Q_INVOKABLE void command(const QString& command_id);

 signals:
  void sessionChanged();
  void changed();
  void saveAsRequested();

 private:
  void Touch();
  Session* session_{nullptr};
};

}  // namespace cutline::app
