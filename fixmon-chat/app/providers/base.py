"""
The provider boundary.

Conversation history is kept in a neutral shape and converted on every request.
That costs a little work per call and buys two things: the provider can be
changed from a config line without migrating stored conversations, and a
conversation that started on one model can finish on another.

The neutral message shapes:

    {"role": "user",      "content": "..."}
    {"role": "assistant", "content": "...", "tool_calls": [ToolCall, ...]}
    {"role": "tool",      "tool_call_id": "...", "name": "...", "content": "..."}
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlparse

import httpx

# Hosts that can only mean "this machine". A proxy is never the right way to
# reach any of them.
_LOOPBACK = {"localhost", "127.0.0.1", "::1", "0.0.0.0"}


@dataclass
class ToolCall:
    id: str
    name: str
    arguments: dict[str, Any]


@dataclass
class Completion:
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    # Prompt/completion tokens, for the audit line. Providers disagree on the
    # field names, so each adapter normalises into this.
    usage: dict[str, int] = field(default_factory=dict)

    @property
    def wants_tools(self) -> bool:
        return bool(self.tool_calls)

    def as_history(self) -> dict[str, Any]:
        return {"role": "assistant", "content": self.text, "tool_calls": self.tool_calls}


class Provider(ABC):
    """One request/response round with a model. No loop, no state."""

    def __init__(self, *, base_url: str, model: str, api_key: str, timeout_s: int = 120):
        self.base_url = base_url.rstrip("/")
        self.model = model
        self.api_key = api_key
        self.timeout_s = timeout_s

    @property
    def _is_local(self) -> bool:
        return (urlparse(self.base_url).hostname or "") in _LOOPBACK

    def _post(self, url: str, *, json: dict[str, Any], headers: dict[str, str]) -> httpx.Response:
        """
        One POST to the model endpoint.

        trust_env is turned off for loopback targets. A corporate proxy picked up
        from the environment will happily intercept 127.0.0.1 and answer with its
        own login page, which arrives as a 302 to something that is not a model at
        all. Relying on no_proxy being set correctly is the fragile version of
        this: the failure then depends on the shell that started the process, so
        it appears and disappears between a terminal and a service unit.

        It stays on for remote providers, because reaching DeepSeek or Anthropic
        through the corporate proxy is exactly what is wanted there.

        Redirects are not followed, and that is the security-relevant half. A
        redirect here means the request did not reach the intended server;
        following it would post the conversation - operator questions and session
        data - to whatever answered instead.
        """
        with httpx.Client(
                trust_env=not self._is_local,
                follow_redirects=False,
                timeout=self.timeout_s,
        ) as client:
            r = client.post(url, json=json, headers=headers)

        if r.is_redirect:
            raise RuntimeError(
                f"{url} answered {r.status_code} redirecting to "
                f"{r.headers.get('location', '?')!r}. The request was intercepted "
                f"before it reached the model - check proxy settings."
            )

        r.raise_for_status()
        return r

    @abstractmethod
    def complete(
            self,
            history: list[dict[str, Any]],
            tools: list[dict[str, Any]],
            system: str,
    ) -> Completion:
        """
        `tools` arrives in the neutral schema form produced by ToolRegistry.

        Note that it is passed on every call, not registered once: these APIs
        are stateless and a request without it is a request where the model has
        no tools at all.
        """