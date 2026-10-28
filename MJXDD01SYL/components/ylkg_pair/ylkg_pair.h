#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/light/light_state.h"

namespace esphome {
namespace text_sensor {
class TextSensor;
}  // namespace text_sensor
namespace ylkg_pair {

// Yeelight dimmer beacon-key handshake (service FE95). The configured
// product id selects which pairing advert is accepted. A remote call opens
// the window. Boot does not. The 12-byte key, MAC, and product id stay in
// flash. The key is also kept in RAM and on the text sensor. It is not logged.
// set_remote() stores a MAC, a 12-byte key, and an optional product id.
// The lamp answers that MAC only when both the MAC and the key are stored.
// With the key, a click toggles the active channel, a double click swaps
// the ceiling and night channels, and rotation dims.
class YlkgPair : public Component,
                 public ble_client::BLEClientNode,
                 public ble_device_base::ESPBTDeviceListener {
 public:
  YlkgPair();
  void set_product_id(uint16_t product_id) { this->product_id_ = product_id; }
  void set_key_sensor(text_sensor::TextSensor *sensor) { this->key_sensor_ = sensor; }
  void set_lights(light::LightState *ceiling, light::LightState *night) {
    this->ceiling_ = ceiling;
    this->night_ = night;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return -950.0f; }

  bool parse_device(const ble_device_base::ESPBTDevice &device) override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  static void open();
  static void close();
  static void set_reflect(void (*fn)(bool on));
  // mac and key are hex, with optional ':' '-' or spaces. product_id 0 keeps
  // the current product. Empty or invalid values are not stored.
  void set_remote(const char *mac, size_t mac_len, const char *key, size_t key_len, int product_id);

 protected:
  enum class Phase : uint8_t {
    IDLE = 0,
    CONNECTING,
    WAIT_DISCOVERY,
    WAIT_LOOKUP,
    WAIT_CCCD,
    WAIT_AUTH_INIT,
    WAIT_CHALLENGE,
    WAIT_NOTIFY,
    WAIT_KEY2,
    WAIT_BEACON,
    WAIT_FW,
    FINISH,
  };

  struct BindRecord {
    uint8_t len;
    uint8_t key[12];
    uint8_t mac[6];
    uint16_t product_id;
  };

  static YlkgPair *instance_;

  void publish_switch_();
  void end_window_();
  void stop_(const char *reason);
  void zero_work_();
  void publish_hex_(const uint8_t key[12]);
  void store_();
  bool self_check_() const;
  bool gesture_self_check_() const;
  void on_control_(const ble_device_base::ESPBTDevice &device);
  void apply_gesture_(uint8_t button_type, uint8_t value, uint8_t press);
  bool write_at_(uint16_t handle, uint16_t len);
  bool write_descr_(uint16_t handle, uint16_t len);
  bool read_at_(uint16_t handle);
  void begin_known_();
  void on_services_();
  void on_auth_notify_();
  void reversed_mac_(uint8_t out[6]) const;
  void mix_a_(const uint8_t mac[6], uint8_t out[8]) const;
  void mix_b_(const uint8_t mac[6], uint8_t out[8]) const;

  text_sensor::TextSensor *key_sensor_{nullptr};
  light::LightState *ceiling_{nullptr};
  light::LightState *night_{nullptr};
  ESPPreferenceObject pref_{};
  uint16_t product_id_{950};
  Phase phase_{Phase::IDLE};
  bool broken_{false};
  bool gesture_broken_{false};
  bool has_key_{false};
  bool address_from_store_{false};
  bool have_frame_{false};
  uint8_t last_frame_{0};
  uint8_t selected_{0};
  uint32_t last_toggle_ms_{0};
  uint32_t last_swap_ms_{0};
  uint8_t bindkey_[12]{};
  bool key_ready_{false};
  bool want_switch_{false};
  bool switch_dirty_{false};
  bool window_logged_{false};
  bool cancel_{false};
  uint32_t window_until_{0};
  uint32_t next_attempt_at_{0};
  uint32_t attempt_started_{0};
  uint32_t rediscover_at_{0};
  bool saw_discovery_{false};
  bool refreshed_{false};
  bool known_used_{false};
  uint16_t handle_auth_{0};
  uint16_t handle_auth_init_{0};
  uint16_t handle_beacon_{0};
  uint16_t handle_fw_{0};
  uint8_t notify_len_{0};
  uint8_t token_[12]{};
  uint8_t scratch_[16]{};
  uint8_t notify_[20]{};
  uint8_t key_[12]{};
  char firmware_[16]{};
};

}  // namespace ylkg_pair
}  // namespace esphome
