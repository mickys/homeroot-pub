#include <esphome/core/defines.h>
#ifdef USE_LIGHT
#pragma once
#include <esphome/core/application.h>
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include <cstdint>
#include "hap_entity.h"
#include "power_loss.h"
#include "../ylkg_pair/ylkg_pair.h"

// HomeRoot change, 2026-10-28, against HAP-ESPHome
// 0ab12c97f33d894909a35b24b9e837384d063861:
// A write that was not color temperature was reported as HAP_STATUS_RES_ABSENT
// because that failure branch hung off the color-temperature test alone.
// Each characteristic is now its own branch. Color-temperature limits follow
// the light's mired range. The SDK default of 50-400 is wider than these LEDs.

namespace esphome
{
  namespace homekit
  {
    class LightEntity : public HAPEntity, public light::LightTargetStateReachedListener
    {
    private:
      static constexpr const char* TAG = "LightEntity";
      light::LightState* lightPtr;
      bool restore_switch_{false};
      static int restore_switch_write(hap_write_data_t write_data[], int count, void* serv_priv, void* write_priv) {
        (void) serv_priv;
        (void) write_priv;
        int ret = HAP_SUCCESS;
        for (int i = 0; i < count; i++) {
          hap_write_data_t* write = &write_data[i];
          if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            RestoreAfterPowerLoss::set_enabled(write->val.b);
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          } else {
            *(write->status) = HAP_STATUS_RES_ABSENT;
          }
        }
        return ret;
      }
      bool pair_switch_{false};
      static hap_char_t *&pair_on_char() {
        static hap_char_t *chr = nullptr;
        return chr;
      }
      static void reflect_pair(bool on) {
        hap_char_t *chr = pair_on_char();
        if (chr == nullptr)
          return;
        hap_val_t state;
        state.b = on;
        hap_char_update_val(chr, &state);
      }
      static int pair_switch_write(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {
        (void) serv_priv;
        (void) write_priv;
        int ret = HAP_SUCCESS;
        for (int i = 0; i < count; i++) {
          hap_write_data_t *write = &write_data[i];
          if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            if (write->val.b)
              ylkg_pair::YlkgPair::open();
            else
              ylkg_pair::YlkgPair::close();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          } else {
            *(write->status) = HAP_STATUS_RES_ABSENT;
          }
        }
        return ret;
      }
      // Mireds the accessory will advertise. Falls back to the SDK's 50-400
      // only when the light trait has no range of its own.
      static void color_temp_bounds(light::LightState* obj, int* min_out, int* max_out) {
        float min_m = obj->get_traits().get_min_mireds();
        float max_m = obj->get_traits().get_max_mireds();
        if (!(min_m > 0.0f) || !(max_m > min_m)) {
          min_m = 50.0f;
          max_m = 400.0f;
        }
        *min_out = (int) (min_m + 0.5f);
        *max_out = (int) (max_m + 0.5f);
        if (*max_out <= *min_out) {
          *min_out = 50;
          *max_out = 400;
        }
      }
      static uint32_t color_temp_mireds(light::LightState* obj) {
        int min_m = 0;
        int max_m = 0;
        color_temp_bounds(obj, &min_m, &max_m);
        float ct = obj->current_values.get_color_temperature();
        if (!(ct >= (float) min_m && ct <= (float) max_m)) {
          ct = ((float) min_m + (float) max_m) * 0.5f;
        }
        return (uint32_t) (ct + 0.5f);
      }
      static int light_write(hap_write_data_t write_data[], int count, void* serv_priv, void* write_priv) {
        light::LightState* lightPtr = (light::LightState*)serv_priv;
        const bool save = RestoreAfterPowerLoss::is_enabled();
        ESP_LOGD(TAG, "Write called for Accessory %s (%s)", std::to_string(lightPtr->get_object_id_hash()).c_str(), lightPtr->get_name().c_str());
        int i, ret = HAP_SUCCESS;
        hap_write_data_t* write;
        for (i = 0; i < count; i++) {
          write = &write_data[i];
          if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            ESP_LOGD(TAG, "Received Write for Light '%s' state: %s", lightPtr->get_name().c_str(), write->val.b ? "On" : "Off");
            ESP_LOGD(TAG, "[STATE] CURRENT STATE: %d", (int)(lightPtr->current_values.get_state() * 100));
            write->val.b ? lightPtr->turn_on().set_save(save).perform() : lightPtr->turn_off().set_save(save).perform();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          }
          else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_BRIGHTNESS)) {
            ESP_LOGD(TAG, "Received Write for Light '%s' Level: %d", lightPtr->get_name().c_str(), write->val.i);
            ESP_LOGD(TAG, "[LEVEL] CURRENT BRIGHTNESS: %d", (int)(lightPtr->current_values.get_brightness() * 100));
            ESP_LOGD(TAG, "TARGET BRIGHTNESS: %d", (int)write->val.i);
            lightPtr->make_call().set_save(save).set_brightness((float)(write->val.i) / 100).perform();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          }
          else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_HUE)) {
            ESP_LOGD(TAG, "Received Write for Light '%s' Hue: %.2f", lightPtr->get_name().c_str(), write->val.f);
            int hue = 0;
            float saturation = 0;
            float colorValue = 0;
            rgb_to_hsv(lightPtr->remote_values.get_red(), lightPtr->remote_values.get_green(), lightPtr->remote_values.get_blue(), hue, saturation, colorValue);
            ESP_LOGD(TAG, "[HUE] CURRENT Hue: %d, Saturation: %.2f, Value: %.2f", hue, saturation, colorValue);
            ESP_LOGD(TAG, "TARGET HUE: %.2f", write->val.f);
            float tR = 0;
            float tG = 0;
            float tB = 0;
            hsv_to_rgb(write->val.f, saturation, colorValue, tR, tG, tB);
            ESP_LOGD(TAG, "TARGET RGB: %.2f %.2f %.2f", tR, tG, tB);
            lightPtr->make_call().set_rgb(tR, tG, tB).set_save(save).perform();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          }
          else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_SATURATION)) {
            ESP_LOGD(TAG, "Received Write for Light '%s' Saturation: %.2f", lightPtr->get_name().c_str(), write->val.f);
            int hue = 0;
            float saturation = 0;
            float colorValue = 0;
            rgb_to_hsv(lightPtr->remote_values.get_red(), lightPtr->remote_values.get_green(), lightPtr->remote_values.get_blue(), hue, saturation, colorValue);
            ESP_LOGD(TAG, "[SATURATION] CURRENT Hue: %d, Saturation: %.2f, Value: %.2f", hue, saturation, colorValue);
            ESP_LOGD(TAG, "TARGET SATURATION: %.2f", write->val.f);
            float tR = 0;
            float tG = 0;
            float tB = 0;
            hsv_to_rgb(hue, write->val.f / 100, colorValue, tR, tG, tB);
            ESP_LOGD(TAG, "TARGET RGB: %.2f %.2f %.2f", tR, tG, tB);
            lightPtr->make_call().set_rgb(tR, tG, tB).set_save(save).perform();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          }
          else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_COLOR_TEMPERATURE)) {
            ESP_LOGD(TAG, "Received Write for Light '%s' Level: %d", lightPtr->get_name().c_str(), write->val.i);
            ESP_LOGD(TAG, "[LEVEL] CURRENT COLOR TEMPERATURE(mired): %.2f", lightPtr->current_values.get_color_temperature());
            ESP_LOGD(TAG, "TARGET COLOR TEMPERATURE(mired): %lu", write->val.u);
            lightPtr->make_call().set_color_temperature(write->val.u).set_save(save).perform();
            hap_char_update_val(write->hc, &(write->val));
            *(write->status) = HAP_STATUS_SUCCESS;
          }
          else {
            *(write->status) = HAP_STATUS_RES_ABSENT;
          }
        }
        return ret;
      }
      void on_light_target_state_reached() override {
        on_light_update(lightPtr);
      }
      static void on_light_update(light::LightState* obj) {
        bool rgb = obj->current_values.get_color_mode() & light::ColorCapability::RGB;
        bool level = obj->get_traits().supports_color_capability(light::ColorCapability::BRIGHTNESS);
        bool temperature = obj->current_values.get_color_mode() & (light::ColorCapability::COLOR_TEMPERATURE | light::ColorCapability::COLD_WARM_WHITE);
        if (rgb) {
          ESP_LOGD(TAG, "%s RED: %.2f, GREEN: %.2f, BLUE: %.2f", obj->get_name().c_str(), obj->current_values.get_red(), obj->current_values.get_green(), obj->current_values.get_blue());
        }
        ESP_LOGD(TAG, "%s state: %d brightness: %d", obj->get_name().c_str(), (int)(obj->current_values.get_state() * 100), (int)(obj->current_values.get_brightness() * 100));
        hap_acc_t* acc = hap_acc_get_by_aid(hap_get_unique_aid(std::to_string(obj->get_object_id_hash()).c_str()));
        if (acc) {
          hap_serv_t* hs = hap_acc_get_serv_by_uuid(acc, HAP_SERV_UUID_LIGHTBULB);
          hap_char_t* on_char = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_ON);
          hap_val_t state;
          state.b = obj->current_values.get_state();
          hap_char_update_val(on_char, &state);
          if (level) {
            hap_char_t* level_char = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_BRIGHTNESS);
            hap_val_t level;
            level.i = (int)(obj->current_values.get_brightness() * 100);
            hap_char_update_val(level_char, &level);
          }
          if (rgb) {
            hap_char_t* hue_char = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_HUE);
            hap_char_t* saturation_char = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_SATURATION);
            hap_val_t hue;
            hap_val_t saturation;
            int cHue = 0;
            float cSaturation = 0;
            float colorValue = 0;
            rgb_to_hsv(obj->current_values.get_red(), obj->current_values.get_green(), obj->current_values.get_blue(), cHue, cSaturation, colorValue);
            hue.f = cHue;
            saturation.f = cSaturation * 100;
            hap_char_update_val(hue_char, &hue);
            hap_char_update_val(saturation_char, &saturation);
          }
          if (temperature) {
            hap_char_t* temp_char = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_COLOR_TEMPERATURE);
            hap_val_t temp;
            temp.u = color_temp_mireds(obj);
            hap_char_update_val(temp_char, &temp);
          }
        }
      }
      static int acc_identify(hap_acc_t* ha) {
        ESP_LOGI(TAG, "Accessory identified");
        return HAP_SUCCESS;
      }
    public:
      LightEntity(light::LightState* lightPtr) : HAPEntity({{MODEL, "HAP-LIGHT"}}), lightPtr(lightPtr) {}
      void set_restore_switch(bool enable) { this->restore_switch_ = enable; }
      void set_pair_switch(bool enable) { this->pair_switch_ = enable; }
      void setup() {
        hap_acc_cfg_t acc_cfg = {
            .model = strdup(accessory_info[MODEL]),
            .manufacturer = strdup(accessory_info[MANUFACTURER]),
            .fw_rev = strdup(accessory_info[FW_REV]),
            .hw_rev = NULL,
            .pv = strdup("1.1.0"),
            .cid = HAP_CID_BRIDGE,
            .identify_routine = acc_identify,
        };
        hap_acc_t* accessory = nullptr;
        hap_serv_t* service = nullptr;
        std::string accessory_name = lightPtr->get_name();
        if (accessory_info[NAME] == NULL) {
          acc_cfg.name = strdup(accessory_name.c_str());
        }
        else {
          acc_cfg.name = strdup(accessory_info[NAME]);
        }
        if (accessory_info[SN] == NULL) {
          acc_cfg.serial_num = strdup(std::to_string(lightPtr->get_object_id_hash()).c_str());
        }
        else {
          acc_cfg.serial_num = strdup(accessory_info[SN]);
        }
        accessory = hap_acc_create(&acc_cfg);
        int hue = 0;
        float saturation = 0;
        float colorValue = 0;
        rgb_to_hsv(lightPtr->current_values.get_red(), lightPtr->current_values.get_green(), lightPtr->current_values.get_blue(), hue, saturation, colorValue);
        service = hap_serv_lightbulb_create(lightPtr->current_values.get_state());
        hap_serv_add_char(service, hap_char_name_create(accessory_name.data()));
        if (lightPtr->get_traits().supports_color_capability(light::ColorCapability::BRIGHTNESS)) {
          hap_serv_add_char(service, hap_char_brightness_create(lightPtr->current_values.get_brightness() * 100));
        }
        if (lightPtr->get_traits().supports_color_capability(light::ColorCapability::RGB)) {
          hap_serv_add_char(service, hap_char_hue_create(hue));
          hap_serv_add_char(service, hap_char_saturation_create(saturation * 100));
        }
        if (lightPtr->get_traits().supports_color_capability(light::ColorCapability::COLOR_TEMPERATURE) ||
            lightPtr->get_traits().supports_color_capability(light::ColorCapability::COLD_WARM_WHITE)) {
          hap_char_t* ct_char = hap_char_color_temperature_create(color_temp_mireds(lightPtr));
          int ct_min = 0;
          int ct_max = 0;
          color_temp_bounds(lightPtr, &ct_min, &ct_max);
          hap_char_int_set_constraints(ct_char, ct_min, ct_max, 1);
          hap_serv_add_char(service, ct_char);
        }
        ESP_LOGD(TAG, "ID HASH: %lu", lightPtr->get_object_id_hash());
        hap_serv_set_priv(service, lightPtr);

        /* Set the write callback for the service */
        hap_serv_set_write_cb(service, light_write);

        /* Add the Light Service to the Accessory Object */
        hap_acc_add_serv(accessory, service);

        if (this->restore_switch_) {
          hap_serv_t* restore = hap_serv_switch_create(RestoreAfterPowerLoss::is_enabled());
          hap_serv_add_char(restore, hap_char_name_create((char*) "Restore after power loss"));
          hap_serv_set_write_cb(restore, restore_switch_write);
          hap_acc_add_serv(accessory, restore);
        }
        if (this->pair_switch_) {
          hap_serv_t* pair = hap_serv_switch_create(false);
          hap_serv_add_char(pair, hap_char_name_create((char*) "Pair remote"));
          hap_serv_set_write_cb(pair, pair_switch_write);
          hap_acc_add_serv(accessory, pair);
          pair_on_char() = hap_serv_get_char_by_uuid(pair, HAP_CHAR_UUID_ON);
          ylkg_pair::YlkgPair::set_reflect(reflect_pair);
        }

        /* Add the Accessory to the HomeKit Database */
        hap_add_bridged_accessory(accessory, hap_get_unique_aid(std::to_string(lightPtr->get_object_id_hash()).c_str()));
        if (!lightPtr->is_internal())
          lightPtr->add_target_state_reached_listener(this);

        ESP_LOGI(TAG, "Light '%s' linked to HomeKit", accessory_name.c_str());
      }
    };
  }
}
#endif
