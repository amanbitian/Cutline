#include "app/GraphicPreview.h"
#include "app/LutPreviewProvider.h"
#include "app/Session.h"

#include <QGuiApplication>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

// The desktop application. Command-line switches exist for testing and for taking pictures of the window:
//   --demo                  open a generated demo project
//   --open <folder>         open a project
//   --config <folder>       keep preferences, shortcuts and workspaces here instead of the user's own
//   --screenshot <file>     save the window as a picture after --delay milliseconds, then quit
//   --delay <ms>            default 2500
//   --no-audio              drive the transport from the clock alone
//   --do <step>             after opening, do something (repeatable): trigger:<command id>, seek:<seconds>,
//                           select:<clip name>, effect:<effect type>, tool:<name>, workspace:<name>
int main(int argc, char* argv[]) {
  QQuickStyle::setStyle("Basic");
  QGuiApplication application(argc, argv);
  QFont interface_font("Segoe UI");
  interface_font.setPointSizeF(9.0);
  application.setFont(interface_font);
  application.setOrganizationName("Cutline");
  application.setApplicationName("Cutline");

  QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  QString open, screenshot;
  bool demo = false;
  bool no_audio = false;
  QStringList steps;
  int delay = 2500;
  const auto arguments = application.arguments();
  for (int i = 1; i < arguments.size(); ++i) {
    const auto& a = arguments[i];
    if (a == "--demo") demo = true;
    else if (a == "--open" && i + 1 < arguments.size()) open = arguments[++i];
    else if (a == "--config" && i + 1 < arguments.size()) config = arguments[++i];
    else if (a == "--screenshot" && i + 1 < arguments.size()) screenshot = arguments[++i];
    else if (a == "--no-audio") no_audio = true;
    else if (a == "--do" && i + 1 < arguments.size()) steps << arguments[++i];
    else if (a == "--delay" && i + 1 < arguments.size()) delay = arguments[++i].toInt();
  }

  cutline::app::Session session;
  session.SetConfigDirectory(config);
  if (no_audio) session.SetAudioEnabled(false);

  QQmlApplicationEngine engine;
  engine.addImageProvider("lutpreview", new cutline::app::LutPreviewProvider);
  engine.addImageProvider("graphicpreview", new cutline::app::GraphicPreviewProvider);
  engine.rootContext()->setContextProperty("session", &session);
  engine.rootContext()->setContextProperty("appSession", &session);
  engine.loadFromModule("Cutline", "Main");
  if (engine.rootObjects().isEmpty()) return 1;

  if (demo) session.newDemoProject();
  else if (!open.isEmpty()) session.openProject(open);

  for (const auto& step : steps) {
    const auto colon = step.indexOf(':');
    const auto what = step.left(colon), value = step.mid(colon + 1);
    if (what == "trigger") session.trigger(value);
    else if (what == "seek") session.seek(value.toDouble());
    else if (what == "tool") session.setTool(value);
    else if (what == "effect") session.addEffect(value, "");
    else if (what == "workspace") session.workspaces().Switch(value.toStdString());
    else if (what == "select") session.selectClipNamed(value);
    else if (what == "title") (void)session.placeGraphic(session.newGraphicFromTemplate(value, 1, QString()), 5.0);
    else if (what == "design") {
      // A graphic with a turned, outlined, shadowed box added to the starting title, open in the designer.
      const auto drawn = session.newGraphic(value);
      (void)session.designerAdd("rectangle");
      for (const auto& [name, setting] : {std::pair<QString, QString>{"fill", "#2255EE"}, {"corner_radius", "0.05"}, {"rotation", "12"}, {"stroke_width", "0.01"}, {"stroke", "#FFFFFF"}, {"shadow", "#000000B0"}, {"shadow_y", "0.02"}, {"shadow_blur", "0.03"}}) (void)session.designerSetProperty(name, setting);
      if (session.designerSave()) (void)session.placeGraphic(drawn, 5.0);
    }
    else if (what == "maskadd") (void)session.maskAdd(QString(), value);
    else if (what == "multicam" && value == "demo") (void)session.multicamDemo();
    else if (what == "multicamcut") session.multicamCut(value.toInt() - 1);
    else if (what == "multicamseek") session.multicamSeek(value.toDouble());
  }

  if (!screenshot.isEmpty()) {
    QTimer::singleShot(delay, [&]() {
      const bool saved = session.saveScreenshot(screenshot);
      QGuiApplication::exit(saved ? 0 : 2);
    });
  }
  return application.exec();
}
