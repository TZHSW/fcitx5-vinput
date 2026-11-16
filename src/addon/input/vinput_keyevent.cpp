#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fcitx-utils/event.h>
#if __has_include(<fcitx-utils/eventloopinterface.h>)
#include <fcitx-utils/eventloopinterface.h>
#endif
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/keysymgen.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <iterator>
#include <memory>
#include <string>

#include "common/config/core_config.h"
#include "common/config/core_config_view.h"
#include "common/config/vinput_config.h"
#include "common/dbus/dbus_interface.h"
#include "common/scene/postprocess_scene.h"
#include "common/utils/debug_log.h"

#include "clipboard_public.h"
#include "core/vinput.h"

namespace {

constexpr auto kDefaultClock = 1; // POSIX CLOCK_MONOTONIC
constexpr auto kReleaseDebounce = std::chrono::milliseconds(500);
constexpr auto kDoubleTapGap = std::chrono::milliseconds(350);
constexpr auto kTriggerDebounce = std::chrono::milliseconds(80);

std::string NoSelectionPreeditText() {
  return _("Please select text first.");
}

std::string CommandDisabledPreeditText() {
  return _("Command mode is disabled (candidate count is 0).");
}
std::string CommandNoProviderPreeditText() {
  return _("No LLM provider configured for command mode.");
}
std::string DaemonUnavailablePreeditText() {
  return _("Voice input daemon is temporarily unavailable.");
}
std::string DaemonNotRespondingPreeditText() {
  return _("Voice input daemon is not responding.");
}

std::string ExtractSurroundingSelection(fcitx::InputContext* ic) {
  if (ic == nullptr) {
    return {};
  }
  auto& surrounding = ic->surroundingText();
  if (!surrounding.isValid() || surrounding.cursor() == surrounding.anchor()) {
    return {};
  }
  const auto& text = surrounding.text();
  if (!fcitx::utf8::validate(text)) {
    return {};
  }
  const auto char_from = std::min(surrounding.cursor(), surrounding.anchor());
  const auto char_to = std::max(surrounding.cursor(), surrounding.anchor());
  const auto byte_from = fcitx::utf8::ncharByteLength(text.begin(), char_from);
  const auto byte_len =
      fcitx::utf8::ncharByteLength(std::next(text.begin(), byte_from), char_to - char_from);
  return text.substr(byte_from, byte_len);
}

std::string ExtractClipboardPrimary(fcitx::Instance* instance, fcitx::InputContext* ic) {
  if (instance == nullptr || ic == nullptr) {
    return {};
  }
  auto* clipboard = instance->addonManager().addon("clipboard");
  if (clipboard == nullptr) {
    return {};
  }
  auto primary = clipboard->call<fcitx::IClipboard::primary>(ic);
  if (!fcitx::utf8::validate(primary)) {
    return {};
  }
  return primary;
}

std::string AcquireContextSelection(fcitx::Instance* instance, fcitx::InputContext* ic) {
  std::string selected_text = ExtractSurroundingSelection(ic);
  if (selected_text.empty()) {
    selected_text = ExtractClipboardPrimary(instance, ic);
  }
  return selected_text;
}

} // namespace

