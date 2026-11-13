#include "capsule_window.h"

#include <QtCore/qcoreapplication.h>
#include <QtCore/qglobal.h>
#include <QtCore/qnamespace.h>
#include <QtCore/qobjectdefs.h>
#include <QtCore/qprocess.h>
#include <QtCore/qrect.h>
#include <QtCore/qstringlist.h>
#include <QtDBus/qdbusconnection.h>
#include <QtDBus/qdbusmessage.h>
#include <QtDBus/qdbuspendingcall.h>
#include <QtDBus/qdbuspendingreply.h>
#include <QtDBus/qdbusservicewatcher.h>
#include <QtGui/qbrush.h>
#include <QtGui/qclipboard.h>
#include <QtGui/qcolor.h>
#include <QtGui/qevent.h>
#include <QtGui/qfontmetrics.h>
#include <QtGui/qguiapplication.h>
#include <QtGui/qicon.h>
#include <QtGui/qpainter.h>
#include <QtGui/qpen.h>
#include <QtGui/qscreen.h>
#include <QtWidgets/qapplication.h>
#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qlayout.h>
#include <QtWidgets/qmenu.h>
#include <QtWidgets/qpushbutton.h>
#include <QtWidgets/qsizepolicy.h>
#include <QtWidgets/qstyle.h>
#include <QtWidgets/qsystemtrayicon.h>
#include <QtWidgets/qwidget.h>
#include <algorithm>
#include <array>
#include <string>

#include "common/asr/recognition_result.h"
#include "common/dbus/dbus_interface.h"

namespace vinput::gui {

CapsuleWindow::CapsuleWindow(QWidget* parent)
    : QWidget(parent), title_(new QLabel(this)),
      indicator_(new QLabel(QString::fromUtf8("●"), this)), preview_(new QLabel(this)),
      hint_(new QLabel(this)), copy_(new QPushButton(tr("Copy text"), this)),
      quit_(new QPushButton(QString::fromUtf8("×"), this)) {
  setWindowTitle(tr("Vinput Live Preview"));
  setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                 Qt::WindowDoesNotAcceptFocus);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);
  setFocusPolicy(Qt::NoFocus);
  resize(448, 92);
  setStyleSheet("QPushButton {color:#bec4cf;background:transparent;border:0;"
                "border-radius:6px;padding:3px 7px;font-size:11px;}"
                "QPushButton:hover {color:#ffffff;background:#343943;}"
                "QPushButton:pressed {background:#424956;}");
  title_->setObjectName("capsuleStatus");
  title_->setStyleSheet("color:#aeb6c5;font-size:12px;font-weight:500;");
  title_->setMinimumHeight(22);
  indicator_->setFixedWidth(10);
  preview_->setObjectName("capsuleText");
  preview_->setTextFormat(Qt::PlainText);
  preview_->setMinimumWidth(0);
  preview_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  preview_->setMinimumHeight(26);
  hint_->setWordWrap(true);
  hint_->setStyleSheet("color:#aeb6c5;font-size:12px;");
  copy_->setObjectName("capsuleCopy");
  copy_->setEnabled(false);
  copy_->setFocusPolicy(Qt::NoFocus);
  connect(copy_, &QPushButton::clicked, this, &CapsuleWindow::copyText);
  quit_->setFixedSize(24, 22);
  quit_->setToolTip(tr("Exit preview"));
  quit_->setAccessibleName(tr("Exit preview"));
  quit_->setFocusPolicy(Qt::NoFocus);
  connect(quit_, &QPushButton::clicked, qApp, &QApplication::quit);
  auto* row = new QHBoxLayout;
  row->setSpacing(7);
  row->addWidget(indicator_);
  row->addWidget(title_);
  row->addStretch();
  row->addWidget(copy_);
  row->addWidget(quit_);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 16, 24, 18);
  layout->setSpacing(8);
  layout->addLayout(row);
  layout->addWidget(preview_);
  layout->addWidget(hint_);
  hideTimer_.setSingleShot(true);
  hideTimer_.setInterval(1800);
  connect(&hideTimer_, &QTimer::timeout, this, [this] {
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
      hide();
    }
  });
  createTray();
}

