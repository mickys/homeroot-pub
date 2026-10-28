#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/light/light_state.h"

namespace esphome {
namespace homekit {

// Runtime copy of restore_after_power_loss. The YAML value is only the
// default used until the HomeKit switch is written. The switch value is
// kept in flash and read before the lights restore.
class RestoreAfterPowerLoss : public Component {
 public:
  void set_default_enabled(bool enabled) { this->default_enabled_ = enabled; }
  void add_light(light::LightState *light) { this->lights_storage_.push_back(light); }
  float get_setup_priority() const override { return setup_priority::HARDWARE + 1.0f; }
  void setup() override;

  static bool is_enabled() { return enabled_; }
  static void set_enabled(bool enabled);

 protected:
  static void gate_(light::LightStateRTCState &state, bool restored);

  static bool enabled_;
  static ESPPreferenceObject pref_;
  static std::vector<light::LightState *> *lights_;
  bool default_enabled_{true};
  std::vector<light::LightState *> lights_storage_;
};

}  // namespace homekit
}  // namespace esphome
