#pragma once

#include <QtCore/qglobal.h>
#include <QtCore/qstring.h>
#include <QtCore/qtimer.h>
#include <QtCore/qtmetamacros.h>
#include <QtDBus/qdbusconnection.h>
#include <QtWidgets/qwidget.h>
#include <optional>

#include "common/dbus/dbus_interface.h"

class QLabel;
class QPushButton;
class QPaintEvent;
class QResizeEvent;

namespace vinput::gui {

// An observer only: recording and committing remain owned by daemon/addon.
class CapsuleWindow final : public QWidget {
  Q_OBJECT

public:
  explicit CapsuleWindow(QWidget* parent = nullptr);
  void observe(QDBusConnection bus);

  Q_SLOT void onStatus(const QString& status);
  Q_SLOT void onPartial(const QString& text);
  Q_SLOT void onResult(const QString& payload);
  Q_SLOT void onNotification(const QString& code, const QString& subject, const QString& detail,
                             const QString& raw);

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void createTray();
  void queryStatus(const QDBusConnection& bus);
  void showFailure(const QString& title);
  void present(const QString& title, const QString& hint);
  void refreshText();
  void copyText() const;

  QLabel* title_;
  QLabel* indicator_;
  QLabel* preview_;
  QLabel* hint_;
  QPushButton* copy_;
  QPushButton* quit_;
  QTimer hideTimer_;
  QString text_;
  std::optional<vinput::dbus::Status> phase_;
  quint64 revision_ = 0;
  bool resultReceived_ = false;
};

} // namespace vinput::gui
