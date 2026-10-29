#include "ylkg_pair.h"

#include "../homekit/power_loss.h"

#include "esphome/components/ble_device_base/ble_aes_ccm.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <esp_gattc_api.h>
#include <esp_random.h>

#include <cctype>
#include <cstring>

namespace esphome {
namespace ylkg_pair {

static const char *const TAG = "ylkg_pair";

static constexpr uint32_t WINDOW_MS = 60000;
static constexpr uint32_t COOLDOWN_MS = 2000;
static constexpr uint32_t ATTEMPT_MS = 25000;
static constexpr uint32_t CLICK_GUARD_MS = 350;
static constexpr float BRIGHTNESS_MIN = 1.0f / 255.0f;
static constexpr float BRIGHTNESS_STEP = 8.0f / 255.0f;
static constexpr int STEP_CAP = 12;
static constexpr uint16_t UUID_SERVICE = 0xFE95;
static constexpr uint16_t UUID_AUTH = 0x0001;
static constexpr uint16_t UUID_FW = 0x0004;
static constexpr uint16_t UUID_AUTH_INIT = 0x0010;
static constexpr uint16_t UUID_BEACON = 0x0014;

static const uint8_t MI_KEY1[4] = {0x90, 0xCA, 0x85, 0xDE};
static const uint8_t MI_KEY2[4] = {0x92, 0xAB, 0x54, 0xFA};

static void (*reflect_fn)(bool on) = nullptr;

YlkgPair *YlkgPair::instance_{nullptr};

static void cipher(const uint8_t *key, size_t key_len, const uint8_t *input, size_t len, uint8_t *output) {
  uint8_t perm[256];
  for (int i = 0; i < 256; i++)
    perm[i] = (uint8_t) i;
  int j = 0;
  for (int i = 0; i < 256; i++) {
    j = (j + perm[i] + key[i % key_len]) & 0xff;
    uint8_t swap = perm[i];
    perm[i] = perm[j];
    perm[j] = swap;
  }
  int index1 = 0;
  int index2 = 0;
  for (size_t i = 0; i < len; i++) {
    index1 = (index1 + 1) & 0xff;
    index2 = (index2 + perm[index1]) & 0xff;
    uint8_t swap = perm[index1];
    perm[index1] = perm[index2];
    perm[index2] = swap;
    output[i] = input[i] ^ perm[(perm[index1] + perm[index2]) & 0xff];
  }
}

YlkgPair::YlkgPair() {
  instance_ = this;
  this->node_state = esp32_ble_tracker::ClientState::IDLE;
}

void YlkgPair::set_reflect(void (*fn)(bool on)) { reflect_fn = fn; }

void YlkgPair::open() {
  if (instance_ == nullptr)
    return;
  instance_->window_until_ = millis() + WINDOW_MS;
  instance_->want_switch_ = true;
  instance_->switch_dirty_ = true;
  if (instance_->window_logged_) {
    ESP_LOGI(TAG, "Pairing window restarted for 60 s");
  } else {
    instance_->window_logged_ = true;
    ESP_LOGI(TAG, "Pairing window open for 60 s");
  }
}

void YlkgPair::close() {
  if (instance_ == nullptr)
    return;
  instance_->end_window_();
  instance_->cancel_ = true;
  ESP_LOGI(TAG, "Pairing window closed");
}

void YlkgPair::end_window_() {
  this->window_until_ = 0;
  this->window_logged_ = false;
  this->want_switch_ = false;
  this->switch_dirty_ = true;
}

void YlkgPair::publish_switch_() {
  if (!this->switch_dirty_ || reflect_fn == nullptr)
    return;
  reflect_fn(this->want_switch_);
  this->switch_dirty_ = false;
}

void YlkgPair::zero_work_() {
  // bindkey_ is the stored beacon key. Handshake scratch must not clear it.
  memset(this->token_, 0, sizeof(this->token_));
  memset(this->scratch_, 0, sizeof(this->scratch_));
  memset(this->notify_, 0, sizeof(this->notify_));
  memset(this->key_, 0, sizeof(this->key_));
  this->notify_len_ = 0;
  this->key_ready_ = false;
  this->firmware_[0] = '\0';
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Copies exactly nbytes of hex. ':' '-' and spaces are skipped. The digit
// count is returned through digits so a rejection can be logged without the text.
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

static uint64_t mac_to_address(const uint8_t mac[6]) {
  uint64_t address = 0;
  for (int i = 0; i < 6; i++)
    address = (address << 8) | mac[i];
  return address;
}

static bool mac_nonzero(const uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0)
      return true;
  }
  return false;
}

void YlkgPair::publish_hex_(const uint8_t key[12]) {
  if (this->key_sensor_ == nullptr)
    return;
  char hex[25];
  static const char *digits = "0123456789abcdef";
  for (int i = 0; i < 12; i++) {
    hex[i * 2] = digits[key[i] >> 4];
    hex[i * 2 + 1] = digits[key[i] & 0x0f];
  }
  hex[24] = '\0';
  this->key_sensor_->publish_state(hex);
  memset(hex, 0, sizeof(hex));
}

bool YlkgPair::self_check_() const {
  const uint8_t mac[6] = {0x09, 0x6c, 0xc5, 0x41, 0x24, 0xf8};
  uint8_t mix[8];
  mix[0] = mac[0];
  mix[1] = mac[2];
  mix[2] = mac[5];
  mix[3] = (uint8_t) (950 & 0xff);
  mix[4] = (uint8_t) (950 & 0xff);
  mix[5] = mac[4];
  mix[6] = mac[5];
  mix[7] = mac[1];
  uint8_t token[12];
  for (int i = 0; i < 12; i++)
    token[i] = (uint8_t) i;
  uint8_t out[12];
  cipher(mix, sizeof(mix), token, sizeof(token), out);
  static const uint8_t expect[12] = {0x95, 0xa8, 0x2f, 0xf4, 0xa5, 0x8f, 0xe0, 0x32, 0x7e, 0x76, 0x84, 0x60};
  uint8_t back[12];
  cipher(mix, sizeof(mix), out, sizeof(out), back);
  bool ok = memcmp(out, expect, sizeof(expect)) == 0 && memcmp(back, token, sizeof(token)) == 0;
  memset(out, 0, sizeof(out));
  memset(back, 0, sizeof(back));
  return ok;
}

void YlkgPair::reversed_mac_(uint8_t out[6]) const {
  uint64_t address = this->parent_ == nullptr ? 0 : this->parent_->get_address();
  for (int i = 0; i < 6; i++)
    out[i] = (uint8_t) ((address >> (8 * i)) & 0xff);
}

void YlkgPair::mix_a_(const uint8_t mac[6], uint8_t out[8]) const {
  out[0] = mac[0];
  out[1] = mac[2];
  out[2] = mac[5];
  out[3] = (uint8_t) (this->product_id_ & 0xff);
  out[4] = (uint8_t) (this->product_id_ & 0xff);
  out[5] = mac[4];
  out[6] = mac[5];
  out[7] = mac[1];
}

void YlkgPair::mix_b_(const uint8_t mac[6], uint8_t out[8]) const {
  out[0] = mac[0];
  out[1] = mac[2];
  out[2] = mac[5];
  out[3] = (uint8_t) ((this->product_id_ >> 8) & 0xff);
  out[4] = mac[4];
  out[5] = mac[0];
  out[6] = mac[5];
  out[7] = (uint8_t) (this->product_id_ & 0xff);
}

bool YlkgPair::write_descr_(uint16_t handle, uint16_t len) {
  if (this->parent_ == nullptr || handle == 0 || len == 0 || len > sizeof(this->scratch_))
    return false;
  esp_err_t err = esp_ble_gattc_write_char_descr(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle,
                                                 len, this->scratch_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  return err == ESP_OK;
}

void YlkgPair::begin_known_() {
  // Product 950 keeps a fixed FE95 layout. Full discovery does not complete
  // on this dimmer, and the published handles are auth 3, firmware 10,
  // auth-init 19, beacon key 25. The auth CCCD is the next handle.
  if (this->parent_ == nullptr) {
    this->stop_("no client");
    return;
  }
  this->node_state = esp32_ble_tracker::ClientState::CONNECTED;
  this->handle_auth_ = 3;
  this->handle_fw_ = 10;
  this->handle_auth_init_ = 19;
  this->handle_beacon_ = 25;
  for (int i = 0; i < 12; i++)
    this->token_[i] = (uint8_t) (esp_random() & 0xff);
  if (this->parent_->register_for_notify(this->handle_auth_) != ESP_OK) {
    this->stop_("notify register failed");
    return;
  }
  this->scratch_[0] = 0x01;
  this->scratch_[1] = 0x00;
  this->phase_ = Phase::WAIT_CCCD;
  if (!this->write_descr_(4, 2))
    this->stop_("subscribe failed");
  else
    ESP_LOGI(TAG, "Using known handles, authenticating");
}

bool YlkgPair::write_at_(uint16_t handle, uint16_t len) {
  if (this->parent_ == nullptr || handle == 0 || len == 0 || len > sizeof(this->scratch_))
    return false;
  esp_err_t err = esp_ble_gattc_write_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle, len,
                                           this->scratch_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  return err == ESP_OK;
}

bool YlkgPair::read_at_(uint16_t handle) {
  if (this->parent_ == nullptr || handle == 0)
    return false;
  esp_err_t err = esp_ble_gattc_read_char(this->parent_->get_gattc_if(), this->parent_->get_conn_id(), handle,
                                          ESP_GATT_AUTH_REQ_NONE);
  return err == ESP_OK;
}

void YlkgPair::stop_(const char *reason) {
  if (this->phase_ == Phase::IDLE)
    return;
  ESP_LOGW(TAG, "Pairing stopped: %s", reason);
  this->zero_work_();
  this->phase_ = Phase::IDLE;
  this->next_attempt_at_ = millis() + COOLDOWN_MS;
  if (this->parent_ == nullptr)
    return;
  auto st = this->parent_->state();
  if (st == esp32_ble_tracker::ClientState::DISCOVERED) {
    this->parent_->set_state(esp32_ble_tracker::ClientState::IDLE);
  } else if (st != esp32_ble_tracker::ClientState::IDLE && st != esp32_ble_tracker::ClientState::DISCONNECTING &&
             st != esp32_ble_tracker::ClientState::INIT) {
    this->parent_->disconnect();
  }
}

void YlkgPair::setup() {
  this->pref_ = global_preferences->make_preference<BindRecord>(fnv1_hash("kitchen_ylkg_bindkey"));
  BindRecord loaded{};
  if (this->pref_.load(&loaded) && loaded.len == 12 && mac_nonzero(loaded.mac) && this->parent_ != nullptr) {
    memcpy(this->bindkey_, loaded.key, sizeof(this->bindkey_));
    this->has_key_ = true;
    this->publish_hex_(loaded.key);
    this->parent_->set_address(mac_to_address(loaded.mac));
    this->address_from_store_ = true;
    if (loaded.product_id != 0)
      this->product_id_ = loaded.product_id;
    ESP_LOGI(TAG, "Stored beacon key, 12 bytes, product %u", this->product_id_);
  } else if (loaded.len != 0) {
    ESP_LOGI(TAG, "Stored remote incomplete");
  }
  memset(&loaded, 0, sizeof(loaded));
  if (!this->self_check_()) {
    this->broken_ = true;
    ESP_LOGE(TAG, "Cipher self-check failed");
    return;
  }
  if (!this->gesture_self_check_()) {
    this->gesture_broken_ = true;
    ESP_LOGE(TAG, "Gesture cipher self-check failed");
  } else if (this->has_key_) {
    ESP_LOGI(TAG, "Remote control ready");
  } else {
    ESP_LOGI(TAG, "Remote control idle");
  }
  ESP_LOGI(TAG, "Pairing idle until a remote call");
}

void YlkgPair::dump_config() {
  ESP_LOGCONFIG(TAG, "YLKG pair: product %u, beacon key %s, address %s", this->product_id_,
                this->has_key_ ? "stored" : "absent", this->address_from_store_ ? "stored" : "config");
}

void YlkgPair::set_remote(const char *mac, size_t mac_len, const char *key, size_t key_len, int product_id) {
  if (this->phase_ != Phase::IDLE || this->parent_ == nullptr ||
      this->parent_->state() != esp32_ble_tracker::ClientState::IDLE) {
    ESP_LOGW(TAG, "Remote rejected, pairing busy");
    return;
  }
  if (product_id < 0 || product_id > 65535) {
    ESP_LOGW(TAG, "Remote rejected, product out of range");
    return;
  }
  uint8_t mac_bytes[6]{};
  uint8_t key_bytes[12]{};
  size_t mac_digits = 0;
  size_t key_digits = 0;
  bool mac_ok = parse_hex(mac, mac_len, mac_bytes, sizeof(mac_bytes), &mac_digits) && mac_nonzero(mac_bytes);
  bool key_ok = parse_hex(key, key_len, key_bytes, sizeof(key_bytes), &key_digits);
  if (!mac_ok || !key_ok) {
    ESP_LOGW(TAG, "Remote rejected, mac length %u key length %u", (unsigned) mac_digits, (unsigned) key_digits);
    memset(mac_bytes, 0, sizeof(mac_bytes));
    memset(key_bytes, 0, sizeof(key_bytes));
    return;
  }
  uint16_t product = product_id == 0 ? this->product_id_ : (uint16_t) product_id;
  BindRecord rec{};
  rec.len = 12;
  memcpy(rec.key, key_bytes, sizeof(rec.key));
  memcpy(rec.mac, mac_bytes, sizeof(rec.mac));
  rec.product_id = product;
  bool saved = this->pref_.save(&rec) && global_preferences->sync();
  if (!saved) {
    ESP_LOGW(TAG, "Remote could not be stored");
    memset(&rec, 0, sizeof(rec));
    memset(mac_bytes, 0, sizeof(mac_bytes));
    memset(key_bytes, 0, sizeof(key_bytes));
    return;
  }
  memcpy(this->bindkey_, rec.key, sizeof(this->bindkey_));
  this->has_key_ = true;
  this->product_id_ = product;
  this->publish_hex_(rec.key);
  this->parent_->set_address(mac_to_address(mac_bytes));
  this->have_frame_ = false;
  this->address_from_store_ = true;
  memset(&rec, 0, sizeof(rec));
  memset(mac_bytes, 0, sizeof(mac_bytes));
  memset(key_bytes, 0, sizeof(key_bytes));
  ESP_LOGI(TAG, "Remote stored, key 12 bytes, product %u", product);
}

void YlkgPair::store_() {
  if (!this->key_ready_) {
    this->phase_ = Phase::IDLE;
    return;
  }
  BindRecord rec{};
  rec.len = 12;
  memcpy(rec.key, this->key_, sizeof(rec.key));
  uint64_t address = this->parent_ == nullptr ? 0 : this->parent_->get_address();
  for (int i = 0; i < 6; i++)
    rec.mac[i] = (uint8_t) ((address >> (40 - 8 * i)) & 0xff);
  rec.product_id = this->product_id_;
  if (!this->pref_.save(&rec) || !global_preferences->sync())
    ESP_LOGW(TAG, "Beacon key could not be stored");
  else if (mac_nonzero(rec.mac))
    this->address_from_store_ = true;
  memcpy(this->bindkey_, rec.key, sizeof(this->bindkey_));
  this->publish_hex_(rec.key);
  memset(&rec, 0, sizeof(rec));
  this->has_key_ = true;
  this->have_frame_ = false;
  if (this->firmware_[0] != '\0')
    ESP_LOGI(TAG, "Remote firmware: %s", this->firmware_);
  else
    ESP_LOGI(TAG, "Remote firmware unreadable");
  ESP_LOGI(TAG, "Pairing complete, 12 bytes stored");
  this->zero_work_();
  this->end_window_();
  this->cancel_ = false;
  this->phase_ = Phase::IDLE;
  this->next_attempt_at_ = millis() + COOLDOWN_MS;
  if (this->parent_ != nullptr)
    this->parent_->disconnect();
}

void YlkgPair::loop() {
  this->publish_switch_();
  // esp_ble_gattc_search_service only reads the local cache. An empty cache
  // returns ESP_GATT_INTERNAL_ERROR (129) and must not be treated as discovery.
  // The peer database arrives with ESP_GATTC_DIS_SRVC_CMPL_EVT. One cache
  // refresh restarts that discovery if the first pass left the cache empty.
  if (this->phase_ == Phase::WAIT_DISCOVERY && this->rediscover_at_ != 0 &&
      (int32_t) (millis() - this->rediscover_at_) >= 0) {
    if (this->known_used_ || this->parent_ == nullptr) {
      this->stop_("service discovery failed");
      return;
    }
    this->known_used_ = true;
    this->rediscover_at_ = 0;
    this->begin_known_();
    return;
  }
  if (this->phase_ == Phase::WAIT_LOOKUP) {
    if (this->cancel_) {
      this->cancel_ = false;
      this->stop_("cancelled");
      return;
    }
    this->on_services_();
    return;
  }
  if (this->phase_ == Phase::FINISH) {
    this->store_();
    return;
  }
  if (this->cancel_) {
    this->cancel_ = false;
    if (this->phase_ != Phase::IDLE)
      this->stop_("cancelled");
  }
  if (this->window_until_ != 0 && (int32_t) (millis() - this->window_until_) >= 0) {
    this->end_window_();
    ESP_LOGI(TAG, "Pairing window closed");
    if (this->phase_ == Phase::CONNECTING)
      this->stop_("window closed");
  }
  if (this->phase_ != Phase::IDLE && (int32_t) (millis() - this->attempt_started_) > (int32_t) ATTEMPT_MS)
    this->stop_("timed out");
}

// Legacy MiBeacon V3. The 12-byte beacon key expands by the fixed 4-byte
// insert. CTR matches Cryptodome CCM decrypt of the object bytes. The last
// four bytes are nonce material, not a tag we check. A parsed object
// 0x1001 of length 3 is the dimmer event. The vector below uses a fake key.
struct Decoded {
  bool ok{false};
  bool structured{false};
  uint8_t button_type{0};
  uint8_t value{0};
  uint8_t press{0};
  uint16_t first_id{0};
  uint8_t first_len{0};
  uint8_t payload_len{0};
};

static bool ctr_xor_(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *input, size_t len, uint8_t *output) {
  if (input == nullptr || output == nullptr || len == 0 || len > 32)
    return false;
  const size_t nonce_len = 13;
  const size_t width = 15 - nonce_len;
  for (size_t off = 0; off < len; off += 16) {
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[0] = (uint8_t) (width - 1);
    memcpy(block + 1, nonce, nonce_len);
    uint32_t counter = (uint32_t) (off / 16) + 1;
    for (size_t i = 0; i < width; i++)
      block[15 - i] = (uint8_t) ((counter >> (8 * i)) & 0xff);
    uint8_t ks[16];
    ble_device_base::aes128_encrypt_block(key, block, ks);
    size_t n = len - off;
    if (n > 16)
      n = 16;
    for (size_t i = 0; i < n; i++)
      output[off + i] = (uint8_t) (input[off + i] ^ ks[i]);
    memset(ks, 0, sizeof(ks));
    memset(block, 0, sizeof(block));
  }
  return true;
}

static Decoded decode_button_(const uint8_t key12[12], const uint8_t mac_rev[6], const uint8_t *svc, size_t size,
                              uint16_t product_id) {
  Decoded out;
  if (svc == nullptr || key12 == nullptr || mac_rev == nullptr || size < 12)
    return out;
  uint16_t frame = (uint16_t) svc[0] | ((uint16_t) svc[1] << 8);
  uint16_t product = (uint16_t) svc[2] | ((uint16_t) svc[3] << 8);
  uint8_t version = (uint8_t) (frame >> 12);
  bool encrypted = (frame & (1u << 3)) != 0;
  bool has_object = (frame & (1u << 6)) != 0;
  if (product != product_id || !encrypted || !has_object || version < 2 || version > 3)
    return out;
  out.structured = true;
  size_t index = 5;
  if ((frame & (1u << 4)) != 0) {
    if (size < index + 6)
      return out;
    if (memcmp(svc + index, mac_rev, 6) != 0)
      return out;
    index += 6;
  }
  if ((frame & (1u << 5)) != 0) {
    if (size < index + 1)
      return out;
    uint8_t capability = svc[index];
    index += 1;
    if ((capability & 0x20) != 0) {
      if (size < index + 1)
        return out;
      index += 1;
    }
  }
  if (size < index + 7)
    return out;
  size_t ct_len = size - index - 4;
  if (ct_len == 0 || ct_len > 32)
    return out;
  uint8_t key[16];
  memcpy(key, key12, 6);
  key[6] = 0x8d;
  key[7] = 0x3d;
  key[8] = 0x3c;
  key[9] = 0x97;
  memcpy(key + 10, key12 + 6, 6);
  uint8_t nonce[13];
  memcpy(nonce, svc, 5);
  memcpy(nonce + 5, svc + size - 4, 3);
  memcpy(nonce + 8, mac_rev, 5);
  uint8_t plain[32];
  bool decrypted = ctr_xor_(key, nonce, svc + index, ct_len, plain);
  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  if (!decrypted) {
    memset(plain, 0, sizeof(plain));
    return out;
  }
  out.payload_len = (uint8_t) ct_len;
  size_t off = 0;
  bool saw = false;
  while (off + 3 <= ct_len) {
    uint16_t id = (uint16_t) plain[off] | ((uint16_t) plain[off + 1] << 8);
    uint8_t len = plain[off + 2];
    if (!saw) {
      out.first_id = id;
      out.first_len = len;
      saw = true;
    }
    if (len == 0 || off + 3u + len > ct_len)
      break;
    if (id == 0x1001 && len == 3) {
      out.ok = true;
      out.button_type = plain[off + 3];
      out.value = plain[off + 4];
      out.press = plain[off + 5];
      break;
    }
    off += 3u + len;
  }
  memset(plain, 0, sizeof(plain));
  return out;
}

bool YlkgPair::gesture_self_check_() const {
  // Fake key and a synthetic rotate-right packet. Not the stored beacon key.
  static const uint8_t key[12] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b};
  static const uint8_t mac[6] = {0x09, 0x6c, 0xc5, 0x41, 0x24, 0xf8};
  static const uint8_t svc[] = {0x58, 0x31, 0xb6, 0x03, 0x2a, 0x09, 0x6c, 0xc5, 0x41, 0x24, 0xf8, 0xed, 0x17, 0x27,
                                0xbb, 0xd6, 0x43, 0x01, 0x02, 0x03, 0x04};
  Decoded decoded = decode_button_(key, mac, svc, sizeof(svc), 950);
  return decoded.ok && decoded.button_type == 0 && decoded.value == 5 && decoded.press == 4;
}

static float kept_brightness_(light::LightState *light) {
  float level = light->remote_values.get_brightness();
  if (level < BRIGHTNESS_MIN)
    return 1.0f;
  return level;
}

void YlkgPair::apply_gesture_(uint8_t button_type, uint8_t value, uint8_t press) {
  if (this->ceiling_ == nullptr || this->night_ == nullptr)
    return;
  const bool toggle = press == 0 || (press == 3 && button_type == 0 && value == 1);
  const bool swap = press == 1 || (press == 3 && button_type == 0 && value == 2);
  const bool dim = press == 4 && button_type == 0;
  if (!toggle && !swap && !dim)
    return;
  const uint32_t now = millis();
  if (toggle && this->last_toggle_ms_ != 0 && (int32_t) (now - this->last_toggle_ms_) < (int32_t) CLICK_GUARD_MS)
    return;
  if (swap && this->last_swap_ms_ != 0 && (int32_t) (now - this->last_swap_ms_) < (int32_t) CLICK_GUARD_MS)
    return;

  const bool ceiling_on = this->ceiling_->remote_values.is_on();
  const bool night_on = this->night_->remote_values.is_on();
  uint8_t channel = this->selected_;
  if (night_on && !ceiling_on)
    channel = 1;
  else if (ceiling_on)
    channel = 0;
  light::LightState *active = channel == 1 ? this->night_ : this->ceiling_;
  const bool save = homekit::RestoreAfterPowerLoss::is_enabled();

  if (toggle) {
    this->last_toggle_ms_ = now;
    this->selected_ = channel;
    const char *which = channel == 1 ? "night" : "ceiling";
    if (active->remote_values.is_on()) {
      active->turn_off().set_save(save).set_transition_length(0).perform();
      ESP_LOGI(TAG, "click %s off", which);
    } else {
      active->turn_on().set_brightness(kept_brightness_(active)).set_save(save).set_transition_length(0).perform();
      ESP_LOGI(TAG, "click %s on", which);
    }
    return;
  }

  if (swap) {
    this->last_swap_ms_ = now;
    uint8_t next = channel == 1 ? 0 : 1;
    this->selected_ = next;
    const char *which = next == 1 ? "night" : "ceiling";
    if (!ceiling_on && !night_on) {
      ESP_LOGI(TAG, "double select %s", which);
      return;
    }
    light::LightState *dest = next == 1 ? this->night_ : this->ceiling_;
    dest->turn_on().set_brightness(kept_brightness_(dest)).set_save(save).set_transition_length(0).perform();
    ESP_LOGI(TAG, "double swap to %s", which);
    return;
  }

  int steps = value;
  int dir = 1;
  if (value > 127) {
    dir = -1;
    steps = 256 - value;
  }
  if (steps > STEP_CAP)
    steps = STEP_CAP;
  if (steps <= 0)
    return;
  if (!active->remote_values.is_on() && dir < 0)
    return;
  float level = !active->remote_values.is_on()
                    ? (float) steps * BRIGHTNESS_STEP
                    : active->remote_values.get_brightness() + (float) (dir * steps) * BRIGHTNESS_STEP;
  if (level < BRIGHTNESS_MIN)
    level = BRIGHTNESS_MIN;
  if (level > 1.0f)
    level = 1.0f;
  this->selected_ = channel;
  active->turn_on().set_brightness(level).set_save(save).set_transition_length(0).perform();
  ESP_LOGI(TAG, "dim %+d", dir * steps);
}

void YlkgPair::on_control_(const ble_device_base::ESPBTDevice &device) {
  if (this->gesture_broken_ || !this->has_key_ || this->phase_ != Phase::IDLE)
    return;
  if (this->ceiling_ == nullptr || this->night_ == nullptr)
    return;
  for (const auto &service : device.get_service_datas()) {
    if (service.uuid.type() != ble_device_base::ESPBTUUID::Type::UUID16 || service.uuid.uuid16() != UUID_SERVICE)
      continue;
    const auto &data = service.data;
    if (data.size() < 5)
      return;
    uint8_t counter = data[4];
    if (this->have_frame_ && counter == this->last_frame_)
      return;
    uint8_t mac[6];
    this->reversed_mac_(mac);
    Decoded decoded = decode_button_(this->bindkey_, mac, data.data(), data.size(), this->product_id_);
    memset(mac, 0, sizeof(mac));
    if (!decoded.structured)
      return;
    this->have_frame_ = true;
    this->last_frame_ = counter;
    if (!decoded.ok) {
      ESP_LOGI(TAG, "Remote gesture rejected, object %04x len %u payload %u", decoded.first_id, decoded.first_len,
               decoded.payload_len);
      return;
    }
    this->apply_gesture_(decoded.button_type, decoded.value, decoded.press);
    return;
  }
}

// A normal press is an encrypted MiBeacon. Pairing is the unencrypted one
// for the aimed product, with either the bind-request bit or a connectable capability.
struct Fe95View {
  bool present{false};
  bool pairing{false};
  uint16_t frame{0};
  uint16_t product{0};
  uint8_t capability{0};
  uint8_t encrypted{0};
};

static Fe95View fe95_view_(const ble_device_base::ESPBTDevice &device, uint16_t product_id) {
  Fe95View view;
  for (const auto &service : device.get_service_datas()) {
    if (service.uuid.type() != ble_device_base::ESPBTUUID::Type::UUID16 || service.uuid.uuid16() != UUID_SERVICE)
      continue;
    const auto &data = service.data;
    if (data.size() < 5)
      return view;
    view.present = true;
    view.frame = (uint16_t) data[0] | ((uint16_t) data[1] << 8);
    view.product = (uint16_t) data[2] | ((uint16_t) data[3] << 8);
    view.encrypted = (view.frame & (1u << 3)) != 0 ? 1 : 0;
    bool mac = (view.frame & (1u << 4)) != 0;
    bool has_capability = (view.frame & (1u << 5)) != 0;
    bool bind_request = (view.frame & (1u << 9)) != 0;
    size_t index = 5;
    if (mac) {
      if (data.size() < index + 6)
        return view;
      index += 6;
    }
    if (has_capability) {
      if (data.size() < index + 1)
        return view;
      view.capability = data[index];
    }
    bool connectable = has_capability && (view.capability & 0x01) != 0;
    view.pairing = view.encrypted == 0 && view.product == product_id && (bind_request || connectable);
    return view;
  }
  return view;
}

bool YlkgPair::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (this->broken_ || this->parent_ == nullptr)
    return false;
  if (device.address_uint64() == this->parent_->get_address())
    this->on_control_(device);
  if (this->window_until_ == 0 || (int32_t) (millis() - this->window_until_) >= 0)
    return false;
  if (this->phase_ != Phase::IDLE)
    return false;
  if ((int32_t) (millis() - this->next_attempt_at_) < 0)
    return false;
  if (device.address_uint64() != this->parent_->get_address())
    return false;
  if (this->parent_->state() != esp32_ble_tracker::ClientState::IDLE)
    return false;
  if (this->parent_->get_gattc_if() == ESP_GATT_IF_NONE)
    return false;
  Fe95View adv = fe95_view_(device, this->product_id_);
  if (!adv.pairing) {
    static uint32_t last_log = 0;
    static uint16_t last_frame = 0xffff;
    if (adv.present && (adv.frame != last_frame || (int32_t) (millis() - last_log) > 5000)) {
      last_log = millis();
      last_frame = adv.frame;
      ESP_LOGI(TAG, "Remote adv fc=%04x pid=%u enc=%u cap=%02x", adv.frame, adv.product, adv.encrypted, adv.capability);
    }
    return false;
  }
  ESP_LOGI(TAG, "Remote pairing advert fc=%04x cap=%02x", adv.frame, adv.capability);
  this->zero_work_();
  this->parent_->set_remote_addr_type(device.get_address_type());
  this->parent_->set_address(device.address_uint64());
  this->attempt_started_ = millis();
  this->rediscover_at_ = 0;
  this->saw_discovery_ = false;
  this->refreshed_ = false;
  this->known_used_ = false;
  this->phase_ = Phase::CONNECTING;
  this->parent_->set_state(esp32_ble_tracker::ClientState::DISCOVERED);
  ESP_LOGI(TAG, "Remote in range, connecting");
  return false;
}

