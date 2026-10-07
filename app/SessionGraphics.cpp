// Titles and graphics in the application: the project's library of them, the templates they can be made from, the controls
// of a title on the timeline, and the designer where one is drawn. The rules (what a drag does, what a plan changes) are in
// ui/GraphicsDesigner.h and the drawing is render/Graphics.h; this is the glue to the window and the project.

#include "app/Session.h"

#include "app/GraphicPreview.h"
#include "core/db/Sql.h"
#include "effects/GraphicsDocument.h"
#include "render/TextRaster.h"
#include "ui/GraphicsDesigner.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace cutline::app {

namespace gfx = render::graphics;
using time::RationalTime;

namespace {

QString LocalFile(const QString& url_or_path) {
  const QUrl url(url_or_path);
  return url.isLocalFile() ? url.toLocalFile() : url_or_path;
}

std::string ReadAll(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open " + path.toStdString());
  return file.readAll().toStdString();
}

void WriteAll(const QString& path, const std::string& text) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw std::runtime_error("Could not write " + path.toStdString());
  file.write(text.data(), static_cast<qint64>(text.size()));
}

std::string Slug(const std::string& name) {
  std::string slug;
  for (const auto c : name) {
    if (std::isalnum(static_cast<unsigned char>(c))) slug += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    else if (!slug.empty() && slug.back() != '-') slug += '-';
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  return slug.empty() ? "title" : slug;
}

const char* GripName(ui::Grip grip) {
  switch (grip) {
    case ui::Grip::Body: return "body";
    case ui::Grip::TopLeft: return "topLeft";
    case ui::Grip::Top: return "top";
    case ui::Grip::TopRight: return "topRight";
    case ui::Grip::Right: return "right";
    case ui::Grip::BottomRight: return "bottomRight";
    case ui::Grip::Bottom: return "bottom";
    case ui::Grip::BottomLeft: return "bottomLeft";
    case ui::Grip::Left: return "left";
    case ui::Grip::Turn: return "turn";
    case ui::Grip::None: break;
  }
  return "none";
}

const char* TypeName(gfx::ElementType type) {
  switch (type) {
    case gfx::ElementType::Text: return "text";
    case gfx::ElementType::Rectangle: return "rectangle";
    case gfx::ElementType::Ellipse: return "ellipse";
    case gfx::ElementType::Image: return "image";
  }
  return "rectangle";
}

}  // namespace

struct Session::DesignerState {
  bool open{false};
  std::string graphic_id;   // empty until it is first saved
  std::string name;
  std::string stored_name;   // the name the project has for it
  gfx::Document document;
  gfx::Document saved;
  std::string selected;
  std::vector<gfx::Document> undo, redo;
  int revision{0};
  // A drag in progress, measured from where it began.
  bool dragging{false};
  ui::Grip grip{ui::Grip::None};
  ui::DocPoint start;
  gfx::Document at_press;
  std::vector<ui::Guide> guides;
  // The preview made for the current revision.
  int preview_revision{-1};
  QString preview_key;
};

// ------------------------------------------------------------------------ the library ----

std::string Session::BindPictures(const std::string& document_json) const {
  if (document_json.find("\"media:") == std::string::npos || store_ == nullptr) return document_json;
  try {
    auto document = gfx::ParseDocument(document_json);
    bool changed = false;
    const std::lock_guard<std::mutex> lock(store_->mutex());
    for (auto& element : document.elements) {
      if (element.type != gfx::ElementType::Image || element.asset.rfind("media:", 0) != 0) continue;
      db::Statement statement(store_->connection(), "SELECT original_path FROM media WHERE id = ?;");
      statement.Bind(1, element.asset.substr(6));
      if (!statement.Step()) continue;
      element.asset = statement.ColumnText(0);
      changed = true;
    }
    return changed ? gfx::ToJson(document) : document_json;
  } catch (const std::exception&) {
    return document_json;
  }
}

QString Session::PreviewKey(const std::string& document_json) {
  const auto bound = BindPictures(document_json);
  const auto key = QString("p") + QCryptographicHash::hash(QByteArray::fromStdString(bound), QCryptographicHash::Md5).toHex().left(16);
  GraphicPreviewStore::Instance().Put(key, bound);
  return key;
}