int VinputEngine::matchKeyListIndex(const fcitx::Key& event_key, const fcitx::KeyList& list,
                                    bool is_release) {
  const int idx = event_key.keyListIndex(list);
  if (idx >= 0) {
    return idx;
  }
  if (is_release && event_key.isModifier()) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (event_key.isReleaseOfModifier(list[i])) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

bool VinputEngine::handleDaemonRecordingPrecondition(fcitx::InputContext* ic,
                                                     const fcitx::Key& trigger, bool is_command) {
  const std::string daemon_status = last_known_daemon_status_;
  if (!is_command && !session_ && daemon_status == vinput::dbus::kStatusRecording) {
    enterRecordingState(ic, trigger, false);
    finishStopRecording();
    return true;
  }
  if (!daemon_status.empty() && daemon_status != vinput::dbus::kStatusIdle) {
    applyDaemonStatusLocally(daemon_status, ic, is_command);
    return true;
  }
  return false;
}

bool VinputEngine::validateCommandModePrerequisites(fcitx::InputContext* ic) {
  auto core_config = LoadCoreConfig();
  const CoreConfigView view(core_config);
  const auto* cmd_scene = view.CommandScene();
  if (cmd_scene == nullptr || cmd_scene->llm_max_candidates <= 0) {
    finishFrontendSession(ic);
    updateVoicePresentation(ic, CommandDisabledPreeditText());
    return false;
  }
  if (cmd_scene->provider_id.empty() || (view.LlmProvider(cmd_scene->provider_id) == nullptr &&
                                         view.LlmAdapter(cmd_scene->provider_id) == nullptr)) {
    finishFrontendSession(ic);
    updateVoicePresentation(ic, CommandNoProviderPreeditText());
    return false;
  }
  return true;
}

void VinputEngine::handleRecordingStartFailure(fcitx::InputContext* ic, bool is_command) {
  finishFrontendSession(ic);
  if (bus_ == nullptr) {
    VLOG_DEBUG("{} trigger fallback: daemon bus unavailable", is_command ? "command" : "record");
    updateVoicePresentation(ic, DaemonUnavailablePreeditText());
    return;
  }
  if (!daemonSyncAllowed()) {
    VLOG_DEBUG("{} trigger fallback: daemon sync throttled after timeout/failure",
               is_command ? "command" : "record");
    updateVoicePresentation(ic, DaemonNotRespondingPreeditText());
  }
}

void VinputEngine::dispatchRecordingStart(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                          bool is_command, const std::string& selected_text) {
  enterPendingStartState(ic, trigger, is_command);
  if (is_command) {
    FCITX_LOG(Debug) << "vinput: command key activated, selected_text length="
                     << selected_text.size();
    if (!callStartCommandRecording(selected_text)) {
      handleRecordingStartFailure(ic, true);
    }
    return;
  }

  FCITX_LOG(Debug) << "vinput: trigger key activated";
  if (!callStartRecording()) {
    handleRecordingStartFailure(ic, false);
  }
}

void VinputEngine::startVoiceRecording(fcitx::InputContext* ic, const fcitx::Key& trigger,
                                       bool is_command) {
  if (ic == nullptr) {
    return;
  }
  dismissMenusForVoiceActivity();
  cancelPendingStop();

  if (handleDaemonRecordingPrecondition(ic, trigger, is_command)) {
    return;
  }

  std::string selected_text;
  if (is_command) {
    if (!validateCommandModePrerequisites(ic)) {
      return;
    }
    selected_text = AcquireContextSelection(instance_, ic);
    if (selected_text.empty()) {
      if (status_ic_ == ic) {
        finishFrontendSession(ic);
      } else {
        clearVoicePresentation(ic);
      }
      VLOG_DEBUG("command trigger ignored because no selection text is available");
      updateVoicePresentation(ic, NoSelectionPreeditText());
      return;
    }
  }

  dispatchRecordingStart(ic, trigger, is_command, selected_text);
}

bool VinputEngine::handleVoiceControlKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (pending_voice_control_release_ && keyEvent.isRelease() &&
      keyEvent.key().normalize().sym() == pending_voice_control_release_->normalize().sym()) {
    pending_voice_control_release_.reset();
    keyEvent.filterAndAccept();
    return true;
  }
  if (!session_ || session_->phase != Session::Phase::Postprocessing ||
      last_known_daemon_status_ != vinput::dbus::kStatusPostprocessing) {
    return false;
  }
  const bool commit_raw =
      (session_->raw_prev && !session_->command_mode) &&
      (keyEvent.key().check(FcitxKey_Return) || keyEvent.key().check(FcitxKey_KP_Enter));

  if (!commit_raw) {
    return false;
  }

  if (!keyEvent.isRelease()) {
    pending_voice_control_release_ = keyEvent.key();
    callCancelOperation(true);
  }
  keyEvent.filterAndAccept();
  return true;
}

