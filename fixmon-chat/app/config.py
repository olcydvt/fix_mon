"""
Configuration and the provider factory.

The one line that matters is `provider:` in config.yaml. Everything the model
actually reasons with - tool names, schemas, descriptions - sits above this
layer and does not change when that line does.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from .providers.anthropic import AnthropicProvider
from .providers.base import Provider
from .providers.gemini import GeminiProvider
from .providers.openai_compat import OpenAICompatProvider

_KINDS: dict[str, type[Provider]] = {
    "openai_compat": OpenAICompatProvider,
    "anthropic": AnthropicProvider,
    "gemini": GeminiProvider,
}


@dataclass
class Limits:
    max_tool_rounds: int = 6
    max_rows: int = 200
    request_timeout_s: int = 120


@dataclass
class Config:
    provider_name: str
    provider: Provider
    model: str
    fixmon_base_url: str
    fixmon_db_path: str
    limits: Limits
    audit_path: str


def load(path: str | Path = "config.yaml") -> Config:
    path = Path(path)
    raw: dict[str, Any] = yaml.safe_load(path.read_text(encoding="utf-8"))
    here = path.resolve().parent

    name = os.environ.get("FIXMON_CHAT_PROVIDER", raw["provider"])
    spec = raw["providers"].get(name)
    if spec is None:
        raise SystemExit(
            f"provider '{name}' is not defined in {path}. "
            f"Known: {sorted(raw['providers'])}"
        )

    kind = spec.get("kind")
    if kind not in _KINDS:
        raise SystemExit(f"provider '{name}' has unknown kind '{kind}'")

    key_env = spec["api_key_env"]
    api_key = os.environ.get(key_env, "")
    if not api_key:
        # Fail at startup rather than on the first question. A missing key that
        # only surfaces when an operator is mid-incident is the worst time to
        # find out.
        raise SystemExit(f"environment variable {key_env} is not set (needed by provider '{name}')")

    limits = Limits(**(raw.get("limits") or {}))

    provider = _KINDS[kind](
        base_url=spec["base_url"],
        model=spec["model"],
        api_key=api_key,
        timeout_s=limits.request_timeout_s,
    )

    fx = raw.get("fixmon") or {}
    return Config(
        provider_name=name,
        provider=provider,
        model=spec["model"],
        fixmon_base_url=fx.get("base_url", "http://127.0.0.1:9109"),
        # Left as written. An absolute path is the sane thing here anyway, and
        # silently re-anchoring one that is relative would point a reader at a
        # database that is not the one they meant.
        fixmon_db_path=fx.get("db_path", "./fixmon.db"),
        limits=limits,
        # Resolved against the config file rather than the working directory,
        # so the audit trail lands in the same place however the service was
        # started. An audit log that quietly moves when the unit file changes
        # is an audit log with a hole in it.
        audit_path=str(_anchor(here, (raw.get("audit") or {}).get("path", "./audit.log"))),
    )


def _anchor(base: Path, value: str) -> Path:
    p = Path(value)
    return p if p.is_absolute() else (base / p)