static uint16_t char_handle_(esp_gatt_if_t gattc, uint16_t conn, uint16_t start, uint16_t end, uint16_t uuid16) {
  esp_bt_uuid_t uuid;
  memset(&uuid, 0, sizeof(uuid));
  uuid.len = ESP_UUID_LEN_16;
  uuid.uuid.uuid16 = uuid16;
  esp_gattc_char_elem_t elem;
  uint16_t count = 1;
  esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(gattc, conn, start, end, uuid, &elem, &count);
  if (status != ESP_GATT_OK || count == 0)
    return 0;
  return elem.char_handle;
}

void YlkgPair::on_services_() {
  // The parent's service vector can already have been released. Read the
  // stack cache from loop(), and keep node_state off ESTABLISHED so that
  // cache is not cleaned while the handshake still needs the handles.
  if (this->parent_ == nullptr) {
    this->stop_("no client");
    return;
  }
  this->node_state = esp32_ble_tracker::ClientState::CONNECTED;
  esp_gatt_if_t gattc = this->parent_->get_gattc_if();
  uint16_t conn = this->parent_->get_conn_id();
  esp_bt_uuid_t svc_uuid;
  memset(&svc_uuid, 0, sizeof(svc_uuid));
  svc_uuid.len = ESP_UUID_LEN_16;
  svc_uuid.uuid.uuid16 = UUID_SERVICE;
  esp_gattc_service_elem_t svc;
  uint16_t count = 1;
  esp_gatt_status_t status = esp_ble_gattc_get_service(gattc, conn, &svc_uuid, &svc, &count, 0);
  if (status != ESP_GATT_OK || count == 0) {
    count = 1;
    esp_bt_uuid_t wide;
    memset(&wide, 0, sizeof(wide));
    wide.len = ESP_UUID_LEN_128;
    static const uint8_t fe95_128[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
                                          0x95, 0xFE, 0x00, 0x00};
    memcpy(wide.uuid.uuid128, fe95_128, sizeof(fe95_128));
    status = esp_ble_gattc_get_service(gattc, conn, &wide, &svc, &count, 0);
  }
  if (status != ESP_GATT_OK || count == 0) {
    if (!this->refreshed_) {
      ESP_LOGW(TAG, "FE95 missing status=%d count=%u", (int) status, count);
      this->phase_ = Phase::WAIT_DISCOVERY;
      this->rediscover_at_ = millis();
      return;
    }
    ESP_LOGW(TAG, "FE95 stack lookup status=%d count=%u", (int) status, count);
    for (uint16_t offset = 0; offset < 6; offset++) {
      esp_gattc_service_elem_t one;
      uint16_t found = 1;
      esp_gatt_status_t one_status = esp_ble_gattc_get_service(gattc, conn, nullptr, &one, &found, offset);
      if (one_status != ESP_GATT_OK || found == 0)
        break;
      uint16_t uuid16 = one.uuid.len == ESP_UUID_LEN_16 ? one.uuid.uuid.uuid16 : 0;
      ESP_LOGW(TAG, "service %u len=%u uuid16=%04x", offset, one.uuid.len, uuid16);
    }
    this->stop_("service fe95 incomplete");
    return;
  }
  this->handle_auth_ = char_handle_(gattc, conn, svc.start_handle, svc.end_handle, UUID_AUTH);
  this->handle_auth_init_ = char_handle_(gattc, conn, svc.start_handle, svc.end_handle, UUID_AUTH_INIT);
  this->handle_beacon_ = char_handle_(gattc, conn, svc.start_handle, svc.end_handle, UUID_BEACON);
  this->handle_fw_ = char_handle_(gattc, conn, svc.start_handle, svc.end_handle, UUID_FW);
  if (this->handle_auth_ == 0 || this->handle_auth_init_ == 0 || this->handle_beacon_ == 0 || this->handle_fw_ == 0) {
    ESP_LOGW(TAG, "FE95 chars auth=%u init=%u beacon=%u fw=%u", this->handle_auth_, this->handle_auth_init_,
             this->handle_beacon_, this->handle_fw_);
    this->stop_("service fe95 incomplete");
    return;
  }
  for (int i = 0; i < 12; i++)
    this->token_[i] = (uint8_t) (esp_random() & 0xff);
  if (this->parent_->register_for_notify(this->handle_auth_) != ESP_OK) {
    this->stop_("notify register failed");
    return;
  }
  this->phase_ = Phase::WAIT_CCCD;
  ESP_LOGI(TAG, "Remote connected, authenticating");
}