std::string Session::GraphicDocumentJson(const std::string& id) const {
  if (store_ == nullptr) return {};
  std::string kind, document, template_id, values;
  std::int64_t version = 0;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT kind, document_json, template_id, template_version, values_json FROM graphics WHERE id = ?;");
    statement.Bind(1, id);
    if (!statement.Step()) return {};
    kind = statement.ColumnText(0);
    document = statement.ColumnText(1);
    template_id = statement.ColumnText(2);
    version = statement.ColumnInt(3);
    values = statement.ColumnText(4);
  }
  if (kind == "graphic") return document;
  const auto package = InstalledTemplateJson(template_id, version);
  if (!package) return {};
  try {
    return gfx::ToJson(gfx::Instantiate(gfx::ParseTemplate(*package), gfx::ValuesFromJson(values)));
  } catch (const std::exception&) {
    return {};
  }
}

std::optional<std::string> Session::InstalledTemplateJson(const std::string& id, std::int64_t version) const {
  if (store_ == nullptr) return std::nullopt;
  const std::lock_guard<std::mutex> lock(store_->mutex());
  db::Statement statement(store_->connection(), "SELECT package_json FROM graphic_templates WHERE id = ? AND version = ?;");
  statement.Bind(1, id).Bind(2, version);
  if (!statement.Step()) return std::nullopt;
  return statement.ColumnText(0);
}

QVariantList Session::graphicLibrary() {
  QVariantList list;
  if (store_ == nullptr) return list;
  struct Row {
    std::string id, name, kind, template_id;
    std::int64_t version{0};
    std::int64_t uses{0};
  };
  std::vector<Row> rows;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), R"sql(
      SELECT g.id, g.name, g.kind, g.template_id, g.template_version,
             (SELECT COUNT(*) FROM effects e WHERE e.preset_name = 'project:' || g.id)
        FROM graphics g ORDER BY g.name, g.id;
    )sql");
    while (statement.Step()) {
      rows.push_back({statement.ColumnText(0), statement.ColumnText(1), statement.ColumnText(2), statement.ColumnText(3), statement.ColumnInt(4), statement.ColumnInt(5)});
    }
  }
  for (const auto& row : rows) {
    const auto json = GraphicDocumentJson(row.id);
    QString preview;
    if (!json.empty()) {
      try {
        preview = "image://graphicpreview/" + PreviewKey(gfx::ToJson(ui::ForPreview(gfx::ParseDocument(json)))) + "@240";
      } catch (const std::exception&) {
      }
    }
    list << QVariantMap{{"id", QString::fromStdString(row.id)},
                        {"name", QString::fromStdString(row.name)},
                        {"kind", QString::fromStdString(row.kind)},
                        {"templateId", QString::fromStdString(row.template_id)},
                        {"templateVersion", static_cast<qlonglong>(row.version)},
                        {"uses", static_cast<qlonglong>(row.uses)},
                        {"preview", preview}};
  }
  return list;
}

QVariantList Session::graphicTemplates() {
  QVariantList list;
  std::set<std::string> installed;
  const auto entry = [&](const gfx::TemplatePackage& package, bool is_installed) {
    QString preview;
    try {
      preview = "image://graphicpreview/" + PreviewKey(gfx::ToJson(ui::ForPreview(gfx::Instantiate(package, {})))) + "@240";
    } catch (const std::exception&) {
    }
    list << QVariantMap{{"id", QString::fromStdString(package.id)},
                        {"version", static_cast<qlonglong>(package.version)},
                        {"name", QString::fromStdString(package.name.empty() ? package.id : package.name)},
                        {"description", QString::fromStdString(package.description)},
                        {"installed", is_installed},
                        {"preview", preview}};
  };
  if (store_ != nullptr) {
    std::vector<std::string> packages;
    {
      const std::lock_guard<std::mutex> lock(store_->mutex());
      db::Statement statement(store_->connection(), "SELECT id, package_json FROM graphic_templates ORDER BY id, version DESC;");
      std::string last;
      while (statement.Step()) {
        const auto id = statement.ColumnText(0);
        if (id == last) continue;   // the newest version of each
        last = id;
        packages.push_back(statement.ColumnText(1));
      }
    }
    for (const auto& json : packages) {
      try {
        const auto package = gfx::ParseTemplate(json);
        installed.insert(package.id);
        entry(package, true);
      } catch (const std::exception&) {
      }
    }
  }
  for (const auto& package : ui::BuiltInTemplates()) {
    if (installed.count(package.id) == 0) entry(package, false);
  }
  return list;
}

