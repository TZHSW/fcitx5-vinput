#include <QtCore/qcoreapplication.h>
#include <QtCore/qeventloop.h>
#include <QtCore/qfile.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qobject.h>
#include <QtCore/qrect.h>
#include <QtCore/qsize.h>
#include <QtCore/qstring.h>
#include <QtCore/qtimer.h>
#include <QtCore/qtmetamacros.h>
#include <QtCore/qtranslator.h>
#include <QtCore/qvariant.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusmessage.h>
#include <QtGui/qclipboard.h>
#include <QtGui/qpixmap.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qsystemtrayicon.h>
#include <cstdlib>
#include <iostream>

#include "common/dbus/dbus_interface.h"

#include "capsule/capsule_window.h"
#include "capsule/focused_screen.h"

class TestDaemon final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.fcitx.Vinput.Service")
public:
  QString status = "idle";
  QString startupPartial;
  Q_SLOT QString GetStatus() {
    if (!startupPartial.isEmpty()) {
      Q_EMIT RecognitionPartial(startupPartial);
    }
    return status;
  }
Q_SIGNALS:
  void StatusChanged(const QString&);
  void RecognitionPartial(const QString&);
  void RecognitionResult(const QString&);
};

class TestKWin final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")
public:
  bool loaded = true;
  int loads = 0;
  Q_SLOT bool unloadScript(const QString&) {
    QTimer::singleShot(30, this, [this] { loaded = false; });
    return true;
  }
  Q_SLOT bool isScriptLoaded(const QString&) const { return loaded; }
  Q_SLOT int loadScript(const QString& path, const QString&) {
    if (loaded || !QFile::exists(path)) {
      return -1;
    }
    loaded = true;
    ++loads;
    return 7;
  }
};

class TestKWinScript final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")
public:
  Q_SLOT void run() {
    auto message = QDBusMessage::createMethodCall(vinput::dbus::kCapsuleBusName,
                                                  "/org/fcitx/Vinput/Capsule/Focus",
                                                  "org.fcitx.Vinput.Capsule.Focus", "setOutput");
    message.setArguments({QString("focused-output")});
    QDBusConnection::sessionBus().send(message);
  }
};

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}

void Pump(int milliseconds = 100) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

QString CopyText(vinput::gui::CapsuleWindow& window) {
  auto* copy = window.findChild<QPushButton*>("capsuleCopy");
  Expect(copy != nullptr && copy->isEnabled(), "Copy must be enabled for retained text");
  copy->click();
  return QApplication::clipboard()->text();
}

