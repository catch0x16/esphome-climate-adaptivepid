"""Pick the heads on one outdoor unit, in order of precedence, then the rooms that receive hints."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_NAME
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import selector

from .const import CONF_HEADS, CONF_HINTED, CONF_OUTDOOR, DEFAULT_NAME, DOMAIN


def _heads_schema(defaults: dict[str, Any], with_name: bool) -> vol.Schema:
    fields: dict[Any, Any] = {}
    if with_name:
        fields[vol.Required(CONF_NAME, default=defaults.get(CONF_NAME, DEFAULT_NAME))] = selector.TextSelector()
    # The order is the precedence, highest first: drag to reorder.
    fields[vol.Required(CONF_HEADS, default=defaults.get(CONF_HEADS, []))] = selector.EntitySelector(
        selector.EntitySelectorConfig(domain="climate", multiple=True, reorder=True)
    )
    fields[vol.Optional(CONF_OUTDOOR, description={"suggested_value": defaults.get(CONF_OUTDOOR)})] = (
        selector.EntitySelector(selector.EntitySelectorConfig(domain="sensor", device_class="temperature"))
    )
    return vol.Schema(fields)


def _hinted_schema(hass: HomeAssistant, heads: list[str], hinted: list[str]) -> vol.Schema:
    """A checkbox for each head picked in the first step."""
    options = []
    for head in heads:
        state = hass.states.get(head)
        options.append(selector.SelectOptionDict(value=head, label=state.name if state else head))
    return vol.Schema(
        {
            vol.Optional(CONF_HINTED, default=[h for h in hinted if h in heads]): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=options, multiple=True, mode=selector.SelectSelectorMode.LIST
                )
            )
        }
    )


def _validate_heads(user_input: dict[str, Any]) -> dict[str, str]:
    if len(user_input.get(CONF_HEADS, [])) < 2:
        return {CONF_HEADS: "need_two_heads"}
    return {}


def in_precedence_order(heads: list[str], hinted: list[str]) -> list[str]:
    """The hinted rooms, in the order of the heads (their precedence)."""
    return [head for head in heads if head in hinted]


class SupervisorConfigFlow(ConfigFlow, domain=DOMAIN):
    # 2: the order of the heads is their precedence (see async_migrate_entry).
    VERSION = 2

    def __init__(self) -> None:
        self._data: dict[str, Any] = {}

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _validate_heads(user_input)
            if not errors:
                await self.async_set_unique_id("_".join(sorted(user_input[CONF_HEADS])))
                self._abort_if_unique_id_configured()
                self._data = dict(user_input)
                return await self.async_step_hinted()
        return self.async_show_form(
            step_id="user", data_schema=_heads_schema(user_input or {}, with_name=True), errors=errors
        )

    async def async_step_hinted(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        heads = self._data[CONF_HEADS]
        if user_input is not None:
            data = {key: value for key, value in self._data.items() if key != CONF_NAME}
            data[CONF_HINTED] = in_precedence_order(heads, user_input.get(CONF_HINTED, []))
            return self.async_create_entry(title=self._data[CONF_NAME], data=data)
        return self.async_show_form(step_id="hinted", data_schema=_hinted_schema(self.hass, heads, []))

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return SupervisorOptionsFlow()


class SupervisorOptionsFlow(OptionsFlow):
    def __init__(self) -> None:
        self._data: dict[str, Any] = {}

    def _current(self) -> dict[str, Any]:
        return {**self.config_entry.data, **self.config_entry.options}

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            errors = _validate_heads(user_input)
            if not errors:
                self._data = dict(user_input)
                return await self.async_step_hinted()
        defaults = {**self._current(), **(user_input or {})}
        return self.async_show_form(
            step_id="init", data_schema=_heads_schema(defaults, with_name=False), errors=errors
        )

    async def async_step_hinted(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        heads = self._data[CONF_HEADS]
        if user_input is not None:
            data = dict(self._data)
            data[CONF_HINTED] = in_precedence_order(heads, user_input.get(CONF_HINTED, []))
            return self.async_create_entry(data=data)
        return self.async_show_form(
            step_id="hinted", data_schema=_hinted_schema(self.hass, heads, self._current().get(CONF_HINTED, []))
        )
