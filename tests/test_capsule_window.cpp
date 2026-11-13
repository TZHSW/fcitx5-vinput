#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QEventLoop>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <cstdlib>
#include <iostream>

#include "common/dbus/dbus_interface.h"

#include "capsule/capsule_window.h"

class TestDaemon final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.fcitx.Vinput.Service")
public Q_SLOTS:
  QString GetStatus() { return "idle"; }
Q_SIGNALS:
  void StatusChanged(const QString& status);
  void RecognitionPartial(const QString& text);
  void RecognitionResult(const QString& payload);
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
  Expect(!window.isVisible(), "Cancelled input must not be presented as a result");
  Expect(!window.findChild<QPushButton*>("capsuleCopy")->isEnabled(), "Cancel discards text");
  window.onStatus("idle");
  window.onPartial("迟到的预览");
  Expect(!window.findChild<QPushButton*>("capsuleCopy")->isEnabled(),
         "Idle ignores delayed partials even when no tray is available");
  window.onStatus("recording");
  window.onPartial("放弃本次录音");
  window.onStatus("idle");
  Expect(!window.findChild<QPushButton*>("capsuleCopy")->isEnabled(),
         "Cancellation via idle without a result discards the partial");
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

void TestPrivateBus() {
  using namespace vinput::dbus;
  auto server = QDBusConnection::connectToBus(QDBusConnection::SessionBus, "capsule-test-server");
  TestDaemon daemon;
  Expect(server.registerService(kBusName), "Private fixture must own the test service");
  Expect(server.registerObject(kObjectPath, &daemon,
                               QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals),
         "Fixture must export existing daemon signatures");
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

} // namespace

int main(int argc, char* argv[]) {
  const QApplication app(argc, argv);
  QApplication::setQuitOnLastWindowClosed(false);
  if (QCoreApplication::arguments().contains("--bus")) {
    TestPrivateBus();
  } else {
    TestStateAndFocus();
  }
  std::cout << "Capsule tests passed.\n";
  return 0;
}

#include "test_capsule_window.moc"