QString Session::newGraphic(const QString& name) {
  if (store_ == nullptr || graph_.root() == nullptr) return {};
  auto document = ui::NewGraphicDocument(static_cast<int>(graph_.root()->width), static_cast<int>(graph_.root()->height));
  const auto id = ui::AddElement(document, gfx::ElementType::Text);
  if (auto* text = ui::FindElement(document, id)) {
    text->text = "Title";
    text->font_size = 0.14;
    text->bold = true;
    text->y = 0.4;
    text->height = 0.2;
  }
  std::string graphic;
  const auto plan = ui::PlanCreateGraphic(EditContextFor(), name.trimmed().isEmpty() ? tr("Title").toStdString() : name.trimmed().toStdString(), document, &graphic);
  if (!Apply(plan)) return {};
  (void)openGraphic(QString::fromStdString(graphic));
  return QString::fromStdString(graphic);
}

QString Session::newGraphicFromTemplate(const QString& template_id, int version, const QString& name) {
  if (store_ == nullptr || graph_.root() == nullptr) return {};
  const auto wanted = template_id.toStdString();
  std::optional<gfx::TemplatePackage> package;
  bool installed = false;
  if (const auto json = InstalledTemplateJson(wanted, version)) {
    try {
      package = gfx::ParseTemplate(*json);
      installed = true;
    } catch (const std::exception&) {
    }
  }
  if (!package) {
    for (const auto& built_in : ui::BuiltInTemplates()) {
      if (built_in.id == wanted) package = built_in;
    }
  }
  if (!package) {
    ShowStatus(tr("There is no template called %1").arg(template_id));
    return {};
  }
  std::string graphic;
  auto plan = ui::PlanCreateFromTemplate(EditContextFor(), name.trimmed().isEmpty() ? package->name : name.trimmed().toStdString(), package->id, package->version, {}, &graphic);
  if (!installed) {
    // A built-in is added to the project's library with the first title made from it: one step.
    auto install = ui::PlanInstallTemplate(EditContextFor(), *package);
    if (!install.ok) {
      ShowStatus(QString::fromStdString(install.refusal));
      return {};
    }
    plan.commands.insert(plan.commands.begin(), install.commands.begin(), install.commands.end());
  }
  if (!Apply(plan)) return {};
  return QString::fromStdString(graphic);
}

bool Session::placeGraphic(const QString& id, double seconds) {
  if (store_ == nullptr || graph_.root() == nullptr) return false;
  QString name;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT name FROM graphics WHERE id = ?;");
    statement.Bind(1, id.toStdString());
    if (!statement.Step()) {
      ShowStatus(tr("There is no such graphic"));
      return false;
    }
    name = QString::fromStdString(statement.ColumnText(0));
  }
  const RationalTime length(static_cast<std::int64_t>(std::llround(std::max(seconds, 0.04) * 1000.0)), 1000);
  return Apply(ui::PlanPlaceGraphic(EditContextFor(), id.toStdString(), name.toStdString(), transport_.position(), length));
}

bool Session::renameGraphic(const QString& id, const QString& name) {
  return Apply(ui::PlanRenameGraphic(EditContextFor(), id.toStdString(), name.trimmed().toStdString()));
}

bool Session::deleteGraphic(const QString& id) {
  if (designer_ && designer_->open && designer_->graphic_id == id.toStdString()) designerClose();
  return Apply(ui::PlanDeleteGraphic(EditContextFor(), id.toStdString()));
}

bool Session::importGraphicTemplate(const QString& url) {
  if (store_ == nullptr) return false;
  const auto path = LocalFile(url);
  try {
    const auto text = ReadAll(path);
    std::optional<gfx::TemplatePackage> package;
    try {
      package = gfx::ParseTemplate(text);
    } catch (const std::exception&) {
      package.reset();
    }
    if (package) return Apply(ui::PlanInstallTemplate(EditContextFor(), *package));
    // Not a package: a plain graphic file becomes a graphic of the project.
    const auto document = gfx::ParseDocument(text);
    const auto plan = ui::PlanCreateGraphic(EditContextFor(), QFileInfo(path).completeBaseName().toStdString(), document);
    return Apply(plan);
  } catch (const std::exception& error) {
    ShowStatus(tr("Could not import %1: %2").arg(QFileInfo(path).fileName(), error.what()));
    return false;
  }
}

