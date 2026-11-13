#include "focused_screen.h"

#include <QtCore/qbytearray.h>
#include <QtCore/qcoreapplication.h>
#include <QtCore/qdir.h>
#include <QtCore/qiodevice.h>
#include <QtCore/qlist.h>
#include <QtCore/qobject.h>
#include <QtCore/qtemporaryfile.h>
#include <QtCore/qtimer.h>
#include <QtCore/qtmetamacros.h>
#include <QtCore/qvariant.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusmessage.h>
#include <QtDBus/qdbuspendingcall.h>
#include <QtDBus/qdbusreply.h>
#include <QtDBus/qdbusservicewatcher.h>
#include <utility>

namespace vinput::gui {
namespace {
constexpr auto kObjectPath = "/org/fcitx/Vinput/Capsule/Focus";
constexpr auto kScriptName = "vinput-capsule-focused-screen";
constexpr auto kKWin = "org.kde.KWin";
constexpr auto kScripting = "org.kde.kwin.Scripting";
constexpr auto kScript = R"JS(
let watched = null;
function publish() {
    const win = workspace.activeWindow;
    if (win && win.output) {
        callDBus('org.fcitx.Vinput.Capsule', '/org/fcitx/Vinput/Capsule/Focus',
                 'org.fcitx.Vinput.Capsule.Focus', 'setOutput', win.output.name);
    }
}
function watchActive() {
    if (watched) {
        try { watched.outputChanged.disconnect(publish); } catch (error) {}
    }
    watched = workspace.activeWindow;
    if (watched) watched.outputChanged.connect(publish);
    publish();
}
workspace.windowActivated.connect(watchActive);
workspace.screensChanged.connect(publish);
watchActive();
)JS";
} // namespace

FocusedScreen::FocusedScreen(QDBusConnection bus, QObject* parent)
    : QObject(parent), bus_(std::move(bus)),
      script_(QDir::tempPath() + "/vinput-focused-screen-XXXXXX.js") {
  const QByteArray source(kScript);
  if (!script_.open() || script_.write(source) != source.size() || !script_.flush()) {
    return;
  }
  registered_ = bus_.registerObject(kObjectPath, this,
                                    QDBusConnection::ExportScriptableSlots |
                                        QDBusConnection::ExportAllProperties);
  if (!registered_) {
    return;
  }
  auto* watcher =
      new QDBusServiceWatcher(kKWin, bus_, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString&, const QString&, const QString& owner) {
            ++generation_;
            if (!owner.isEmpty()) {
              restart();
            }
          });
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &FocusedScreen::stop);
  restart();
}

FocusedScreen::~FocusedScreen() {
  stop();
}

void FocusedScreen::stop() {
  ++generation_;
  if (!registered_) {
    return;
  }
  registered_ = false;
  bus_.unregisterObject(kObjectPath);
  auto message = QDBusMessage::createMethodCall(kKWin, "/Scripting", kScripting, "unloadScript");
  message.setArguments({kScriptName});
  bus_.send(message);
}

void FocusedScreen::setOutput(const QString& name) {
  // Keep the last focused output during transient desktop/menu focus.
  if (name.isEmpty() || name == output_) {
    return;
  }
  output_ = name;
  Q_EMIT outputChanged(output_);
}

void FocusedScreen::invoke(const QString& method, const QVariantList& arguments,
                           std::function<void(const QDBusMessage&)> callback) {
  auto message = QDBusMessage::createMethodCall(kKWin, "/Scripting", kScripting, method);
  message.setAutoStartService(false);
  message.setArguments(arguments);
  auto* pending = new QDBusPendingCallWatcher(bus_.asyncCall(message, 1000), this);
  connect(pending, &QDBusPendingCallWatcher::finished, this,
          [callback = std::move(callback)](QDBusPendingCallWatcher* call) {
            const auto reply = call->reply();
            call->deleteLater();
            callback(reply);
          });
}

void FocusedScreen::restart() {
  if (!registered_) {
    return;
  }
  const auto generation = ++generation_;
  invoke("unloadScript", {kScriptName}, [this, generation](const QDBusMessage& message) {
    if (generation != generation_ || message.type() == QDBusMessage::ErrorMessage) {
      return;
    }
    waitForUnload(generation, 20);
  });
}

void FocusedScreen::waitForUnload(quint64 generation, int retries) {
  invoke("isScriptLoaded", {kScriptName}, [this, generation, retries](const QDBusMessage& message) {
    const QDBusReply<bool> reply(message);
    if (generation != generation_ || !reply.isValid()) {
      return;
    }
    if (!reply.value()) {
      load(generation);
      return;
    }
    if (retries > 0) {
      QTimer::singleShot(25, this, [this, generation, retries] {
        if (generation == generation_) {
          waitForUnload(generation, retries - 1);
        }
      });
    }
  });
}

void FocusedScreen::load(quint64 generation) {
  invoke("loadScript", {script_.fileName(), kScriptName},
         [this, generation](const QDBusMessage& message) {
           const QDBusReply<int> reply(message);
           if (generation != generation_ || !reply.isValid() || reply.value() < 0) {
             return;
           }
           auto run = QDBusMessage::createMethodCall(
               kKWin, "/Scripting/Script" + QString::number(reply.value()), "org.kde.kwin.Script",
               "run");
           run.setAutoStartService(false);
           bus_.send(run);
         });
}

} // namespace vinput::gui
