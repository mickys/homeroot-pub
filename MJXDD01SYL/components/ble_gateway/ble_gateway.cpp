#include "ble_gateway.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <mbedtls/md.h>
#include <esp_random.h>

#include <cstring>

namespace esphome {
namespace ble_gateway {

static const char *const TAG = "ble_gateway";

static constexpr uint8_t VERSION = 1;
static constexpr size_t KEY_LEN = 16;
static constexpr size_t TAG_LEN = 16;
static constexpr size_t MAX_ADV = 96;
static constexpr size_t HEAD_LEN = 19;
static constexpr uint16_t MAX_PER_SEC = 40;
static constexpr uint32_t DEDUP_MS = 300;

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static bool parse_hex(const char *in, size_t len, uint8_t *out, size_t nbytes, size_t *digits) {
  size_t got = 0;
  size_t count = 0;
  int hi = -1;
  bool ok = in != nullptr || len == 0;
  for (size_t i = 0; in != nullptr && i < len; i++) {
    char c = in[i];
    if (c == ':' || c == '-' || c == ' ') {
      if (hi >= 0)
        ok = false;
      continue;
    }
    int n = hex_nibble(c);
    if (n < 0) {
      ok = false;
      continue;
    }
    count++;
    if (hi < 0) {
      hi = n;
      continue;
    }
    if (got < nbytes)
      out[got] = (uint8_t) ((hi << 4) | n);
    got++;
    hi = -1;
  }
  if (hi >= 0)
    ok = false;
  if (got != nbytes)
    ok = false;
  if (digits != nullptr)
    *digits = count;
  return ok;
}

static bool parse_ipv4(const char *in, size_t len, uint8_t out[4]) {
  if (in == nullptr)
    return false;
  size_t i = 0;
  for (int octet = 0; octet < 4; octet++) {
    if (i >= len || in[i] < '0' || in[i] > '9')
      return false;
    int value = 0;
    int digits = 0;
    while (i < len && in[i] >= '0' && in[i] <= '9') {
      value = value * 10 + (in[i] - '0');
      digits++;
      if (value > 255 || digits > 3)
        return false;
      i++;
    }
    if (digits == 0)
      return false;
    out[octet] = (uint8_t) value;
    if (octet < 3) {
      if (i >= len || in[i] != '.')
        return false;
      i++;
    }
  }
  return i == len;
}

static void put_be32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t) (value >> 24);
  out[1] = (uint8_t) (value >> 16);
  out[2] = (uint8_t) (value >> 8);
  out[3] = (uint8_t) value;
}

static void put_be16(uint8_t *out, uint16_t value) {
  out[0] = (uint8_t) (value >> 8);
  out[1] = (uint8_t) value;
}

void BleGateway::set_receiver(const char *host, uint16_t port, bool enable) {
  uint8_t ip[4]{};
  if (host == nullptr || !parse_ipv4(host, strlen(host), ip) || port == 0)
    return;
  memcpy(this->ip_, ip, sizeof(this->ip_));
  this->port_ = port;
  this->enabled_ = enable;
}

void BleGateway::set_default_key(const char *hex) {
  uint8_t key[KEY_LEN]{};
  size_t digits = 0;
  if (hex == nullptr || !parse_hex(hex, strlen(hex), key, sizeof(key), &digits)) {
    memset(key, 0, sizeof(key));
    return;
  }
  memcpy(this->key_, key, sizeof(this->key_));
  this->has_key_ = true;
  memset(key, 0, sizeof(key));
}

void BleGateway::setup() {
  this->pref_ = global_preferences->make_preference<Stored>(fnv1_hash("kitchen_ble_gateway"));
  Stored loaded{};
  if (this->pref_.load(&loaded)) {
    memcpy(this->ip_, loaded.ip, sizeof(this->ip_));
    this->port_ = loaded.port;
    this->enabled_ = loaded.enable != 0 && this->port_ != 0;
    this->has_key_ = loaded.has_key != 0;
    if (this->has_key_)
      memcpy(this->key_, loaded.key, sizeof(this->key_));
  }
  memset(&loaded, 0, sizeof(loaded));
  this->boot_id_ = esp_random();
  if (this->boot_id_ == 0)
    this->boot_id_ = 1;
  this->counter_ = 0;
  this->open_socket_();
  if (this->hub_ != nullptr) {
    this->hub_->set_raw_advertisement_callback(
        {this, [](void *self, const ble_device_base::RawAdvertisement &adv) {
           static_cast<BleGateway *>(self)->on_advertisement_(adv);
         }});
  }
  if (!this->enabled_)
    ESP_LOGI(TAG, "Gateway off");
  else if (!this->has_key_)
    ESP_LOGI(TAG, "Gateway waiting for a key");
  else
    ESP_LOGI(TAG, "Gateway on, %u.%u.%u.%u:%u", this->ip_[0], this->ip_[1], this->ip_[2], this->ip_[3], this->port_);
}