bool Session::exportGraphicTemplate(const QString& id, const QString& url) {
  if (store_ == nullptr) return false;
  try {
    std::string kind, document, template_id, name;
    std::int64_t version = 0;
    {
      const std::lock_guard<std::mutex> lock(store_->mutex());
      db::Statement statement(store_->connection(), "SELECT kind, document_json, template_id, template_version, name FROM graphics WHERE id = ?;");
      statement.Bind(1, id.toStdString());
      if (!statement.Step()) throw std::runtime_error("There is no such graphic");
      kind = statement.ColumnText(0);
      document = statement.ColumnText(1);
      template_id = statement.ColumnText(2);
      version = statement.ColumnInt(3);
      name = statement.ColumnText(4);
    }
    std::string text;
    if (kind == "template") {
      const auto package = InstalledTemplateJson(template_id, version);
      if (!package) throw std::runtime_error("The template this title was made from is not in the project");
      text = *package;
    } else {
      const auto parsed = gfx::ParseDocument(document);
      text = gfx::ToJson(ui::MakeTemplate(parsed, Slug(name), name, "", ui::AutoControls(parsed)));
    }
    WriteAll(LocalFile(url), text);
    ShowStatus(tr("Template saved to %1").arg(QFileInfo(LocalFile(url)).fileName()));
    return true;
  } catch (const std::exception& error) {
    ShowStatus(tr("Could not export the template: %1").arg(error.what()));
    return false;
  }
}

QVariantMap Session::clipGraphic(const QString& clip_id) const {
  const auto* sequence = graph_.root();
  if (sequence == nullptr || store_ == nullptr) return {};
  std::string graphic;
  for (const auto& track : sequence->tracks) {
    for (const auto& clip : track.clips) {
      if (clip.id != clip_id.toStdString()) continue;
      for (const auto& effect : clip.effects) {
        if (effect.preset_name.rfind("project:", 0) == 0) graphic = effect.preset_name.substr(8);
      }
    }
  }
  if (graphic.empty()) return {};
  std::string name, kind, template_id, values;
  std::int64_t version = 0;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT name, kind, template_id, template_version, values_json FROM graphics WHERE id = ?;");
    statement.Bind(1, graphic);
    if (!statement.Step()) return {};
    name = statement.ColumnText(0);
    kind = statement.ColumnText(1);
    template_id = statement.ColumnText(2);
    version = statement.ColumnInt(3);
    values = statement.ColumnText(4);
  }
  QVariantMap result{{"graphic", QString::fromStdString(graphic)}, {"name", QString::fromStdString(name)}, {"kind", QString::fromStdString(kind)},
                     {"template", QString::fromStdString(template_id)}, {"clip", clip_id}};
  QVariantList controls;
  if (kind == "template") {
    if (const auto json = InstalledTemplateJson(template_id, version)) {
      try {
        const auto package = gfx::ParseTemplate(*json);
        const auto current = gfx::ValuesFromJson(values);
        for (const auto& control : package.controls) {
          const auto found = current.find(control.name);
          const auto& property = control.property;
          const auto type = property == "text" ? "text" : property == "asset" ? "asset" : (property == "fill" || property == "stroke" || property == "shadow") ? "colour" : "number";
          controls << QVariantMap{{"name", QString::fromStdString(control.name)},
                                  {"label", QString::fromStdString(control.label.empty() ? control.name : control.label)},
                                  {"property", QString::fromStdString(property)},
                                  {"type", type},
                                  {"value", QString::fromStdString(found != current.end() && !found->second.empty() ? found->second : control.default_value)},
                                  {"minimum", control.minimum},
                                  {"maximum", control.maximum >= control.minimum ? control.maximum : 1.0e9}};
        }
      } catch (const std::exception&) {
      }
    }
  }
  result["controls"] = controls;
  return result;
}

QVariantMap Session::selectedGraphic() const {
  const auto clip = PrimaryClip();
  return clip ? clipGraphic(QString::fromStdString(*clip)) : QVariantMap{};
}

