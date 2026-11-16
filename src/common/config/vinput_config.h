#pragma once

#include <cstdint>
#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/option.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

inline constexpr const char* kVinputConfigPath = "conf/vinput.conf";

enum class TriggerMode : std::uint8_t { Tap, Hold, Both, HoldDoubleTap };
FCITX_CONFIG_ENUM_NAME_WITH_I18N(TriggerMode, N_("Tap"), N_("Hold"), N_("Both"),
                                 N_("HoldDoubleTap"))

class VinputConfig : public fcitx::Configuration {
public:
  VinputConfig();
  VinputConfig(const VinputConfig&) = delete;
  VinputConfig& operator=(const VinputConfig&) = delete;

  ~VinputConfig() override = default;
  VinputConfig(VinputConfig&&) = delete;
  VinputConfig& operator=(VinputConfig&&) = delete;

  const char* typeName() const override { return "VinputConfig"; }

  fcitx::Option<TriggerMode, fcitx::NoConstrain<TriggerMode>, fcitx::DefaultMarshaller<TriggerMode>,
                TriggerModeI18NAnnotation>
      triggerMode;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      triggerKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      commandKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      menuKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      pagePrevKeys;

  fcitx::Option<fcitx::KeyList, fcitx::ListConstrain<fcitx::KeyConstrain>,
                fcitx::DefaultMarshaller<fcitx::KeyList>, fcitx::ToolTipAnnotation>
      pageNextKeys;

  fcitx::Option<int, fcitx::IntConstrain, fcitx::DefaultMarshaller<int>, fcitx::ToolTipAnnotation>
      maxStreamingDisplayWidth;

  fcitx::ExternalOption modelManager;
};