bool VinputEngine::handleMenuNavigationKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (result_menu_visible_ && handleResultMenuKeyEvent(keyEvent)) {
    return true;
  }
  if (palette_menu_visible_ && handlePaletteMenuKeyEvent(keyEvent)) {
    return true;
  }
  return false;
}

VinputEngine::HotkeyMatch VinputEngine::classifyHotkey(const fcitx::Key& event_key,
                                                       bool is_release) const {
  struct HotkeyBinding {
    HotkeyRole role = HotkeyRole::None;
    const fcitx::KeyList* keys = nullptr;
    bool is_command = false;
  };

  const std::array bindings = {
      HotkeyBinding{.role = HotkeyRole::Trigger, .keys = &trigger_keys_, .is_command = false},
      HotkeyBinding{.role = HotkeyRole::Command, .keys = &command_keys_, .is_command = true},
      HotkeyBinding{.role = HotkeyRole::Menu, .keys = &menu_keys_, .is_command = false},
  };

  for (const auto& binding : bindings) {
    const int index = matchKeyListIndex(event_key, *binding.keys, is_release);
    if (index >= 0) {
      return {.role = binding.role,
              .matched_key = (*binding.keys)[index],
              .is_command = binding.is_command};
    }
  }

  return {.role = HotkeyRole::None, .matched_key = fcitx::Key(), .is_command = false};
}

void VinputEngine::handleNonHotkeyKeyEvent(fcitx::KeyEvent& keyEvent) {
  if (keyEvent.isRelease() || held_role_ == HotkeyRole::None) {
    return;
  }

  chord_interrupted_ = true;
  cancelPendingStart();

  if (session_ && session_->stop_on_release && !session_->trigger_released) {
    auto* target_ic = session_->ic;
    callCancelOperation(false);
    finishFrontendSession(target_ic);
    clearVoicePresentation(target_ic);
  }
}

bool VinputEngine::handleActiveSessionHotkeyDown(const fcitx::Key& matched_key) {
  if (!session_ || (session_->phase != Session::Phase::Recording &&
                    session_->phase != Session::Phase::PendingStart)) {
    return false;
  }

  if (session_->capture_action != Session::CaptureAction::Continue) {
    return true;
  }
  if (session_->trigger != matched_key) {
    auto* target_ic = session_->ic;
    callCancelOperation(false);
    finishFrontendSession(target_ic);
    clearVoicePresentation(target_ic);
    return true;
  }

  if (session_->trigger_released) {
    if (session_->phase == Session::Phase::PendingStart) {
      session_->capture_action = Session::CaptureAction::Confirm;
    } else {
      finishStopRecording();
    }
  }
  return true;
}

bool VinputEngine::handleDictationGestureKey(fcitx::KeyEvent& event, const HotkeyMatch& match) {
  auto* ic = event.inputContext();
  if (held_role_ == HotkeyRole::Trigger && pending_start_ic_.get() == ic) {
    // The physical trigger still owns its release after modifier state changes.
    if (event.isRelease() && held_key_sym_ == event.origKey().sym()) {
      const auto trigger = pending_start_trigger_;
      releaseDictationGesture(ic, trigger);
      return true;
    }
    if (match.role == HotkeyRole::Trigger) {
      return true; // Ignore repeats and overlapping triggers until our release.
    }
    if (event.isRelease() && event.origKey().isModifier() && !isHoldRecording()) {
      cancelPendingStart();
    }
  }
  if (match.role != HotkeyRole::Trigger) {
    return false;
  }
  if (!event.isRelease() && !event.origKey().states().test(fcitx::KeyState::Repeat)) {
    beginDictationGesture(ic, match.matched_key);
    held_key_sym_ = event.origKey().sym();
  }
  return true;
}

