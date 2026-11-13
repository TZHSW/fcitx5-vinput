#pragma once

#include <QtCore/qcontainerfwd.h>
#include <QtCore/qglobal.h>
#include <QtCore/qobject.h>
#include <QtCore/qstring.h>
#include <QtCore/qtemporaryfile.h>
#include <QtCore/qtmetamacros.h>
#include <QtCore/qvariant.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusmessage.h>
#include <functional>

namespace vinput::gui {

// KWin knows the focused native Wayland window even when this GUI uses XWayland.
class FocusedScreen final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.fcitx.Vinput.Capsule.Focus")
  Q_PROPERTY(QString output READ output NOTIFY outputChanged)

public:
  explicit FocusedScreen(QDBusConnection bus, QObject* parent = nullptr);
  ~FocusedScreen() override;
  [[nodiscard]] QString output() const { return output_; }
  Q_SLOT Q_SCRIPTABLE void setOutput(const QString& name);

Q_SIGNALS:
  void outputChanged(const QString&);

private:
  void restart();
  void waitForUnload(quint64 generation, int retries);
  void load(quint64 generation);
  void stop();
  void invoke(const QString& method, const QVariantList& arguments,
              std::function<void(const QDBusMessage&)> callback);

  QDBusConnection bus_;
  QTemporaryFile script_;
  QString output_;
  quint64 generation_ = 0;
  bool registered_ = false;
};

} // namespace vinput::gui