void YlkgPair::on_auth_notify_() {
  if (this->phase_ != Phase::WAIT_NOTIFY || this->notify_len_ != 12)
    return;
  uint8_t mac[6];
  uint8_t mixa[8];
  uint8_t mixb[8];
  uint8_t inner[12];
  uint8_t outer[12];
  this->reversed_mac_(mac);
  this->mix_a_(mac, mixa);
  this->mix_b_(mac, mixb);
  cipher(mixa, sizeof(mixa), this->notify_, this->notify_len_, inner);
  cipher(mixb, sizeof(mixb), inner, this->notify_len_, outer);
  bool ok = memcmp(outer, this->token_, sizeof(this->token_)) == 0;
  memset(inner, 0, sizeof(inner));
  memset(outer, 0, sizeof(outer));
  memset(mixa, 0, sizeof(mixa));
  memset(mixb, 0, sizeof(mixb));
  if (!ok) {
    this->stop_("auth check failed");
    return;
  }
  cipher(this->token_, sizeof(this->token_), MI_KEY2, sizeof(MI_KEY2), this->scratch_);
  this->phase_ = Phase::WAIT_KEY2;
  if (!this->write_at_(this->handle_auth_, sizeof(MI_KEY2)))
    this->stop_("key confirm write failed");
}