// A short first tap only arms the pair. The timer is the only path that starts
// hold-to-talk, so releasing a single tap never opens the microphone.
void VinputEngine::beginDictationGesture(fcitx::InputContext* ic, const fcitx::Key& key) {
  const auto now = std::chrono::steady_clock::now();
  if (pending_start_ic_.get() != ic || pending_start_trigger_ != key ||
      (first_tap_release_ && now - *first_tap_release_ > kDoubleTapGap)) {
    cancelPendingStart();
  }
  pending_start_ic_ = ic->watch();
  pending_start_trigger_ = key;
  held_role_ = HotkeyRole::Trigger;
  held_press_time_ = now;
  const auto deadline =
      fcitx::now(kDefaultClock) +
      std::chrono::duration_cast<std::chrono::microseconds>(kTapMaxHoldDuration).count();
  // A zero accuracy lets the system event loop coalesce timers past key release.
  constexpr uint64_t kHoldAccuracyUsec = 1000;
  pending_start_event_ = instance_->eventLoop().addTimeEvent(
      kDefaultClock, deadline, kHoldAccuracyUsec, [this](auto*, uint64_t) {
        startHeldDictation();
        return false;
      });
  pending_start_event_->setOneShot();
}

void VinputEngine::startHeldDictation() {
  first_tap_release_.reset();
  auto* ic = pending_start_ic_.get();
  if (ic == nullptr || session_ || last_known_daemon_status_ == vinput::dbus::kStatusRecording) {
    return;
  }
  startVoiceRecording(ic, pending_start_trigger_, false);
  if (session_ && session_->phase == Session::Phase::PendingStart) {
    session_->stop_on_release = true;
    session_->recording_gesture = vinput::dbus::kHoldGesture;
  }
}

void VinputEngine::toggleDictationGesture(fcitx::InputContext* ic, const fcitx::Key& key) {
  if (!session_) {
    if (last_known_daemon_status_ != vinput::dbus::kStatusRecording) {
      handleTapReleaseMode(ic, {.role = HotkeyRole::Trigger, .matched_key = key},
                           std::chrono::steady_clock::now());
      if (session_ && session_->phase == Session::Phase::PendingStart) {
        session_->recording_gesture = vinput::dbus::kDoubleTapGesture;
      }
    }
    return;
  }
  if (session_->trigger == key && session_->capture_action == Session::CaptureAction::Continue) {
    handleActiveSessionHotkeyDown(key);
  }
}

void VinputEngine::releaseDictationGesture(fcitx::InputContext* ic, const fcitx::Key& key) {
  if (held_role_ != HotkeyRole::Trigger || pending_start_trigger_ != key ||
      pending_start_ic_.get() != ic) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const bool held =
      now - held_press_time_ >= kTapMaxHoldDuration || !pending_start_event_->isEnabled();
  pending_start_event_->setEnabled(false);
  held_role_ = HotkeyRole::None;
  if (held) {
    cancelPendingStart();
    if (session_ && session_->stop_on_release && session_->trigger == key) {
      session_->stop_on_release = false;
      session_->trigger_released = true;
      handleActiveSessionHotkeyDown(key);
    }
    return;
  }
  if (!first_tap_release_) {
    first_tap_release_ = now;
    return;
  }
  cancelPendingStart();
  toggleDictationGesture(ic, key);
}

void VinputEngine::handleHotkeyDown(fcitx::KeyEvent& keyEvent, fcitx::InputContext* ic,
                                    const HotkeyMatch& match) {
  const auto now = std::chrono::steady_clock::now();

  if (match.role == HotkeyRole::Trigger || match.role == HotkeyRole::Command) {
    const auto since_last = now - last_trigger_time_;
    last_trigger_time_ = now;
    if (since_last < kTriggerDebounce) {
      keyEvent.filterAndAccept();
      return;
    }

    dismissMenusForVoiceActivity();
    cancelPendingStop();

    if (handleActiveSessionHotkeyDown(match.matched_key)) {
      keyEvent.filterAndAccept();
      return;
    }
  }

  held_key_sym_ = keyEvent.origKey().normalize().sym();
  held_role_ = match.role;
  chord_interrupted_ = false;
  held_press_time_ = now;

  if (match.role == HotkeyRole::Trigger || match.role == HotkeyRole::Command) {
    const auto trigger_mode = *config_.triggerMode;
    if (trigger_mode == TriggerMode::Hold || trigger_mode == TriggerMode::Both) {
      startVoiceRecording(ic, match.matched_key, match.is_command);
      if (session_) {
        session_->press_time = now;
        session_->stop_on_release = true;
        session_->trigger_released = false;
      }
    }
  }

  keyEvent.filterAndAccept();
}

