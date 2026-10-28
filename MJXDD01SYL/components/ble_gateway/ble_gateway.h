#pragma once

#include "esphome/components/ble_device_base/ble_hub.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/socket/socket.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace ble_gateway {

// Relays BLE advertisements to one LAN address. A YAML receiver is the cold-boot
// default: absent leaves the relay off, and enabled turns it on. A stored
// receiver replaces that default. Each datagram is authenticated with the
// 16-byte key. The key is not logged.
class BleGateway : public Component {
 public:
  void set_hub(esp32_ble_tracker::ESP32BLETracker *hub) { this->hub_ = hub; }
  void set_receiver(const char *host, uint16_t port, bool enable);
  void set_default_key(const char *hex);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return -940.0f; }

  void configure(const char *host, size_t host_len, int port, bool enable);
  void set_key(const char *key, size_t key_len);

 protected:
  struct Stored {
    uint8_t enable;
    uint8_t has_key;
    uint8_t ip[4];
    uint8_t key[16];
    uint16_t port;
  };

  void on_advertisement_(const ble_device_base::RawAdvertisement &adv);
  bool recent_(uint64_t address, uint32_t hash, uint32_t now);
  void send_(const ble_device_base::RawAdvertisement &adv);
  bool save_();
  void open_socket_();

  esp32_ble_tracker::ESP32BLETracker *hub_{nullptr};
  ESPPreferenceObject pref_{};
  std::unique_ptr<socket::Socket> sock_{};
  struct sockaddr_in dest_ {};
  uint8_t ip_[4]{};
  uint8_t key_[16]{};
  uint16_t port_{0};
  uint32_t boot_id_{0};
  uint32_t counter_{0};
  uint32_t window_start_{0};
  uint32_t report_at_{0};
  uint16_t window_count_{0};
  uint32_t dropped_{0};
  uint32_t reported_drops_{0};
  bool enabled_{false};
  bool has_key_{false};
  bool socket_failed_{false};

  struct Seen {
    uint64_t address{0};
    uint32_t hash{0};
    uint32_t at{0};
  };
  Seen seen_[16]{};
  uint8_t seen_pos_{0};
};

}  // namespace ble_gateway
}  // namespace esphome