void YlkgPair::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  (void) gattc_if;
  if (this->parent_ == nullptr || param == nullptr)
    return;
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status != ESP_GATT_OK && param->open.status != ESP_GATT_ALREADY_OPEN &&
          this->phase_ == Phase::CONNECTING) {
        ESP_LOGW(TAG, "Connect failed, status %d", param->open.status);
        this->zero_work_();
        this->phase_ = Phase::IDLE;
        this->next_attempt_at_ = millis() + COOLDOWN_MS;
      }
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      if (this->phase_ != Phase::IDLE && this->phase_ != Phase::FINISH)
        this->stop_("link down");
      break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT: {
      uint16_t event_conn = param->dis_srvc_cmpl.conn_id;
      uint16_t ours = this->parent_->get_conn_id();
      // The completion event can be queued before OPEN stores conn_id. 0xFFFF
      // means this client has not recorded one yet, so accept conn 0.
      if (ours != 0xFFFF && event_conn != ours)
        break;
      if (this->phase_ != Phase::CONNECTING && this->phase_ != Phase::WAIT_DISCOVERY)
        break;
      this->saw_discovery_ = true;
      // Stay off ESTABLISHED so the parent does not cache_clean this database.
      this->node_state = esp32_ble_tracker::ClientState::CONNECTED;
      ESP_LOGI(TAG, "Discovery status=%d", (int) param->dis_srvc_cmpl.status);
      if (param->dis_srvc_cmpl.status == ESP_GATT_OK) {
        this->phase_ = Phase::WAIT_LOOKUP;
      } else if (!this->refreshed_) {
        this->phase_ = Phase::WAIT_DISCOVERY;
        this->rediscover_at_ = millis();
      } else {
        this->stop_("service discovery failed");
      }
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (param->search_cmpl.conn_id != this->parent_->get_conn_id())
        break;
      if (this->phase_ != Phase::CONNECTING && this->phase_ != Phase::WAIT_DISCOVERY)
        break;
      this->node_state = esp32_ble_tracker::ClientState::CONNECTED;
      if (param->search_cmpl.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Cache search ready");
        this->phase_ = Phase::WAIT_LOOKUP;
        break;
      }
      ESP_LOGI(TAG, "Cache search status=%d", (int) param->search_cmpl.status);
      // Status 129 means the local cache is empty. This dimmer does not
      // finish service discovery, so use the fixed product-950 handles.
      this->phase_ = Phase::WAIT_DISCOVERY;
      if (!this->known_used_)
        this->rediscover_at_ = millis();
      break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (this->phase_ != Phase::WAIT_CCCD || param->reg_for_notify.handle != this->handle_auth_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK)
        this->stop_("notify register status");
      break;
    case ESP_GATTC_WRITE_DESCR_EVT:
      if (this->phase_ != Phase::WAIT_CCCD)
        break;
      if (param->write.status != ESP_GATT_OK) {
        this->stop_("subscribe failed");
        break;
      }
      memcpy(this->scratch_, MI_KEY1, sizeof(MI_KEY1));
      this->phase_ = Phase::WAIT_AUTH_INIT;
      if (!this->write_at_(this->handle_auth_init_, sizeof(MI_KEY1)))
        this->stop_("auth init write failed");
      break;
    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        if (this->phase_ != Phase::IDLE)
          this->stop_("write rejected");
        break;
      }
      if (this->phase_ == Phase::WAIT_AUTH_INIT && param->write.handle == this->handle_auth_init_) {
        uint8_t mac[6];
        uint8_t mixa[8];
        this->reversed_mac_(mac);
        this->mix_a_(mac, mixa);
        cipher(mixa, sizeof(mixa), this->token_, sizeof(this->token_), this->scratch_);
        memset(mixa, 0, sizeof(mixa));
        this->phase_ = Phase::WAIT_CHALLENGE;
        if (!this->write_at_(this->handle_auth_, 12))
          this->stop_("challenge write failed");
      } else if (this->phase_ == Phase::WAIT_CHALLENGE && param->write.handle == this->handle_auth_) {
        this->phase_ = Phase::WAIT_NOTIFY;
        if (this->notify_len_ == 12)
          this->on_auth_notify_();
      } else if (this->phase_ == Phase::WAIT_KEY2 && param->write.handle == this->handle_auth_) {
        this->phase_ = Phase::WAIT_BEACON;
        if (!this->read_at_(this->handle_beacon_))
          this->stop_("beacon key read failed");
      }
      break;
    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle != this->handle_auth_ || param->notify.value == nullptr)
        break;
      if (param->notify.value_len != 12) {
        if (this->phase_ != Phase::IDLE)
          this->stop_("notify length");
        break;
      }
      if (this->notify_len_ == 0) {
        memcpy(this->notify_, param->notify.value, 12);
        this->notify_len_ = 12;
      }
      if (this->phase_ == Phase::WAIT_NOTIFY)
        this->on_auth_notify_();
      break;
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status != ESP_GATT_OK || param->read.value == nullptr) {
        if (this->phase_ == Phase::WAIT_BEACON || this->phase_ == Phase::WAIT_FW)
          this->stop_("read failed");
        break;
      }
      uint16_t len = param->read.value_len;
      if (this->phase_ == Phase::WAIT_BEACON && param->read.handle == this->handle_beacon_) {
        if (len != 12) {
          ESP_LOGW(TAG, "Beacon key length %u", len);
          this->stop_("beacon key length");
          break;
        }
        cipher(this->token_, sizeof(this->token_), param->read.value, len, this->key_);
        this->key_ready_ = true;
        this->phase_ = Phase::WAIT_FW;
        if (!this->read_at_(this->handle_fw_))
          this->stop_("firmware read failed");
      } else if (this->phase_ == Phase::WAIT_FW && param->read.handle == this->handle_fw_) {
        uint8_t plain[16];
        uint16_t n = len > sizeof(plain) ? sizeof(plain) : len;
        cipher(this->token_, sizeof(this->token_), param->read.value, n, plain);
        size_t out = 0;
        for (uint16_t i = 0; i < n && out + 1 < sizeof(this->firmware_); i++) {
          unsigned char ch = plain[i];
          if (std::isprint(ch) == 0)
            break;
          this->firmware_[out++] = (char) ch;
        }
        this->firmware_[out] = '\0';
        memset(plain, 0, sizeof(plain));
        this->phase_ = Phase::FINISH;
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace ylkg_pair
}  // namespace esphome
