"""
Anthropic's Messages API.

Three differences from the OpenAI shape, all of them mechanical:
  - the system prompt is a top-level field, not a message
  - schemas go under `input_schema`, not `function.parameters`
  - a tool result is a *user* message carrying a tool_result block
"""

from __future__ import annotations

from typing import Any

import httpx

from .base import Completion, Provider, ToolCall

API_VERSION = "2023-06-01"


class AnthropicProvider(Provider):
    kind = "anthropic"

    def complete(self, history, tools, system) -> Completion:
        payload: dict[str, Any] = {
            "model": self.model,
            "max_tokens": 4096,
            "system": system,
            "messages": _to_wire(history),
        }
        if tools:
            payload["tools"] = [
                {
                    "name": t["name"],
                    "description": t["description"],
                    "input_schema": t["parameters"],
                }
                for t in tools
            ]

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

        text_parts: list[str] = []
        calls: list[ToolCall] = []
        for block in body.get("content", []):
            if block["type"] == "text":
                text_parts.append(block["text"])
            elif block["type"] == "tool_use":
                calls.append(
                    ToolCall(id=block["id"], name=block["name"], arguments=block.get("input") or {})
                )

        u = body.get("usage") or {}
        return Completion(
            text="".join(text_parts),
            tool_calls=calls,
            usage={"prompt": u.get("input_tokens", 0), "completion": u.get("output_tokens", 0)},
        )


def _to_wire(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """
    Neutral history -> Anthropic messages.

    Consecutive tool results have to be merged into one user message. Sending
    them separately is rejected, and it is the failure that shows up only once
    the model starts calling two tools in a single turn.
    """
    out: list[dict[str, Any]] = []

    for m in history:
        role = m["role"]

        if role == "tool":
            block = {
                "type": "tool_result",
                "tool_use_id": m["tool_call_id"],
                "content": m["content"],
            }
            if out and out[-1]["role"] == "user" and isinstance(out[-1]["content"], list):
                out[-1]["content"].append(block)
            else:
                out.append({"role": "user", "content": [block]})
            continue

        if role == "assistant" and m.get("tool_calls"):
            content: list[dict[str, Any]] = []
            if m.get("content"):
                content.append({"type": "text", "text": m["content"]})
            for c in m["tool_calls"]:
                content.append(
                    {"type": "tool_use", "id": c.id, "name": c.name, "input": c.arguments}
                )
            out.append({"role": "assistant", "content": content})
            continue

        out.append({"role": role, "content": m.get("content") or ""})

    return out

