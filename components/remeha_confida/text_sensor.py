# ----------------------------------------------------------------------------
#  text_sensor platform: a read-only STRING value (e.g. the device name at
#  0x300F). The raw bytes are published verbatim up to the first NUL. value_type
#  is irrelevant here, so it is fixed to VT_U8 in the RemehaItem.
# ----------------------------------------------------------------------------
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from . import remeha_ns, RemehaConfida, READ_MODES, CONF_READ

CONF_REMEHA_ID = "remeha_confida_id"
CONF_NODE = "node"
CONF_INDEX = "index"
CONF_SUBINDEX = "subindex"

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.GenerateID(CONF_REMEHA_ID): cv.use_id(RemehaConfida),
        cv.Optional(CONF_NODE, default=1): cv.hex_uint8_t,
        cv.Required(CONF_INDEX): cv.hex_uint16_t,
        cv.Optional(CONF_SUBINDEX, default=0): cv.hex_uint8_t,
        cv.Optional(CONF_READ, default="bulk"): cv.enum(READ_MODES, lower=True),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_REMEHA_ID])
    ts = await text_sensor.new_text_sensor(config)
    item = cg.StructInitializer(
        remeha_ns.struct("RemehaItem"),
        ("node", config[CONF_NODE]),
        ("index", config[CONF_INDEX]),
        ("sub", config[CONF_SUBINDEX]),
        ("type", remeha_ns.enum("ValueType").VT_U8),
        ("gain", 1.0),
        ("sensor", cg.nullptr),
        ("text_sensor", ts),
        ("single", config[CONF_READ]),
    )
    cg.add(hub.add_item(item))
