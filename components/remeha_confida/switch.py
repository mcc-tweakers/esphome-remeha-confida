#  switch platform: a read-write BOOLEAN value. Each entry binds one
#  appliance object (node/index/subindex + value_type) to an HA
#  switch, and is registered as a RemehaItem on the hub so update()
#  polls it. `read: single` fetches it with an individual request instead
#  of the bulk GetList.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from . import (
    remeha_ns, RemehaConfida, RemehaSwitch, VALUE_TYPES, READ_MODES,
    CONF_REMEHA_ID, CONF_NODE, CONF_INDEX, CONF_SUBINDEX, CONF_READ,
    CONF_TRUTHY_CHECK, CONF_TRUE_VALUE
)

DEPENDENCIES = ["remeha_confida"]

CONFIG_SCHEMA = cv.All(
    switch.switch_schema(RemehaSwitch).extend(
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
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    cg.add(var.set_parent(hub))
    cg.add(var.set_node(config[CONF_NODE]))
    cg.add(var.set_index(config[CONF_INDEX]))
    cg.add(var.set_sub(config[CONF_SUBINDEX]))

    item = cg.StructInitializer(
        remeha_ns.struct("RemehaItem"),
        ("node", config[CONF_NODE]),
        ("index", config[CONF_INDEX]),
        ("sub", config[CONF_SUBINDEX]),
        ("type", VALUE_TYPES["bool"]),
        ("switch_", var),
        ("truthy_check", config[CONF_TRUTHY_CHECK]),
        ("true_value", config[CONF_TRUE_VALUE]),
        ("single", config[CONF_READ]),
    )
    cg.add(hub.add_item(item))