bool Session::setGraphicControl(const QString& graphic_id, const QString& name, const QString& value) {
  if (store_ == nullptr) return false;
  std::string values;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT values_json FROM graphics WHERE id = ? AND kind = 'template';");
    statement.Bind(1, graphic_id.toStdString());
    if (!statement.Step()) return false;
    values = statement.ColumnText(0);
  }
  try {
    auto current = gfx::ValuesFromJson(values);
    current[name.toStdString()] = value.toStdString();
    return Apply(ui::PlanSetTemplateValues(EditContextFor(), graphic_id.toStdString(), current));
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    return false;
  }
}

// ----------------------------------------------------------------------------- designer ----

int Session::designerRevision() const { return designer_ ? designer_->revision : 0; }
bool Session::designerOpen() const { return designer_ && designer_->open; }

bool Session::openGraphic(const QString& id) {
  if (store_ == nullptr) return false;
  std::string kind, document, name;
  {
    const std::lock_guard<std::mutex> lock(store_->mutex());
    db::Statement statement(store_->connection(), "SELECT kind, document_json, name FROM graphics WHERE id = ?;");
    statement.Bind(1, id.toStdString());
    if (!statement.Step()) {
      ShowStatus(tr("There is no such graphic"));
      return false;
    }
    kind = statement.ColumnText(0);
    document = statement.ColumnText(1);
    name = statement.ColumnText(2);
  }
  if (kind != "graphic") {
    ShowStatus(tr("A title made from a template is changed through its controls; save it as a graphic to design it freely"));
    return false;
  }
  try {
    auto state = std::make_shared<DesignerState>();
    state->open = true;
    state->graphic_id = id.toStdString();
    state->name = name;
    state->stored_name = name;
    state->document = gfx::ParseDocument(document);
    state->saved = state->document;
    if (!state->document.elements.empty()) state->selected = state->document.elements.back().id;
    state->revision = designer_ ? designer_->revision + 1 : 1;
    designer_ = std::move(state);
  } catch (const std::exception& error) {
    ShowStatus(tr("This graphic could not be opened: %1").arg(error.what()));
    return false;
  }
  emit designerChanged();
  emit designerRequested();
  return true;
}

void Session::designerClose() {
  if (!designer_) return;
  designer_->open = false;
  ++designer_->revision;
  emit designerChanged();
}

void Session::DesignerPushUndo() {
  if (!designer_ || !designer_->open) return;
  designer_->undo.push_back(designer_->document);
  if (designer_->undo.size() > 100) designer_->undo.erase(designer_->undo.begin());
  designer_->redo.clear();
}

void Session::DesignerEdited(bool) {
  if (!designer_) return;
  ++designer_->revision;
  emit designerChanged();
}

QString Session::DesignerPreviewUrl(int width) {
  if (!designer_ || !designer_->open) return {};
  if (designer_->preview_revision != designer_->revision) {
    try {
      designer_->preview_key = "design" + QString::number(designer_->revision);
      GraphicPreviewStore::Instance().Put(designer_->preview_key, BindPictures(gfx::ToJson(ui::ForPreview(designer_->document))));
      designer_->preview_revision = designer_->revision;
    } catch (const std::exception&) {
      return {};
    }
  }
  return "image://graphicpreview/" + designer_->preview_key + "@" + QString::number(width);
}

QVariantMap Session::designer() {
  if (!designer_ || !designer_->open) return {};
  const auto& state = *designer_;
  QVariantList problems;
  for (const auto& problem : ui::Problems(state.document)) problems << QString::fromStdString(problem);
  QVariantList fonts;
  for (const auto& family : gfx::MissingFonts(state.document)) fonts << QString::fromStdString(family);
  bool dirty = true;
  try {
    dirty = gfx::ToJson(ui::ForPreview(state.document)) != gfx::ToJson(ui::ForPreview(state.saved)) || state.graphic_id.empty() || state.name != state.stored_name;
  } catch (const std::exception&) {
  }
  return QVariantMap{{"id", QString::fromStdString(state.graphic_id)},
                     {"name", QString::fromStdString(state.name)},
                     {"width", state.document.width},
                     {"height", state.document.height},
                     {"dirty", dirty},
                     {"selected", QString::fromStdString(state.selected)},
                     {"canUndo", !state.undo.empty()},
                     {"canRedo", !state.redo.empty()},
                     {"problems", problems},
                     {"missingFonts", fonts},
                     {"preview", DesignerPreviewUrl(960)}};
}