void CapsuleWindow::createTray() {
  // Keep the icon registered even without a tray; Qt adds it automatically
  // if the desktop's system tray becomes available later.
  const QIcon icon = QIcon::fromTheme("audio-input-microphone",
                                      QApplication::style()->standardIcon(QStyle::SP_MediaVolume));
  auto* tray = new QSystemTrayIcon(icon, this);
  tray->setToolTip(tr("Vinput Live Preview"));
  auto* menu = new QMenu(this);
  menu->addAction(tr("Open settings"), this, [] {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList{});
  });
  menu->addAction(tr("Copy text"), this, &CapsuleWindow::copyText);
  menu->addAction(tr("Show preview"), this, [this] {
    present(tr("Vinput Live Preview"), tr("Use your voice-input shortcut to start recording."));
  });
  menu->addSeparator();
  menu->addAction(tr("Exit preview"), qApp, &QApplication::quit);
  tray->setContextMenu(menu);
  tray->show();
}

void CapsuleWindow::observe(QDBusConnection bus) {
  using namespace vinput::dbus;
  const bool status = bus.connect(kBusName, kObjectPath, kInterface, kSignalStatusChanged, this,
                                  SLOT(onStatus(QString)));
  const bool partial = bus.connect(kBusName, kObjectPath, kInterface, kSignalRecognitionPartial,
                                   this, SLOT(onPartial(QString)));
  const bool result = bus.connect(kBusName, kObjectPath, kInterface, kSignalRecognitionResult, this,
                                  SLOT(onResult(QString)));
  if (!status || !partial || !result) {
    showFailure(tr("Cannot observe voice input"));
    return;
  }
  // 2.4.2 emits this extra signal; newer daemons use desktop notifications instead.
  (void)bus.connect(kBusName, kObjectPath, kInterface, "DaemonNotification", this,
                    SLOT(onNotification(QString, QString, QString, QString)));
  auto* watcher =
      new QDBusServiceWatcher(kBusName, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this, bus](const QString&, const QString&, const QString& owner) {
            ++revision_;
            hideTimer_.stop();
            if (owner.isEmpty()) {
              showFailure(tr("Voice input disconnected"));
              return;
            }
            queryStatus(bus);
          });
  queryStatus(bus);
}

void CapsuleWindow::queryStatus(const QDBusConnection& bus) {
  using namespace vinput::dbus;
  auto message =
      QDBusMessage::createMethodCall(kBusName, kObjectPath, kInterface, kMethodGetStatus);
  message.setAutoStartService(false);
  const quint64 revision = revision_;
  auto* pending = new QDBusPendingCallWatcher(bus.asyncCall(message, 3000), this);
  connect(pending, &QDBusPendingCallWatcher::finished, this,
          [this, revision](QDBusPendingCallWatcher* call) {
            const QDBusPendingReply<QString> reply = *call;
            call->deleteLater();
            if (revision != revision_) {
              return; // A newer live signal takes precedence over the startup snapshot.
            }
            if (reply.isError()) {
              showFailure(tr("Voice input disconnected"));
              return;
            }
            onStatus(reply.value());
          });
}

void CapsuleWindow::onStatus(const QString& status) {
  using namespace vinput::dbus;
  const std::string value = status.toStdString();
  constexpr std::array known{kStatusIdle, kStatusRecording, kStatusInferring, kStatusPostprocessing,
                             kStatusError};
  if (std::ranges::find(known, value) == known.end()) {
    return;
  }
  ++revision_;
  const Status next = StringToStatus(value);
  switch (next) {
  case Status::Recording:
    if (phase_.has_value() && phase_ != Status::Recording) {
      text_.clear();
      resultReceived_ = false;
    }
    phase_ = next;
    present(tr("Listening"), tr("Use your voice-input shortcut to finish."));
    return;
  case Status::Inferring:
    phase_ = next;
    present(tr("Finalizing"), tr("Waiting for the final result; preview text may change."));
    return;
  case Status::Postprocessing:
    phase_ = next;
    present(tr("Rewriting"), tr("Waiting for the final result; preview text may change."));
    return;
  case Status::Error:
    showFailure(tr("Recognition failed"));
    return;
  case Status::Idle:
    if (phase_ == Status::Error) {
      return; // Preserve failed text until the next recording or explicit exit.
    }
    phase_ = next;
    if (resultReceived_) {
      hideTimer_.start();
      return;
    }
    if (!text_.isEmpty()) {
      // Current daemons use idle for both cancellation and some failures.
      // Preserve the hypothesis without claiming a result or a specific cause.
      present(tr("Session ended"), tr("No final result. You can copy the preview text."));
      return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
      present(tr("Ready"), tr("Use your voice-input shortcut to start recording."));
      return;
    }
    hide();
    return;
  }
}