void BleGateway::dump_config() {
  ESP_LOGCONFIG(TAG, "BLE gateway: %s, key %s, %u.%u.%u.%u:%u", this->enabled_ ? "on" : "off",
                this->has_key_ ? "stored" : "absent", this->ip_[0], this->ip_[1], this->ip_[2], this->ip_[3],
                this->port_);
}

void BleGateway::loop() {
  uint32_t now = millis();
  if (this->dropped_ != this->reported_drops_ && (int32_t) (now - this->report_at_) >= 0) {
    ESP_LOGW(TAG, "Gateway dropped %u", this->dropped_);
    this->reported_drops_ = this->dropped_;
    this->report_at_ = now + 60000;
  }
}

bool BleGateway::save_() {
  Stored rec{};
  rec.enable = this->enabled_ ? 1 : 0;
  rec.has_key = this->has_key_ ? 1 : 0;
  memcpy(rec.ip, this->ip_, sizeof(rec.ip));
  memcpy(rec.key, this->key_, sizeof(rec.key));
  rec.port = this->port_;
  bool ok = this->pref_.save(&rec) && global_preferences->sync();
  memset(&rec, 0, sizeof(rec));
  return ok;
}

void BleGateway::open_socket_() {
  if (this->sock_ != nullptr)
    return;
  this->sock_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_ == nullptr) {
    this->socket_failed_ = true;
    ESP_LOGW(TAG, "Gateway socket failed");
    return;
  }
  this->sock_->setblocking(false);
  this->dest_.sin_family = AF_INET;
}

void BleGateway::configure(const char *host, size_t host_len, int port, bool enable) {
  uint8_t ip[4]{};
  if (!parse_ipv4(host, host_len, ip) || port < 1 || port > 65535) {
    ESP_LOGW(TAG, "Gateway rejected");
    return;
  }
  uint8_t previous_ip[4];
  memcpy(previous_ip, this->ip_, sizeof(previous_ip));
  uint16_t previous_port = this->port_;
  bool previous_enabled = this->enabled_;
  memcpy(this->ip_, ip, sizeof(this->ip_));
  this->port_ = (uint16_t) port;
  this->enabled_ = enable;
  if (!this->save_()) {
    memcpy(this->ip_, previous_ip, sizeof(this->ip_));
    this->port_ = previous_port;
    this->enabled_ = previous_enabled;
    ESP_LOGW(TAG, "Gateway could not be stored");
    return;
  }
  this->open_socket_();
  if (!this->enabled_)
    ESP_LOGI(TAG, "Gateway off");
  else if (!this->has_key_)
    ESP_LOGI(TAG, "Gateway waiting for a key");
  else
    ESP_LOGI(TAG, "Gateway on, %u.%u.%u.%u:%u", this->ip_[0], this->ip_[1], this->ip_[2], this->ip_[3], this->port_);
}

void BleGateway::set_key(const char *key, size_t key_len) {
  uint8_t parsed[KEY_LEN]{};
  size_t digits = 0;
  if (!parse_hex(key, key_len, parsed, sizeof(parsed), &digits)) {
    ESP_LOGW(TAG, "Gateway key rejected, length %u", (unsigned) digits);
    memset(parsed, 0, sizeof(parsed));
    return;
  }
  uint8_t previous[KEY_LEN];
  memcpy(previous, this->key_, sizeof(previous));
  bool previous_has = this->has_key_;
  memcpy(this->key_, parsed, sizeof(this->key_));
  this->has_key_ = true;
  memset(parsed, 0, sizeof(parsed));
  if (!this->save_()) {
    memcpy(this->key_, previous, sizeof(this->key_));
    this->has_key_ = previous_has;
    memset(previous, 0, sizeof(previous));
    ESP_LOGW(TAG, "Gateway key could not be stored");
    return;
  }
  memset(previous, 0, sizeof(previous));
  ESP_LOGI(TAG, "Gateway key stored, 16 bytes");
  if (this->enabled_)
    ESP_LOGI(TAG, "Gateway on, %u.%u.%u.%u:%u", this->ip_[0], this->ip_[1], this->ip_[2], this->ip_[3], this->port_);
}

