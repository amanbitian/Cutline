#include "app/DockController.h"

namespace cutline::app {
namespace {

QVariantList PathList(const ui::NodePath& path) {
  QVariantList list;
  for (const auto step : path) list.push_back(step);
  return list;
}

}  // namespace

void DockController::setSession(Session* session) {
  if (session_ == session) return;
  session_ = session;
  if (session_ != nullptr) connect(session_, &Session::workspaceCommand, this, &DockController::command);
  emit sessionChanged();
  emit changed();
}

QStringList DockController::workspaces() const {
  QStringList names;
  if (session_ == nullptr) return names;
  for (const auto& name : session_->workspaces().Names()) names << QString::fromStdString(name);
  return names;
}

QString DockController::current() const { return session_ != nullptr ? QString::fromStdString(session_->workspaces().current()) : QString(); }
bool DockController::modified() const { return session_ != nullptr && session_->workspaces().modified(); }

QStringList DockController::closedPanels() const {
  QStringList closed;
  if (session_ == nullptr) return closed;
  for (const auto& panel : ui::BuiltInPanels().All()) {
    if (!ui::Has(session_->workspaces().layout(), panel.id)) closed << QString::fromStdString(panel.id);
  }
  return closed;
}

QString DockController::panelTitle(const QString& panel) const {
  const auto* def = ui::BuiltInPanels().Find(panel.toStdString());
  return def != nullptr ? QString::fromStdString(def->title) : panel;
}

QVariantMap DockController::arrange(double width, double height) const {
  QVariantList groups, splitters;
  if (session_ != nullptr && width > 0 && height > 0) {
    const auto arrangement = ui::Arrange(session_->workspaces().layout(), {0, 0, width, height}, ui::BuiltInPanels(), 5.0);
    for (const auto& group : arrangement.groups) {
      QVariantList tabs;
      for (const auto& tab : group.tabs) tabs.push_back(QVariantMap{{"id", QString::fromStdString(tab)}, {"title", panelTitle(QString::fromStdString(tab))}});
      groups.push_back(QVariantMap{{"x", group.rect.x}, {"y", group.rect.y}, {"width", group.rect.width}, {"height", group.rect.height},
                                   {"tabs", tabs}, {"active", group.active}, {"path", PathList(group.path)}, {"floating", group.floating_index}});
    }
    for (const auto& bar : arrangement.splitters) {
      splitters.push_back(QVariantMap{{"x", bar.rect.x}, {"y", bar.rect.y}, {"width", bar.rect.width}, {"height", bar.rect.height},
                                      {"horizontal", bar.orientation == ui::Orientation::Horizontal}, {"path", PathList(bar.path)},
                                      {"index", bar.index}, {"extent", bar.extent}});
    }
  }
  return {{"groups", groups}, {"splitters", splitters}};
}

void DockController::Touch() {
  if (session_ != nullptr) session_->SaveConfiguration();
  emit changed();
}

void DockController::activate(const QString& panel) {
  if (session_ != nullptr && ui::Activate(session_->workspaces().edit(), panel.toStdString())) Touch();
}

void DockController::closePanel(const QString& panel) {
  if (session_ != nullptr && ui::Remove(session_->workspaces().edit(), panel.toStdString())) Touch();
}

void DockController::showPanel(const QString& panel) {
  if (session_ != nullptr && ui::Show(session_->workspaces().edit(), panel.toStdString())) Touch();
}

void DockController::togglePanel(const QString& panel) {
  if (session_ == nullptr) return;
  if (ui::Has(session_->workspaces().layout(), panel.toStdString())) closePanel(panel);
  else showPanel(panel);
}

bool DockController::dock(const QString& panel, const QString& target, const QString& zone, double share) {
  if (session_ == nullptr) return false;
  const auto z = zone == "left" ? ui::DropZone::Left : zone == "right" ? ui::DropZone::Right : zone == "top" ? ui::DropZone::Top
               : zone == "bottom" ? ui::DropZone::Bottom : ui::DropZone::Center;
  const bool ok = ui::Dock(session_->workspaces().edit(), panel.toStdString(), target.toStdString(), z, share);
  if (ok) Touch();
  return ok;
}

void DockController::floatPanel(const QString& panel, double x, double y, double width, double height) {
  if (session_ != nullptr && ui::Float(session_->workspaces().edit(), panel.toStdString(), x, y, width, height)) Touch();
}

void DockController::moveSplitter(const QVariantList& path, int index, double pixels, double extent) {
  if (session_ == nullptr || extent <= 0) return;
  ui::NodePath node;
  for (const auto& step : path) node.push_back(step.toInt());
  if (ui::MoveSplitter(session_->workspaces().edit(), node, index, pixels / extent)) emit changed();
}

void DockController::switchWorkspace(const QString& name) {
  if (session_ != nullptr && session_->workspaces().Switch(name.toStdString())) Touch();
}

bool DockController::saveWorkspaceAs(const QString& name) {
  if (session_ == nullptr || !session_->workspaces().SaveAs(name.toStdString())) return false;
  Touch();
  return true;
}

void DockController::saveWorkspace() {
  if (session_ == nullptr) return;
  session_->workspaces().SaveCurrent();
  Touch();
}

void DockController::resetWorkspace() {
  if (session_ == nullptr) return;
  session_->workspaces().ResetCurrent();
  Touch();
}

bool DockController::deleteWorkspace(const QString& name) {
  if (session_ == nullptr || !session_->workspaces().Delete(name.toStdString())) return false;
  Touch();
  return true;
}

void DockController::command(const QString& command_id) {
  static const std::pair<const char*, const char*> workspaces[] = {{"workspace.editing", "Editing"}, {"workspace.assembly", "Assembly"},
                                                                   {"workspace.color", "Color"}, {"workspace.audio", "Audio"},
                                                                   {"workspace.effects", "Effects"}, {"workspace.multicam", "Multicam"}, {"workspace.text", "Text"}, {"workspace.graphics", "Graphics"}};
  for (const auto& [id, name] : workspaces) {
    if (command_id == id) {
      switchWorkspace(name);
      return;
    }
  }
  if (command_id == "workspace.reset") {
    resetWorkspace();
  } else if (command_id == "workspace.save_as") {
    emit saveAsRequested();
  } else if (command_id.startsWith("panelshow.")) {
    showPanel(command_id.mid(10));
  } else if (command_id.startsWith("panel.")) {
    togglePanel(command_id.mid(6));
  }
}

}  // namespace cutline::app
