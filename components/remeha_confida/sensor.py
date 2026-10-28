# ----------------------------------------------------------------------------
#  sensor platform: a read-only NUMERIC value. Each entry binds one appliance
#  object (node/index/subindex + value_type + gain) to an HA sensor, and is
#  registered as a RemehaItem on the hub so update() polls it. `read: single`
#  fetches it with an individual request instead of the bulk GetList.
# ----------------------------------------------------------------------------
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from . import (
    remeha_ns, RemehaConfida, VALUE_TYPES, READ_MODES,
    CONF_REMEHA_ID, CONF_NODE, CONF_INDEX, CONF_SUBINDEX, CONF_TYPE, CONF_GAIN, CONF_READ,
)

DEPENDENCIES = ["remeha_confida"]

CONFIG_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.GenerateID(CONF_REMEHA_ID): cv.use_id(RemehaConfida),
        cv.Optional(CONF_NODE, default=1): cv.hex_uint8_t,
        cv.Required(CONF_INDEX): cv.hex_uint16_t,
        cv.Optional(CONF_SUBINDEX, default=0): cv.hex_uint8_t,
        cv.Required(CONF_TYPE): cv.enum(VALUE_TYPES, lower=True),
        cv.Optional(CONF_GAIN, default=1.0): cv.float_,
        cv.Optional(CONF_READ, default="bulk"): cv.enum(READ_MODES, lower=True),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_REMEHA_ID])
    sens = await sensor.new_sensor(config)
    item = cg.StructInitializer(
        remeha_ns.struct("RemehaItem"),
        ("node", config[CONF_NODE]),
        ("index", config[CONF_INDEX]),
        ("sub", config[CONF_SUBINDEX]),
        ("type", config[CONF_TYPE]),
        ("gain", config[CONF_GAIN]),
        ("sensor", sens),
        ("text_sensor", cg.nullptr),
        ("single", config[CONF_READ]),
    )
    cg.add(hub.add_item(item))