QVariantList Session::designerElements() const {
  QVariantList list;
  if (!designer_ || !designer_->open) return list;
  for (auto it = designer_->document.elements.rbegin(); it != designer_->document.elements.rend(); ++it) {
    list << QVariantMap{{"id", QString::fromStdString(it->id)},
                        {"label", QString::fromStdString(ui::ElementLabel(*it))},
                        {"type", TypeName(it->type)},
                        {"selected", it->id == designer_->selected}};
  }
  return list;
}

QVariantList Session::designerProperties() const {
  QVariantList list;
  if (!designer_ || !designer_->open) return list;
  const auto* element = ui::FindElement(designer_->document, designer_->selected);
  if (element == nullptr) return list;
  for (const auto& property : ui::PropertiesOf(element->type)) {
    QStringList choices;
    for (const auto& choice : property.choices) choices << QString::fromStdString(choice);
    list << QVariantMap{{"name", QString::fromStdString(property.name)},
                        {"label", QString::fromStdString(property.label)},
                        {"group", QString::fromStdString(property.group)},
                        {"kind", QString::fromStdString(property.kind)},
                        {"value", QString::fromStdString(ui::PropertyValue(*element, property.name))},
                        {"minimum", property.minimum},
                        {"maximum", property.maximum},
                        {"step", property.step},
                        {"choices", choices}};
  }
  return list;
}

QVariantMap Session::designerSelection() const {
  if (!designer_ || !designer_->open) return {};
  const auto* element = ui::FindElement(designer_->document, designer_->selected);
  if (element == nullptr) return {};
  const auto box = ui::ElementBoxOf(designer_->document, *element);
  QVariantList grips;
  for (const auto& grip : ui::GripsOf(designer_->document, *element)) {
    grips << QVariantMap{{"name", GripName(grip.grip)}, {"x", grip.at.x}, {"y", grip.at.y}};
  }
  return QVariantMap{{"id", QString::fromStdString(element->id)}, {"x", box.x}, {"y", box.y}, {"width", box.width}, {"height", box.height},
                     {"rotation", element->rotation}, {"grips", grips}};
}

QVariantList Session::designerPictures() const {
  QVariantList list;
  for (const auto& item : media_) {
    const auto map = item.toMap();
    if (!map["video"].toBool()) continue;
    list << QVariantMap{{"value", "media:" + map["id"].toString()}, {"label", map["name"].toString()}};
  }
  return list;
}

void Session::designerSelect(const QString& id) {
  if (!designer_ || !designer_->open) return;
  const auto wanted = id.toStdString();
  designer_->selected = ui::FindElement(designer_->document, wanted) != nullptr ? wanted : std::string{};
  DesignerEdited(false);
}

QString Session::designerAdd(const QString& type) {
  if (!designer_ || !designer_->open) return {};
  const auto kind = type == "text" ? gfx::ElementType::Text : type == "ellipse" ? gfx::ElementType::Ellipse : type == "image" ? gfx::ElementType::Image : gfx::ElementType::Rectangle;
  DesignerPushUndo();
  designer_->selected = ui::AddElement(designer_->document, kind);
  DesignerEdited();
  return QString::fromStdString(designer_->selected);
}

void Session::designerRemove() {
  if (!designer_ || !designer_->open || designer_->selected.empty()) return;
  DesignerPushUndo();
  (void)ui::RemoveElement(designer_->document, designer_->selected);
  designer_->selected = designer_->document.elements.empty() ? std::string{} : designer_->document.elements.back().id;
  DesignerEdited();
}

void Session::designerDuplicate() {
  if (!designer_ || !designer_->open || designer_->selected.empty()) return;
  DesignerPushUndo();
  const auto copy = ui::DuplicateElement(designer_->document, designer_->selected);
  if (!copy.empty()) designer_->selected = copy;
  DesignerEdited();
}

void Session::designerRestack(const QString& where) {
  if (!designer_ || !designer_->open || designer_->selected.empty()) return;
  const auto how = where == "front" ? ui::Stacking::Front : where == "forward" ? ui::Stacking::Forward : where == "backward" ? ui::Stacking::Backward : ui::Stacking::Back;
  auto before = designer_->document;
  if (!ui::Restack(designer_->document, designer_->selected, how)) return;
  designer_->undo.push_back(std::move(before));
  designer_->redo.clear();
  DesignerEdited();
}

