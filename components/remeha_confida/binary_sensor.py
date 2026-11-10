#  binary_sensor platform: a read-only BOOLEAN value. Each entry binds one
#  appliance object (node/index/subindex + value_type + gain) to an HA
#  binary sensor, and is registered as a RemehaItem on the hub so update()
#  polls it. `read: single` fetches it with an individual request instead
#  of the bulk GetList.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from . import (
    remeha_ns, RemehaConfida, VALUE_TYPES, READ_MODES,
    CONF_REMEHA_ID, CONF_NODE, CONF_INDEX, CONF_SUBINDEX, CONF_READ,
    CONF_TRUTHY_CHECK, CONF_TRUE_VALUE
)

DEPENDENCIES = ["remeha_confida"]

CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema().extend(
        {
            cv.GenerateID(CONF_REMEHA_ID): cv.use_id(RemehaConfida),
            cv.Optional(CONF_NODE, default=1): cv.hex_uint8_t,
            cv.Required(CONF_INDEX): cv.hex_uint16_t,
            cv.Optional(CONF_SUBINDEX, default=0): cv.hex_uint8_t,
            cv.Optional(CONF_READ, default="bulk"): cv.enum(READ_MODES, lower=True),
            cv.Optional(CONF_TRUTHY_CHECK): cv.boolean,
            cv.Optional(CONF_TRUE_VALUE): cv.int_range(min=0,max=255),
        }
    ),
    cv.has_at_most_one_key(CONF_TRUTHY_CHECK, CONF_TRUE_VALUE)
)


async def to_code(config):
    # handle defaults for binary sensor checks here
    if CONF_TRUTHY_CHECK not in config:
        config[CONF_TRUTHY_CHECK] = True
    if CONF_TRUE_VALUE in config:
        config[CONF_TRUTHY_CHECK] = False
    else:
        config[CONF_TRUE_VALUE] = 1

    hub = await cg.get_variable(config[CONF_REMEHA_ID])
    sens = await binary_sensor.new_binary_sensor(config)
    item = cg.StructInitializer(
        remeha_ns.struct("RemehaItem"),
        ("node", config[CONF_NODE]),
        ("index", config[CONF_INDEX]),
        ("sub", config[CONF_SUBINDEX]),
        ("type", VALUE_TYPES["bool"]),
        ("binary_sensor", sens),
        ("truthy_check", config[CONF_TRUTHY_CHECK]),
        ("true_value", config[CONF_TRUE_VALUE]),
        ("single", config[CONF_READ]),
    )
    cg.add(hub.add_item(item))