void TestStateAndFocus() {
  vinput::gui::CapsuleWindow window;
  Expect(window.testAttribute(Qt::WA_ShowWithoutActivating), "Must not activate on show");
  Expect(window.windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus), "Must reject input focus");
  window.onStatus("recording");
  window.onPartial("中文 English");
  window.onPartial("中文和 English。");
  Expect(QSystemTrayIcon::isSystemTrayAvailable() ||
             window.findChild<QPushButton*>("capsuleCopy")->isVisible(),
         "Live text must remain copyable without a tray");
  Expect(CopyText(window) == "中文和 English。", "Cumulative partial replaces previous text");
  window.onStatus("recording");
  Expect(CopyText(window) == "中文和 English。", "Duplicate recording status must not clear text");
  window.onStatus("inferring");
  Expect(CopyText(window) == "中文和 English。", "Finalization retains the current hypothesis");
  window.onResult(R"({"commit_text":"最终 English。"})");
  auto* status = window.findChild<QLabel*>("capsuleStatus");
  Expect(status != nullptr && status->text() == "Recognition result",
         "Result is not insertion ACK");
  window.onStatus("idle");
  Expect(window.isVisible(), "Idle must not erase the just-arrived final result");
  window.onStatus("recording");
  window.onPartial("新会话");
  Pump(2000);
  Expect(window.isVisible(), "Previous result timer must not hide a new recording");
  Expect(CopyText(window) == "新会话", "New recording replaces old final text");
  window.onStatus("error");
  window.onStatus("idle");
  window.onPartial("迟到数据");
  Expect(window.isVisible() && CopyText(window) == "新会话", "Error preserves text across idle");
  window.onStatus("recording");
  window.onPartial("保留");
  window.onResult("{malformed");
  Expect(CopyText(window) == "保留", "Malformed result must not destroy the preview");
  window.onResult(R"({"candidates":[{"text":"取消","source":"cancel"}]})");
  Expect(window.isVisible() != QSystemTrayIcon::isSystemTrayAvailable(),
         "Cancellation hides only when the tray provides an exit control");
  Expect(!window.findChild<QPushButton*>("capsuleCopy")->isEnabled(), "Cancel discards text");
  window.onStatus("idle");
  window.onPartial("迟到的预览");
  Expect(!window.findChild<QPushButton*>("capsuleCopy")->isEnabled(),
         "Idle ignores delayed partials even when no tray is available");
  window.onResult(R"({"candidates":[{"text":"取消","source":"cancel"}]})");
  Expect(window.isVisible() != QSystemTrayIcon::isSystemTrayAvailable(),
         "Idle before cancellation must preserve the no-tray exit control");
  window.onStatus("recording");
  window.onPartial("放弃本次录音");
  window.onStatus("idle");
  Expect(CopyText(window) == "放弃本次录音" && status->text() == "Session ended",
         "Idle without a result may be cancellation or failure; keep an uncommitted preview");
  window.onStatus("recording");
  window.onPartial("网络中断前的文字");
  window.onStatus("inferring");
  window.onStatus("idle");
  window.onPartial("迟到的预览");
  Expect(CopyText(window) == "网络中断前的文字" && status->text() == "Session ended",
         "Finalization failure with only idle retains text and ignores late partials");
  window.onStatus("recording");
  window.onPartial("等待整理");
  window.onStatus("postprocessing");
  Expect(status->text() == "Rewriting" && CopyText(window) == "等待整理",
         "Postprocessing retains text and has its own status");
  window.onNotification("asr_error", "", "", "");
  window.onStatus("idle");
  Expect(CopyText(window) == "等待整理" && status->text() == "Recognition failed",
         "Legacy error notification preserves text across idle");
}

void TestScreenGeometry() {
  const QSize capsule(448, 92);
  const QRect right(1920, 0, 1920, 1040);
  const QRect left(-1280, -200, 1280, 720);
  const QRect portrait(0, -1080, 720, 1080);
  for (const auto& area : {right, left, portrait}) {
    const auto bounds = vinput::gui::CapsuleBounds(area, capsule);
    Expect(area.contains(bounds), "Capsule stays on the selected screen's available area");
    Expect(bounds.center().x() == area.center().x(),
           "Capsule centered on that screen, not desktop");
    Expect(area.bottom() - bounds.bottom() == 36,
           "Bottom margin uses the selected logical geometry");
  }
  const QRect tiny(-200, 10, 200, 60);
  Expect(tiny.contains(vinput::gui::CapsuleBounds(tiny, capsule)),
         "Small output clamps both dimensions");
}

void TestFocusedScreenBus() {
  auto client = QDBusConnection::sessionBus();
  auto server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "focus-test-server");
  TestKWin kwin;
  TestKWinScript script;
  Expect(client.registerService(vinput::dbus::kCapsuleBusName), "Own private capsule service");
  Expect(server.registerService("org.kde.KWin"), "Own private KWin fixture service");
  Expect(server.registerObject("/Scripting", &kwin, QDBusConnection::ExportAllSlots),
         "Export scripting API");
  Expect(server.registerObject("/Scripting/Script7", &script, QDBusConnection::ExportAllSlots),
         "Export script run API");
  {
    vinput::gui::FocusedScreen focused(client);
    Pump(400);
    Expect(kwin.loads == 1 && focused.output() == "focused-output",
           "Wait for stale script unload, load/run owned script, receive output over D-Bus");
    Expect(server.unregisterService("org.kde.KWin"), "Simulate compositor service restart");
    Pump();
    Expect(server.registerService("org.kde.KWin"), "Restore compositor service");
    Pump(400);
    Expect(kwin.loads == 2, "Reinstall focus tracking after compositor restart");
  }
  Pump();
  Expect(!kwin.loaded, "Tracker teardown unloads only its owned script");
  Expect(client.unregisterService(vinput::dbus::kCapsuleBusName),
         "Release capsule fixture service");
  Expect(server.unregisterService("org.kde.KWin"), "Release KWin fixture service");
}

