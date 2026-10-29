#include "power_loss.h"

#include "esphome/core/log.h"

namespace esphome {
namespace homekit {

static const char *const TAG = "power_loss";

bool RestoreAfterPowerLoss::enabled_{true};
ESPPreferenceObject RestoreAfterPowerLoss::pref_{};
std::vector<light::LightState *> *RestoreAfterPowerLoss::lights_{nullptr};

void RestoreAfterPowerLoss::gate_(light::LightStateRTCState &state, bool restored) {
  if (!enabled_)
    state.state = false;
  (void) restored;
}

void RestoreAfterPowerLoss::setup() {
  this->pref_ = global_preferences->make_preference<bool>(fnv1_hash("kitchen_restore_after_power_loss"));
  bool stored = this->default_enabled_;
  if (!this->pref_.load(&stored))
    stored = this->default_enabled_;
  enabled_ = stored;
  lights_ = &this->lights_storage_;
  for (light::LightState *light : this->lights_storage_)
    light->set_state_callback(gate_);
  ESP_LOGCONFIG(TAG, "Restore after power loss: %s", enabled_ ? "ON" : "OFF");
}

void RestoreAfterPowerLoss::set_enabled(bool enabled) {
  enabled_ = enabled;
  if (pref_.save(&enabled_))
    global_preferences->sync();
  // Snapshot the current values once saving is turned back on. perform()
  // with no other fields keeps the lamps as they are.
  if (!enabled || lights_ == nullptr)
    return;
  for (light::LightState *light : *lights_)
    light->make_call().set_publish(false).set_save(true).perform();
}

}  // namespace homekit
}  // namespace esphome