void CapsuleWindow::onPartial(const QString& text) {
  if (text.isEmpty() || phase_ == vinput::dbus::Status::Idle ||
      phase_ == vinput::dbus::Status::Error) {
    return;
  }
  // Buffer startup partials until GetStatus establishes the phase. A partial
  // does not establish a new phase and must not invalidate that snapshot.
  // Partial is a cumulative hypothesis, not a delta.
  text_ = text;
  refreshText();
}

void CapsuleWindow::onResult(const QString& payload) {
  const auto result = vinput::result::Parse(payload.toStdString());
  const bool cancelled = std::ranges::any_of(result.candidates, [](const auto& candidate) {
    return candidate.source == vinput::result::kSourceCancel;
  });
  if (cancelled) {
    ++revision_;
    hideTimer_.stop();
    phase_ = vinput::dbus::Status::Idle;
    resultReceived_ = false;
    text_.clear();
    refreshText();
    onStatus(vinput::dbus::kStatusIdle);
    return;
  }
  if (result.commitText.empty()) {
    return;
  }
  ++revision_;
  resultReceived_ = true;
  text_ = QString::fromStdString(result.commitText);
  present(tr("Recognition result"),
          tr("Your input method handles insertion or candidate selection."));
  if (phase_ == vinput::dbus::Status::Idle) {
    hideTimer_.start();
  }
}

void CapsuleWindow::onNotification(const QString& /*code*/, const QString& /*subject*/,
                                   const QString& /*detail*/, const QString& /*raw*/) {
  if (phase_ != vinput::dbus::Status::Idle) {
    ++revision_;
    showFailure(tr("Recognition failed"));
  }
}

void CapsuleWindow::showFailure(const QString& title) {
  phase_ = vinput::dbus::Status::Error;
  present(title, tr("Preview text is kept for copying. See the system notification for details."));
}

void CapsuleWindow::present(const QString& title, const QString& hint) {
  hideTimer_.stop();
  title_->setText(title);
  indicator_->setStyleSheet(phase_ == vinput::dbus::Status::Error
                                ? "color:#efb87b;font-size:10px;"
                                : "color:#78d4b3;font-size:10px;");
  hint_->setText(hint);
  title_->setToolTip(hint);
  hint_->setVisible(phase_ == vinput::dbus::Status::Error ||
                    (phase_ == vinput::dbus::Status::Idle && !text_.isEmpty() && !resultReceived_));
  copy_->setVisible(!QSystemTrayIcon::isSystemTrayAvailable() || resultReceived_ ||
                    phase_ == vinput::dbus::Status::Error ||
                    (phase_ == vinput::dbus::Status::Idle && !text_.isEmpty()));
  quit_->setVisible(!QSystemTrayIcon::isSystemTrayAvailable());
  const auto* screen = QGuiApplication::primaryScreen();
  if (screen != nullptr) {
    const QRect area = screen->availableGeometry();
    resize(std::min(448, area.width()), height());
  }
  layout()->activate();
  const int contentHeight = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width())
                                                          : layout()->sizeHint().height();
  resize(width(), std::max(92, contentHeight));
  if (screen != nullptr) {
    const QRect area = screen->availableGeometry();
    move(area.center().x() - (width() / 2), area.bottom() - height() - 36);
  }
  refreshText();
  show(); // No activateWindow/raise: keep the target application's input focus.
}

void CapsuleWindow::refreshText() {
  copy_->setEnabled(!text_.isEmpty());
  preview_->setStyleSheet(text_.isEmpty() ? "color:#7e899b;font-size:17px;"
                                          : "color:#f1f3f7;font-size:17px;");
  const QString display = text_.isEmpty() ? tr("Speak to see a live preview.") : text_;
  preview_->setText(preview_->fontMetrics().elidedText(display, Qt::ElideLeft, preview_->width()));
  preview_->setToolTip(text_);
}

void CapsuleWindow::copyText() const {
  if (!text_.isEmpty()) {
    QGuiApplication::clipboard()->setText(text_);
  }
}

void CapsuleWindow::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(0, 0, 0, 28));
  painter.drawRoundedRect(rect().adjusted(3, 7, -3, -1), 23, 23);
  QLinearGradient surface(0, 0, 0, height());
  surface.setColorAt(0, QColor("#242932"));
  surface.setColorAt(1, QColor("#1b1f27"));
  painter.setBrush(surface);
  painter.setPen(QPen(QColor(255, 255, 255, 28), 1));
  painter.drawRoundedRect(rect().adjusted(6, 4, -6, -6), 19, 19);
}

void CapsuleWindow::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  refreshText();
}

} // namespace vinput::gui
