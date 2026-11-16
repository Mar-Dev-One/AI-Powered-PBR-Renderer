"""The in-app agent's settings: which model API and model it uses.

Each setting comes from the first of these that has it:

    1. the command line            --provider gemini
    2. the environment             PBR_AGENT_PROVIDER=gemini
    3. the settings file           ~/.pbr-renderer/agent.json  {"provider": "gemini"}
    4. the default                 claude

The file is there because the app starts the agent with the app's own
environment, which is whatever the program that started the app had: a
variable set after VS Code (or Explorer) started doesn't reach it until that
restarts. The file is read every time the agent starts.

    {
      "provider": "gemini",              // claude | gemini | ollama | openai
      "model": "gemini-3.8-flash",       // optional; each provider has a default
      "effort": "medium",                // Claude only: low | medium | high | xhigh | max
      "base_url": "http://localhost:1234/v1"   // provider "openai" only
    }

(JSON has no comments; they're only here to explain.) API keys are not read
from the file: they stay in the environment (ANTHROPIC_API_KEY,
GEMINI_API_KEY, ...), so the file can be shared or shown without leaking one.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

DEFAULT_SETTINGS_PATH = Path.home() / ".pbr-renderer" / "agent.json"

PROVIDERS = ("claude", "gemini", "ollama", "openai")
EFFORTS = ("low", "medium", "high", "xhigh", "max")

# setting -> its environment variable
ENVIRONMENT = {
    "provider": "PBR_AGENT_PROVIDER",
    "model": "PBR_AGENT_MODEL",
    "effort": "PBR_AGENT_EFFORT",
    "base_url": "PBR_AGENT_BASE_URL",
}

DEFAULTS = {"provider": "claude", "model": None, "effort": "medium", "base_url": None}


class SettingsError(Exception):
    """The settings file can't be used; the message says why."""


@dataclass
class Setting:
    value: Any
    source: str   # "command line", "$PBR_AGENT_PROVIDER", the file's path, or "default"


def read_file(path: Path) -> dict:
    """The settings file's values ({} if there is no file)."""
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return {}
    except OSError as error:
        raise SettingsError(f"can't read {path}: {error}") from error
    try:
        values = json.loads(text)
    except json.JSONDecodeError as error:
        raise SettingsError(f"{path} isn't valid JSON: {error}") from error
    if not isinstance(values, dict):
        raise SettingsError(f"{path} should hold a JSON object, like {{\"provider\": \"gemini\"}}")

    unknown = sorted(set(values) - set(DEFAULTS))
    if unknown:
        raise SettingsError(f"{path}: unknown setting(s) {', '.join(unknown)}; "
                            f"the settings are {', '.join(DEFAULTS)}")
    for name, value in values.items():
        if value is not None and not isinstance(value, str):
            raise SettingsError(f"{path}: \"{name}\" should be a string")
    if values.get("provider") not in (None, *PROVIDERS):
        raise SettingsError(f"{path}: \"provider\" must be one of {', '.join(PROVIDERS)}")
    if values.get("effort") not in (None, *EFFORTS):
        raise SettingsError(f"{path}: \"effort\" must be one of {', '.join(EFFORTS)}")
    return values


def resolve(command_line: dict, *, environ: dict | None = None, path: Path | None = None,
            use_file: bool = True) -> dict[str, Setting]:
    """Every setting with where it came from. `command_line` holds what was
    given on the command line (None = not given). Raises SettingsError for a
    broken file, or a provider / effort that doesn't exist, wherever it came from."""
    environ = os.environ if environ is None else environ
    path = DEFAULT_SETTINGS_PATH if path is None else path
    from_file = read_file(path) if use_file else {}

    settings = {}
    for name, default in DEFAULTS.items():
        variable = ENVIRONMENT[name]
        if command_line.get(name) is not None:
            settings[name] = Setting(command_line[name], "command line")
        elif environ.get(variable):
            settings[name] = Setting(environ[variable], f"${variable}")
        elif from_file.get(name) is not None:
            settings[name] = Setting(from_file[name], str(path))
        else:
            settings[name] = Setting(default, "default")

    for name, allowed in (("provider", PROVIDERS), ("effort", EFFORTS)):
        if settings[name].value not in allowed:
            raise SettingsError(f"{name} \"{settings[name].value}\" (from {settings[name].source}) "
                                f"must be one of {', '.join(allowed)}")
    return settings