void TestPrivateBus() {
  using namespace vinput::dbus;
  auto server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "capsule-test-server");
  TestDaemon daemon;
  Expect(server.registerService(kBusName), "Private fixture must own the test service");
  Expect(server.registerObject(kObjectPath, &daemon,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals),
         "Fixture must export existing daemon signatures");
  {
    daemon.status = "recording";
    daemon.startupPartial = "启动中 English";
    vinput::gui::CapsuleWindow startingWindow;
    startingWindow.observe(QDBusConnection::sessionBus());
    Pump();
    Expect(startingWindow.isVisible() && CopyText(startingWindow) == "启动中 English",
           "Real partial sent before startup reply must be retained and displayed");
  }
  daemon.status = "idle";
  daemon.startupPartial.clear();
  vinput::gui::CapsuleWindow window;
  window.observe(QDBusConnection::sessionBus());
  // Simulate a new live event before the asynchronous idle snapshot is delivered.
  window.onStatus("recording");
  window.onPartial("新状态优先");
  Pump();
  Expect(window.isVisible() && CopyText(window) == "新状态优先", "Stale startup reply ignored");
  Q_EMIT daemon.StatusChanged("inferring");
  Q_EMIT daemon.RecognitionPartial("真实 D-Bus 信号");
  Pump();
  Expect(CopyText(window) == "真实 D-Bus 信号", "Public D-Bus partial reaches the widget");
  Q_EMIT daemon.RecognitionResult(R"({"commit_text":"豆包 API"})");
  Q_EMIT daemon.StatusChanged("idle");
  Pump();
  Expect(window.isVisible() && CopyText(window) == "豆包 API", "Result survives following idle");
  Expect(server.unregisterService(kBusName), "Remove fixture service");
  Pump();
  Expect(window.isVisible() && CopyText(window) == "豆包 API", "Owner loss preserves text");
  Expect(server.registerService(kBusName), "Restart fixture service");
  Pump();
  Q_EMIT daemon.StatusChanged("recording");
  Q_EMIT daemon.RecognitionPartial("服务重连");
  Pump();
  Expect(CopyText(window) == "服务重连", "Subscriptions survive service restart");
}

void RenderPreviews() {
  vinput::gui::CapsuleWindow window;
  window.onStatus("recording");
  window.onPartial("把想到的，自然地说出来。");
  Pump();
  Expect(window.grab().save("capsule-recording.png"), "Recording preview rendered");
  window.onStatus("inferring");
  window.onResult(R"({"commit_text":"中文与 English，都可以自然表达。"})");
  Pump();
  Expect(window.grab().save("capsule-result.png"), "Result preview rendered");
  window.onStatus("recording");
  window.onPartial("识别到的文字会为你保留。");
  window.onStatus("error");
  Pump();
  Expect(window.grab().save("capsule-error.png"), "Error preview rendered");
}

} // namespace

int main(int argc, char* argv[]) {
  const QApplication app(argc, argv);
  QApplication::setQuitOnLastWindowClosed(false);
  if (QCoreApplication::arguments().contains("--render")) {
    QTranslator translator;
    Expect(translator.load("build/src/gui/vinput-gui_zh_CN.qm"), "Chinese translation available");
    QCoreApplication::installTranslator(&translator);
    RenderPreviews();
    return 0;
  }
  if (QCoreApplication::arguments().contains("--bus")) {
    TestFocusedScreenBus();
    TestPrivateBus();
  } else {
    TestScreenGeometry();
    TestStateAndFocus();
  }
  std::cout << "Capsule tests passed.\n";
  return 0;
}

#include "test_capsule_window.moc"
