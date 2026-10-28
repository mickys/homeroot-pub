import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, ble_device_base, light, text_sensor
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client", "esp32_ble_tracker", "light"]
AUTO_LOAD = ["ble_client"]

ylkg_ns = cg.esphome_ns.namespace("ylkg_pair")
YlkgPair = ylkg_ns.class_(
    "YlkgPair",
    cg.Component,
    ble_client.BLEClientNode,
    ble_device_base.ESPBTDeviceListener,
)

CONF_PRODUCT_ID = "product_id"
CONF_KEY_SENSOR = "key_sensor"
CONF_CEILING = "ceiling"
CONF_NIGHT = "night"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(YlkgPair),
        cv.Required(CONF_PRODUCT_ID): cv.int_range(min=1, max=65535),
        cv.Optional(CONF_KEY_SENSOR): cv.use_id(text_sensor.TextSensor),
        cv.Required(CONF_CEILING): cv.use_id(light.LightState),
        cv.Required(CONF_NIGHT): cv.use_id(light.LightState),
    }
).extend(cv.COMPONENT_SCHEMA).extend(ble_client.BLE_CLIENT_SCHEMA).extend(
    ble_device_base.BLE_DEVICE_SCHEMA
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    await ble_device_base.register_ble_device(var, config)
    cg.add(var.set_product_id(config[CONF_PRODUCT_ID]))
    if CONF_KEY_SENSOR in config:
        sens = await cg.get_variable(config[CONF_KEY_SENSOR])
        cg.add(var.set_key_sensor(sens))
    ceiling = await cg.get_variable(config[CONF_CEILING])
    night = await cg.get_variable(config[CONF_NIGHT])
    cg.add(var.set_lights(ceiling, night))