bool BleGateway::recent_(uint64_t address, uint32_t hash, uint32_t now) {
  for (const auto &slot : this->seen_) {
    if (slot.address == address && slot.hash == hash && (uint32_t) (now - slot.at) < DEDUP_MS)
      return true;
  }
  Seen &slot = this->seen_[this->seen_pos_];
  slot.address = address;
  slot.hash = hash;
  slot.at = now;
  this->seen_pos_ = (uint8_t) ((this->seen_pos_ + 1) % 16);
  return false;
}

void BleGateway::on_advertisement_(const ble_device_base::RawAdvertisement &adv) {
  if (!this->enabled_ || !this->has_key_ || this->port_ == 0 || this->sock_ == nullptr)
    return;
  if (adv.data_len > MAX_ADV || (adv.data == nullptr && adv.data_len != 0)) {
    this->dropped_++;
    return;
  }
  uint32_t now = millis();
  if (this->window_start_ == 0 || (uint32_t) (now - this->window_start_) >= 1000) {
    this->window_start_ = now;
    this->window_count_ = 0;
  }
  if (this->window_count_ >= MAX_PER_SEC) {
    this->dropped_++;
    return;
  }
  uint32_t hash = 2166136261u;
  for (uint16_t i = 0; i < adv.data_len; i++) {
    hash ^= adv.data[i];
    hash *= 16777619u;
  }
  if (this->recent_(adv.address, hash, now))
    return;
  this->send_(adv);
}

void BleGateway::send_(const ble_device_base::RawAdvertisement &adv) {
  uint8_t frame[HEAD_LEN + MAX_ADV + TAG_LEN];
  uint16_t len = adv.data_len;
  frame[0] = VERSION;
  put_be32(frame + 1, this->boot_id_);
  this->counter_++;
  put_be32(frame + 5, this->counter_);
  frame[9] = adv.addr_type;
  ble_device_base::uint64_to_mac_msb_first(adv.address, frame + 10);
  frame[16] = (uint8_t) adv.rssi;
  put_be16(frame + 17, len);
  if (len != 0)
    memcpy(frame + HEAD_LEN, adv.data, len);
  size_t body = HEAD_LEN + len;

  uint8_t full[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  bool ok = info != nullptr && mbedtls_md_setup(&ctx, info, 1) == 0 &&
            mbedtls_md_hmac_starts(&ctx, this->key_, KEY_LEN) == 0 &&
            mbedtls_md_hmac_update(&ctx, frame, body) == 0 && mbedtls_md_hmac_finish(&ctx, full) == 0;
  mbedtls_md_free(&ctx);
  if (!ok) {
    memset(full, 0, sizeof(full));
    memset(frame, 0, body);
    this->dropped_++;
    return;
  }
  memcpy(frame + body, full, TAG_LEN);
  memset(full, 0, sizeof(full));

  this->dest_.sin_family = AF_INET;
  this->dest_.sin_port = htons(this->port_);
  uint32_t host = ((uint32_t) this->ip_[0] << 24) | ((uint32_t) this->ip_[1] << 16) |
                  ((uint32_t) this->ip_[2] << 8) | this->ip_[3];
  this->dest_.sin_addr.s_addr = htonl(host);
  ssize_t sent = this->sock_->sendto(frame, body + TAG_LEN, 0, reinterpret_cast<const sockaddr *>(&this->dest_),
                                     sizeof(this->dest_));
  memset(frame, 0, body + TAG_LEN);
  if (sent < 0)
    this->dropped_++;
  else
    this->window_count_++;
}

}  // namespace ble_gateway
}  // namespace esphome
