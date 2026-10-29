import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_device_base
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32_ble_tracker"]
AUTO_LOAD = ["ble_device_base"]

ns = cg.esphome_ns.namespace("ble_gateway")
BleGateway = ns.class_("BleGateway", cg.Component)

CONF_RECEIVER = "receiver"
CONF_HOST = "host"
CONF_PORT = "port"
CONF_ENABLED = "enabled"
CONF_KEY = "key"


def _gateway_key(value):
    if value == cv.UNDEFINED:
        return value
    text = cv.string(value)
    digits = "".join(ch for ch in text if ch not in ": -")
    if len(digits) != 32 or any(ch not in "0123456789abcdefABCDEF" for ch in digits):
        raise cv.Invalid("receiver key must be 32 hex digits")
    return digits


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BleGateway),
        # Omit receiver and the relay stays off until set_ble_gateway.
        # enabled: true is what turns it on. A stored receiver replaces this.
        cv.Optional(CONF_RECEIVER): cv.Schema(
            {
                cv.Required(CONF_HOST): cv.ipv4address,
                cv.Required(CONF_PORT): cv.int_range(min=1, max=65535),
                cv.Optional(CONF_ENABLED, default=False): cv.boolean,
                cv.Optional(CONF_KEY): cv.sensitive(_gateway_key),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA).extend(ble_device_base.BLE_DEVICE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[ble_device_base.CONF_BLE_HUB_ID])
    cg.add(var.set_hub(hub))
    receiver = config.get(CONF_RECEIVER)
    if receiver is None:
        return
    cg.add(var.set_receiver(str(receiver[CONF_HOST]), receiver[CONF_PORT], receiver[CONF_ENABLED]))
    if CONF_KEY in receiver:
        cg.add(var.set_default_key(receiver[CONF_KEY]))