void VinputEngine::handleTapReleaseMode(fcitx::InputContext* ic, const HotkeyMatch& match,
                                        std::chrono::steady_clock::time_point now) {
  if (!session_) {
    startVoiceRecording(ic, match.matched_key, match.is_command);
    if (session_) {
      session_->press_time = now;
      session_->trigger_released = true;
      session_->stop_on_release = false;
    }
    return;
  }
  if (session_->phase == Session::Phase::Recording) {
    scheduleStopRecording();
  }
}

void VinputEngine::handleTriggerReleaseMode(fcitx::InputContext* ic, const HotkeyMatch& match,
                                            std::chrono::steady_clock::duration duration,
                                            std::chrono::steady_clock::time_point now) {
  const auto mode = *config_.triggerMode;
  const auto action =
      EvaluateTapHoldTiming(mode == TriggerMode::HoldDoubleTap ? TriggerMode::Tap : mode, duration);
  if (action == TapHoldAction::HoldStop) {
    if (session_ && session_->phase == Session::Phase::Recording) {
      scheduleStopRecording();
    }
    return;
  }

  if (action == TapHoldAction::TapToggle) {
    handleTapReleaseMode(ic, match, now);
    return;
  }

  // TapHoldAction::BothTapLock
  if (session_) {
    session_->trigger_released = true;
    session_->stop_on_release = false;
  }
}

