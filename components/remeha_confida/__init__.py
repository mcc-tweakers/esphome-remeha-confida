# ============================================================================
#  remeha_confida - ESPHome external component (hub + shared definitions)
# ----------------------------------------------------------------------------
#  This file defines the HUB component (one per appliance): the BLE connection,
#  the GATT UUIDs and the pairing passkey. The per-value entities live in the
#  sibling platform files (sensor.py / text_sensor.py / number.py), which all
#  import the shared symbols defined here (value-type map, config keys, the
#  read-mode map). AUTO_LOAD pulls in those three platforms automatically.
# ============================================================================
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, esp32_ble, esp32_ble_tracker, number
# Import the core sensor/text_sensor modules under aliases: this component has its
# own sensor.py/text_sensor.py submodules, and those shadow the bare names once
# ESPHome imports them, which would break new_sensor()/new_text_sensor() below.
from esphome.components import sensor as core_sensor, text_sensor as core_text_sensor
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC, STATE_CLASS_TOTAL_INCREASING

CODEOWNERS = ["@diy"]
DEPENDENCIES = ["ble_client", "esp32_ble"]
AUTO_LOAD = ["sensor", "text_sensor", "number"]
MULTI_CONF = True

remeha_ns = cg.esphome_ns.namespace("remeha_confida")
RemehaConfida = remeha_ns.class_(
    "RemehaConfida", cg.PollingComponent, ble_client.BLEClientNode
)
RemehaNumber = remeha_ns.class_("RemehaNumber", number.Number, cg.Component)

# Shared value-type map (used by sensor + number platforms). Both the native
# tokens (u16/i16/...) and Modbus-style aliases (U_WORD/S_WORD/...) are accepted.
ValueType = remeha_ns.enum("ValueType")
VALUE_TYPES = {
    "u8": ValueType.VT_U8, "i8": ValueType.VT_I8,
    "u16": ValueType.VT_U16, "i16": ValueType.VT_I16,
    "u32": ValueType.VT_U32, "i32": ValueType.VT_I32,
    "enum": ValueType.VT_ENUM,
    "bool": ValueType.VT_BOOL,  # 0 / 0xFF flag -> 0 / 1
    # Modbus-style aliases for familiarity
    "u_byte": ValueType.VT_U8, "s_byte": ValueType.VT_I8,
    "u_word": ValueType.VT_U16, "s_word": ValueType.VT_I16,
    "u_dword": ValueType.VT_U32, "s_dword": ValueType.VT_I32,
    "raw": ValueType.VT_ENUM,
}

# Config keys shared across platforms
CONF_REMEHA_ID = "remeha_confida_id"
CONF_NODE = "node"
CONF_INDEX = "index"
CONF_SUBINDEX = "subindex"
CONF_TYPE = "value_type"
CONF_GAIN = "gain"
CONF_READ = "read"

# How an object is fetched. "bulk" = included in the single GetList round (default,
# cheapest). "single" = fetched with its own individual read request (operation 0x00),
# for objects the appliance does NOT return in the bulk GetList (typically scalar
# config parameters such as AP058/0x302B). Adding a value is YAML-only: set read: single.
READ_MODES = {"bulk": False, "single": True}

CONF_SERVICE_UUID = "service_uuid"
CONF_WRITE_UUID = "write_uuid"
CONF_NOTIFY_UUID = "notify_uuid"
CONF_PASSKEY = "passkey"
# Advertised-name prefix used to auto-discover the appliance when the ble_client
# MAC is left at 00:00:00:00:00:00. Confida/EHC modules advertise as "EHC-...".
CONF_DEVICE_NAME_PREFIX = "device_name_prefix"

# Optional hub-level DIAGNOSTIC entities (not tied to any appliance object):
#  - reconnect_reason: text_sensor showing WHY the link last reconnected
#    ("verbonden" while healthy, else e.g. "TX stuck in RUN"). Visible in HA
#    history/logbook, so you can see it afterwards without watching the live log.
#  - reconnect_count:  sensor counting reconnects since boot (total_increasing),
#    so you can see HOW OFTEN it happens.
CONF_RECONNECT_REASON = "reconnect_reason"
CONF_RECONNECT_COUNT = "reconnect_count"
# Optional: text_sensor with the result of the last parameter write
# ("OK: 0x3212:0 node 1" or "geweigerd 0x3212:0: waarde te laag (0x06090032)").
CONF_WRITE_RESULT = "write_result"


def _passkey(value):
    """The sticker number. Prefer it QUOTED in secrets.yaml ("012345"): an
    unquoted number with a leading zero is read by YAML as OCTAL (012345 -> 5349),
    which silently gives a wrong passkey."""
    if isinstance(value, str):
        v = value.strip()
        if not v.isdigit() or len(v) > 6:
            raise cv.Invalid("passkey must be the 6-digit sticker number, e.g. \"012345\"")
        return int(v, 10)
    return cv.int_range(min=0, max=999999)(value)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RemehaConfida),
            cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
            # 128-bit GATT UUIDs - get these once from an nRF Connect scan of your Confida.
            cv.Required(CONF_SERVICE_UUID): cv.string,
            cv.Required(CONF_WRITE_UUID): cv.string,
            cv.Required(CONF_NOTIFY_UUID): cv.string,
            # The number on the sticker (the BLE pairing passkey). Quote it in secrets.yaml.
            cv.Optional(CONF_PASSKEY): _passkey,
            # Auto-discovery name prefix (used only when the ble_client MAC is 0).
            cv.Optional(CONF_DEVICE_NAME_PREFIX, default="EHC-"): cv.string,
            cv.Optional(CONF_RECONNECT_REASON): core_text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:bluetooth-connect",
            ),
            cv.Optional(CONF_WRITE_RESULT): core_text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:pencil-box-outline",
            ),
            cv.Optional(CONF_RECONNECT_COUNT): core_sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:counter",
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    # Also registers this component as an esp32_ble_tracker scan-listener (at
    # build time), so parse_device() actually receives advertisements - needed
    # for auto-connect-by-name.
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    # Register as a scan-listener via codegen (not at runtime), so the tracker
    # delivers advertisements to parse_device().
    await esp32_ble_tracker.register_ble_device(var, config)
    ble_parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    esp32_ble.register_gap_event_handler(ble_parent, var)
    cg.add(var.set_service_uuid(config[CONF_SERVICE_UUID]))
    cg.add(var.set_write_uuid(config[CONF_WRITE_UUID]))
    cg.add(var.set_notify_uuid(config[CONF_NOTIFY_UUID]))
    if CONF_PASSKEY in config:
        cg.add(var.set_passkey(config[CONF_PASSKEY]))
    cg.add(var.set_name_prefix(config[CONF_DEVICE_NAME_PREFIX]))
    if CONF_RECONNECT_REASON in config:
        ts = await core_text_sensor.new_text_sensor(config[CONF_RECONNECT_REASON])
        cg.add(var.set_reason_sensor(ts))
    if CONF_WRITE_RESULT in config:
        wt = await core_text_sensor.new_text_sensor(config[CONF_WRITE_RESULT])
        cg.add(var.set_write_result_sensor(wt))
    if CONF_RECONNECT_COUNT in config:
        rs = await core_sensor.new_sensor(config[CONF_RECONNECT_COUNT])
        cg.add(var.set_reconnect_sensor(rs))