void Session::designerAlign(const QString& how) {
  if (!designer_ || !designer_->open || designer_->selected.empty()) return;
  const auto alignment = how == "left" ? ui::Alignment::Left : how == "right" ? ui::Alignment::Right : how == "top" ? ui::Alignment::Top
                       : how == "middle" ? ui::Alignment::Middle : how == "bottom" ? ui::Alignment::Bottom : ui::Alignment::Centre;
  DesignerPushUndo();
  ui::AlignElement(designer_->document, designer_->selected, alignment);
  DesignerEdited();
}

bool Session::designerSetProperty(const QString& name, const QString& value) {
  if (!designer_ || !designer_->open) return false;
  auto* element = ui::FindElement(designer_->document, designer_->selected);
  if (element == nullptr) return false;
  const auto before = designer_->document;
  try {
    ui::SetProperty(*element, name.toStdString(), value.toStdString());
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    DesignerEdited(false);   // the field shows what the element has again
    return false;
  }
  if (gfx::ToJson(ui::ForPreview(before)) != gfx::ToJson(ui::ForPreview(designer_->document))) {
    designer_->undo.push_back(before);
    if (designer_->undo.size() > 100) designer_->undo.erase(designer_->undo.begin());
    designer_->redo.clear();
  }
  DesignerEdited();
  return true;
}

bool Session::designerSetEntrance(const QString& kind, double seconds) {
  if (!designer_ || !designer_->open) return false;
  auto* element = ui::FindElement(designer_->document, designer_->selected);
  if (element == nullptr) return false;
  const auto before = designer_->document;
  try {
    ui::SetEntrance(*element, kind.toStdString(), seconds);
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    return false;
  }
  designer_->undo.push_back(before);
  designer_->redo.clear();
  DesignerEdited();
  return true;
}

QVariantMap Session::designerEntrance() const {
  if (!designer_ || !designer_->open) return {};
  const auto* element = ui::FindElement(designer_->document, designer_->selected);
  QStringList kinds;
  for (const auto& kind : ui::EntranceKinds()) kinds << QString::fromStdString(kind);
  double seconds = 0.0;
  const auto kind = element != nullptr ? ui::EntranceOf(*element, &seconds) : std::string("none");
  return QVariantMap{{"kind", QString::fromStdString(kind)}, {"seconds", seconds > 0.0 ? seconds : 0.5}, {"kinds", kinds}};
}

bool Session::designerSetSize(int width, int height) {
  if (!designer_ || !designer_->open || width < 16 || height < 16 || width > 16384 || height > 16384) return false;
  DesignerPushUndo();
  designer_->document.width = width;
  designer_->document.height = height;
  DesignerEdited();
  return true;
}

void Session::designerSetName(const QString& name) {
  if (!designer_ || !designer_->open || name.trimmed().isEmpty()) return;
  designer_->name = name.trimmed().toStdString();
  DesignerEdited(false);
}

QString Session::designerPress(double x, double y, bool) {
  if (!designer_ || !designer_->open) return "none";
  auto& state = *designer_;
  const ui::DocPoint at{x, y};
  ui::Grip grip = ui::Grip::None;
  if (const auto* selected = ui::FindElement(state.document, state.selected)) grip = ui::GripAt(state.document, *selected, at);
  if (grip == ui::Grip::None) {
    if (const auto hit = ui::HitElement(state.document, at)) {
      state.selected = *hit;
      grip = ui::Grip::Body;
    } else {
      state.selected.clear();
      DesignerEdited(false);
      return "none";
    }
  }
  DesignerPushUndo();
  state.dragging = true;
  state.grip = grip;
  state.start = at;
  state.at_press = state.document;
  state.guides.clear();
  DesignerEdited(false);
  return GripName(grip);
}

void Session::designerDrag(double x, double y, bool keep_proportions, bool snap) {
  if (!designer_ || !designer_->open || !designer_->dragging) return;
  auto& state = *designer_;
  auto document = state.at_press;
  const auto dx = x - state.start.x, dy = y - state.start.y;
  state.guides.clear();
  switch (state.grip) {
    case ui::Grip::Body: state.guides = ui::MoveBy(document, state.selected, dx, dy, snap); break;
    case ui::Grip::Turn: ui::TurnTo(document, state.selected, {x, y}); break;
    default: ui::ResizeBy(document, state.selected, state.grip, dx, dy, keep_proportions); break;
  }
  state.document = std::move(document);
  DesignerEdited(false);
}

