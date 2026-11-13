#pragma once

#include <QDBusConnection>
#include <QString>
#include <QTimer>
#include <QWidget>

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

public Q_SLOTS:
  void onStatus(const QString& status);
  void onPartial(const QString& text);
  void onResult(const QString& payload);
  void onNotification(const QString& code, const QString& subject, const QString& detail,
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
  QLabel* preview_;
  QLabel* hint_;
  QPushButton* copy_;
  QTimer hideTimer_;
  QString text_;
  vinput::dbus::Status phase_ = vinput::dbus::Status::Idle;
  quint64 revision_ = 0;
  bool resultReceived_ = false;
};

} // namespace vinput::gui
