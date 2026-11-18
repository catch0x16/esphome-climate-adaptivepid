"""Pick the heads on one outdoor unit and the rooms that receive hints."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_NAME
from homeassistant.core import callback
from homeassistant.helpers import selector

from .const import CONF_HEADS, CONF_HINTED, DEFAULT_NAME, DOMAIN


def _schema(defaults: dict[str, Any], with_name: bool) -> vol.Schema:
    fields: dict[Any, Any] = {}
    if with_name:
        fields[vol.Required(CONF_NAME, default=defaults.get(CONF_NAME, DEFAULT_NAME))] = selector.TextSelector()
    fields[vol.Required(CONF_HEADS, default=defaults.get(CONF_HEADS, []))] = selector.EntitySelector(
        selector.EntitySelectorConfig(domain="climate", multiple=True)
    )
    fields[vol.Optional(CONF_HINTED, default=defaults.get(CONF_HINTED, []))] = selector.EntitySelector(
        selector.EntitySelectorConfig(domain="climate", multiple=True)
    )
    return vol.Schema(fields)


def _validate(user_input: dict[str, Any]) -> dict[str, str]:
    errors: dict[str, str] = {}
    heads = user_input.get(CONF_HEADS, [])
    if len(heads) < 2:
        errors[CONF_HEADS] = "need_two_heads"
    elif any(h not in heads for h in user_input.get(CONF_HINTED, [])):
        errors[CONF_HINTED] = "hinted_not_in_heads"
    return errors


class SupervisorConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _validate(user_input)
            if not errors:
                await self.async_set_unique_id("_".join(sorted(user_input[CONF_HEADS])))
                self._abort_if_unique_id_configured()
                name = user_input.pop(CONF_NAME)
                return self.async_create_entry(title=name, data=user_input)
        return self.async_show_form(
            step_id="user", data_schema=_schema(user_input or {}, with_name=True), errors=errors
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return SupervisorOptionsFlow()


class SupervisorOptionsFlow(OptionsFlow):
    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _validate(user_input)
            if not errors:
                return self.async_create_entry(data=user_input)
        defaults = {**self.config_entry.data, **self.config_entry.options, **(user_input or {})}
        return self.async_show_form(step_id="init", data_schema=_schema(defaults, with_name=False), errors=errors)