void VinputEngine::handleActiveHeldHotkeyRelease(fcitx::InputContext* ic, const HotkeyMatch& match,
                                                 HotkeyRole active_role) {
  if (active_role == HotkeyRole::Menu) {
    if (!session_) {
      toggleCommandPalette(ic);
    }
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  handleTriggerReleaseMode(ic, match, now - held_press_time_, now);
}

void VinputEngine::handleHotkeyUp(fcitx::KeyEvent& keyEvent, fcitx::InputContext* ic,
                                  const HotkeyMatch& match) {
  const auto event_key = keyEvent.origKey().normalize();
  const bool matches_active =
      (held_role_ == match.role && held_key_sym_.has_value() && *held_key_sym_ == event_key.sym());

  if (matches_active) {
    const auto active_role = held_role_;
    held_role_ = HotkeyRole::None;
    held_key_sym_.reset();

    if (chord_interrupted_) {
      chord_interrupted_ = false;
      cancelPendingStart();
      keyEvent.filterAndAccept();
      return;
    }

    handleActiveHeldHotkeyRelease(ic, match, active_role);
  }

  if (session_ && session_->trigger == match.matched_key) {
    session_->trigger_released = true;
    if (session_->stop_on_release && session_->phase == Session::Phase::Recording) {
      scheduleStopRecording();
    }
  }

  keyEvent.filterAndAccept();
}

bool VinputEngine::passThroughVoiceEscape(const fcitx::KeyEvent& keyEvent,
                                          const HotkeyMatch& hotkey_match) {
  // Leave both edges to the application. In particular, Alt+Escape during a
  // hold must not enter the generic chord-interruption path below.
  if (hotkey_match.role == HotkeyRole::None && keyEvent.origKey().sym() == FcitxKey_Escape &&
      (session_ || isPendingStart() || first_tap_release_)) {
    if (held_role_ == HotkeyRole::None && first_tap_release_) {
      cancelPendingStart();
    }
    return true;
  }
  return false;
}

void VinputEngine::handleKeyEvent(fcitx::Event& event) {
  auto& keyEvent = dynamic_cast<fcitx::KeyEvent&>(event);
  auto* ic = keyEvent.inputContext();
  rememberInputContext(ic);

  const auto event_key = keyEvent.origKey().normalize();
  const auto hotkey_match = classifyHotkey(event_key, keyEvent.isRelease());
  if (trigger_mode_ == TriggerMode::HoldDoubleTap &&
      handleDictationGestureKey(keyEvent, hotkey_match)) {
    keyEvent.filterAndAccept();
    return;
  }
  if (passThroughVoiceEscape(keyEvent, hotkey_match)) {
    return;
  }
  if (handleVoiceControlKeyEvent(keyEvent)) {
    return;
  }
  if (pending_start_ic_.get() != nullptr && !keyEvent.isRelease()) {
    if (isHoldRecording()) {
      discardRecording();
    } else {
      cancelPendingStart();
    }
  }
  if (handleMenuNavigationKeyEvent(keyEvent)) {
    return;
  }

  if (hotkey_match.role == HotkeyRole::None) {
    handleNonHotkeyKeyEvent(keyEvent);
    return;
  }

  if (!keyEvent.isRelease()) {
    handleHotkeyDown(keyEvent, ic, hotkey_match);
    return;
  }

  handleHotkeyUp(keyEvent, ic, hotkey_match);
}

void VinputEngine::toggleCommandPalette(fcitx::InputContext* ic) {
  if (palette_menu_visible_) {
    hidePaletteMenu();
    return;
  }
  if (session_) {
    return;
  }
  if (pending_start_event_ && pending_start_event_->isEnabled()) {
    return;
  }
  showPaletteMenu(ic);
}

bool VinputEngine::isPendingStartTrigger(const fcitx::Key& trigger) const {
  return pending_start_event_ && pending_start_event_->isEnabled() &&
         pending_start_trigger_ == trigger;
}

void VinputEngine::cancelPendingStop() {
  if (pending_stop_event_ && pending_stop_event_->isEnabled()) {
    pending_stop_event_->setEnabled(false);
  }
}

void VinputEngine::cancelPendingStart() {
  first_tap_release_.reset();
  if (pending_start_event_ && pending_start_event_->isEnabled()) {
    pending_start_event_->setEnabled(false);
  }
  pending_start_trigger_ = fcitx::Key();
  pending_start_ic_.unwatch();
  held_role_ = HotkeyRole::None;
  held_key_sym_.reset();
}

void VinputEngine::scheduleStopRecording() {
  const auto fire_at_usec =
      fcitx::now(kDefaultClock) +
      static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(kReleaseDebounce).count());

  if (!pending_stop_event_) {
    const std::weak_ptr<bool> token = lifetime_token_;
    pending_stop_event_ = instance_->eventLoop().addTimeEvent(kDefaultClock, fire_at_usec, 0,
                                                              [this, token](auto*, uint64_t) {
                                                                if (token.expired()) {
                                                                  return false;
                                                                }
                                                                finishStopRecording();
                                                                return false;
                                                              });
    pending_stop_event_->setOneShot();
    return;
  }

  pending_stop_event_->setTime(fire_at_usec);
  pending_stop_event_->setEnabled(true);
}

void VinputEngine::finishStopRecording() {
  if (!session_.has_value() || session_->phase != Session::Phase::Recording) {
    return;
  }

  reloadSceneConfig();
  if (!session_.has_value()) {
    return;
  }
  const auto& scene = vinput::scene::Resolve(scene_config_, active_scene_id_);
  active_scene_id_ = scene.id;
  session_->raw_prev = scene.raw_prev;
  session_->trigger = fcitx::Key();
  enterBusyState(session_->ic, session_->command_mode, _("... Recognizing ..."), false,
                 scene.raw_prev);
  callStopRecording(scene.id);
}
