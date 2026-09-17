"""
OpenAI-compatible wire format: DeepSeek, OpenAI, vLLM, Ollama, LM Studio.

One adapter covers all of them because they agreed on a shape. The only trap is
that `arguments` arrives as a JSON *string* rather than an object, which is the
single most common source of "it works with Anthropic but not DeepSeek".
"""

from __future__ import annotations

import json
from typing import Any

import httpx

from .base import Completion, Provider, ToolCall


class OpenAICompatProvider(Provider):
    kind = "openai_compat"

    def complete(self, history, tools, system) -> Completion:
        messages: list[dict[str, Any]] = [{"role": "system", "content": system}]
        messages.extend(_to_wire(m) for m in history)

        payload: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
        }
        if tools:
            payload["tools"] = [
                {
                    "type": "function",
                    "function": {
                        "name": t["name"],
                        "description": t["description"],
                        "parameters": t["parameters"],
                    },
                }
                for t in tools
            ]
            payload["tool_choice"] = "auto"

        r = self._post(
            f"{self.base_url}/v1/chat/completions"
            if not self.base_url.endswith("/v1")
            else f"{self.base_url}/chat/completions",
            json=payload,
            headers={
                "Authorization": f"Bearer {self.api_key}",
                "Content-Type": "application/json",
            },
        )
        body = r.json()

        choice = body["choices"][0]["message"]
        calls: list[ToolCall] = []
        for c in choice.get("tool_calls") or []:
            fn = c["function"]
            calls.append(
                ToolCall(
                    id=c["id"],
                    name=fn["name"],
                    # A string, not an object. Parsed here so the rest of the
                    # application never has to know which provider it came from.
                    arguments=_loads(fn.get("arguments")),
                )
            )

        u = body.get("usage") or {}
        return Completion(
            text=choice.get("content") or "",
            tool_calls=calls,
            usage={
                "prompt": u.get("prompt_tokens", 0),
                "completion": u.get("completion_tokens", 0),
            },
        )


def _loads(raw: Any) -> dict[str, Any]:
    if isinstance(raw, dict):
        return raw
    if not raw:
        return {}
    try:
        parsed = json.loads(raw)
        return parsed if isinstance(parsed, dict) else {}
    except json.JSONDecodeError:
        # Let it through as an empty call; the registry will answer with the
        # expected parameters and the model usually corrects itself next round.
        return {}


def _to_wire(m: dict[str, Any]) -> dict[str, Any]:
    role = m["role"]

    if role == "tool":
        return {
            "role": "tool",
            "tool_call_id": m["tool_call_id"],
            "content": m["content"],
        }

    if role == "assistant" and m.get("tool_calls"):
        return {
            "role": "assistant",
            "content": m.get("content") or None,
            "tool_calls": [
                {
                    "id": c.id,
                    "type": "function",
                    "function": {"name": c.name, "arguments": json.dumps(c.arguments)},
                }
                for c in m["tool_calls"]
            ],
        }

    return {"role": role, "content": m.get("content") or ""}