void Session::designerRelease() {
  if (!designer_ || !designer_->open || !designer_->dragging) return;
  auto& state = *designer_;
  state.dragging = false;
  state.guides.clear();
  // A click that moved nothing is not an edit.
  try {
    if (!state.undo.empty() && gfx::ToJson(ui::ForPreview(state.undo.back())) == gfx::ToJson(ui::ForPreview(state.document))) state.undo.pop_back();
  } catch (const std::exception&) {
  }
  DesignerEdited(false);
}

QVariantList Session::designerGuides() const {
  QVariantList list;
  if (!designer_) return list;
  for (const auto& guide : designer_->guides) list << QVariantMap{{"vertical", guide.vertical}, {"at", guide.at}};
  return list;
}

void Session::designerUndo() {
  if (!designer_ || !designer_->open || designer_->undo.empty()) return;
  designer_->redo.push_back(designer_->document);
  designer_->document = std::move(designer_->undo.back());
  designer_->undo.pop_back();
  if (ui::FindElement(designer_->document, designer_->selected) == nullptr) designer_->selected.clear();
  DesignerEdited(false);
}

void Session::designerRedo() {
  if (!designer_ || !designer_->open || designer_->redo.empty()) return;
  designer_->undo.push_back(designer_->document);
  designer_->document = std::move(designer_->redo.back());
  designer_->redo.pop_back();
  if (ui::FindElement(designer_->document, designer_->selected) == nullptr) designer_->selected.clear();
  DesignerEdited(false);
}

bool Session::designerSave() {
  if (!designer_ || !designer_->open || store_ == nullptr) return false;
  auto& state = *designer_;
  if (const auto problems = ui::Problems(state.document); !problems.empty()) {
    ShowStatus(QString::fromStdString(problems.front()));
    return false;
  }
  std::string id = state.graphic_id;
  ui::EditPlan plan;
  if (id.empty()) {
    plan = ui::PlanCreateGraphic(EditContextFor(), state.name, state.document, &id);
  } else {
    plan = ui::PlanSaveGraphic(EditContextFor(), id, state.document);
    if (plan.ok && !state.name.empty() && state.name != state.stored_name) {
      commands::UpdateGraphicPayload rename;
      rename.id = id;
      rename.name = state.name;
      plan.commands.push_back({commands::CommandType::UpdateGraphic, rename});
    }
  }
  if (!plan.ok) {
    ShowStatus(QString::fromStdString(plan.refusal));
    return false;
  }
  if (!Apply(plan)) return false;
  state.graphic_id = id;
  state.stored_name = state.name;
  state.saved = state.document;
  ++state.revision;
  emit designerChanged();
  return true;
}

bool Session::designerSaveAsTemplate(const QString& name, const QString& description) {
  if (!designer_ || !designer_->open || store_ == nullptr) return false;
  const auto& document = designer_->document;
  if (const auto problems = ui::Problems(document); !problems.empty()) {
    ShowStatus(QString::fromStdString(problems.front()));
    return false;
  }
  try {
    const auto title = name.trimmed().isEmpty() ? designer_->name : name.trimmed().toStdString();
    const auto id = Slug(title);
    std::int64_t version = 1;
    {
      const std::lock_guard<std::mutex> lock(store_->mutex());
      db::Statement statement(store_->connection(), "SELECT COALESCE(MAX(version), 0) FROM graphic_templates WHERE id = ?;");
      statement.Bind(1, id);
      if (statement.Step()) version = statement.ColumnInt(0) + 1;
    }
    auto package = ui::MakeTemplate(document, id, title, description.toStdString(), ui::AutoControls(document));
    package.version = version;
    return Apply(ui::PlanInstallTemplate(EditContextFor(), package));
  } catch (const std::exception& error) {
    ShowStatus(QString::fromStdString(error.what()));
    return false;
  }
}

QStringList Session::designerFonts() const {
  QStringList fonts;
  for (const auto& family : render::text::InstalledFamilies()) fonts << QString::fromStdString(family);
  return fonts;
}

}  // namespace cutline::app
