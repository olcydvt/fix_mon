"""Google Gemini's generateContent API.

The most divergent of the three, and the differences are not all cosmetic:

  - roles are `user` and `model`; there is no `assistant`
  - messages are `contents`, each holding a list of `parts`
  - the system prompt is `systemInstruction`, shaped like a content block
  - schemas live under `tools[].functionDeclarations[]`
  - a result goes back as a `functionResponse` part whose `response` must be a
    JSON *object*, where the other two accept a plain string

The one that is structural rather than cosmetic: **Gemini function calls carry
no id.** OpenAI has `tool_call_id` and Anthropic has `tool_use_id`, and the
application's neutral history is built around having one. Gemini matches a
response to its call by function *name* instead.

So ids are synthesised on the way out and dropped on the way in. That is fine
for a single call, and still fine for several calls in one turn as long as they
are to different tools. It breaks down only if the model calls the *same* tool
twice in one turn, because then two responses carry the same name and the
pairing is ambiguous - on Gemini's side, not ours. Nothing here can fix that;
what it can do is not pretend the id it invented means anything.
"""

from __future__ import annotations

import json
from typing import Any

import httpx

from .base import Completion, Provider, ToolCall

# Keywords Gemini's schema dialect rejects outright. Ours do not currently use
# any of them, but a schema edit upstream should degrade to a working call
# rather than a 400 from a provider the author was not testing against.
_UNSUPPORTED_SCHEMA_KEYS = {
    "additionalProperties",
    "$schema",
    "$id",
    "$ref",
    "definitions",
    "default",
    "examples",
    "title",
}


class GeminiProvider(Provider):
    kind = "gemini"

    def complete(self, history, tools, system) -> Completion:
        payload: dict[str, Any] = {
            "contents": _to_wire(history),
            "systemInstruction": {"parts": [{"text": system}]},
        }
        if tools:
            payload["tools"] = [
                {
                    "functionDeclarations": [
                        {
                            "name": t["name"],
                            "description": t["description"],
                            "parameters": _sanitise(t["parameters"]),
                        }
                        for t in tools
                    ]
                }
            ]

        # The key goes in the query string rather than a header. Which means it
        # can end up in an access log on anything sitting in front of this, so
        # it is worth knowing about if a proxy is ever introduced.
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

        candidates = body.get("candidates") or []
        if not candidates:
            # Usually a safety block. Say so rather than returning an empty
            # string that reads like the model had nothing to add.
            reason = (body.get("promptFeedback") or {}).get("blockReason")
            return Completion(
                text=f"The model returned no candidates{f' ({reason})' if reason else ''}.",
                usage=_usage(body),
            )

        parts = ((candidates[0].get("content") or {}).get("parts")) or []

        text_parts: list[str] = []
        calls: list[ToolCall] = []
        for part in parts:
            if "text" in part:
                text_parts.append(part["text"])
            elif "functionCall" in part:
                fc = part["functionCall"]
                name = fc.get("name", "")
                calls.append(
                    ToolCall(
                        # Synthetic, and only meaningful inside this process.
                        # _to_wire throws it away again; Gemini pairs by name.
                        id=f"gemini-{len(calls)}-{name}",
                        name=name,
                        arguments=fc.get("args") or {},
                    )
                )

        return Completion(
            text="".join(text_parts),
            tool_calls=calls,
            usage=_usage(body),
        )


def _usage(body: dict[str, Any]) -> dict[str, int]:
    u = body.get("usageMetadata") or {}
    return {
        "prompt": u.get("promptTokenCount", 0),
        "completion": u.get("candidatesTokenCount", 0),
    }


def _sanitise(schema: Any) -> Any:
    """Strips keywords Gemini's schema dialect will not accept."""
    if isinstance(schema, dict):
        return {
            k: _sanitise(v)
            for k, v in schema.items()
            if k not in _UNSUPPORTED_SCHEMA_KEYS
        }
    if isinstance(schema, list):
        return [_sanitise(v) for v in schema]
    return schema


def _to_wire(history: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Neutral history -> Gemini contents.

    Consecutive tool results merge into one content block, for the same reason
    they do on Anthropic: they answer one turn and are rejected when split
    across several.
    """
    out: list[dict[str, Any]] = []

    for m in history:
        role = m["role"]

        if role == "tool":
            part = {
                "functionResponse": {
                    # Paired by name. The tool_call_id the rest of the
                    # application carries has no counterpart here and is
                    # deliberately not sent.
                    "name": m["name"],
                    "response": _as_object(m["content"]),
                }
            }
            if out and out[-1]["role"] == "user" and _is_function_response(out[-1]):
                out[-1]["parts"].append(part)
            else:
                out.append({"role": "user", "parts": [part]})
            continue

        if role == "assistant" and m.get("tool_calls"):
            parts: list[dict[str, Any]] = []
            if m.get("content"):
                parts.append({"text": m["content"]})
            for c in m["tool_calls"]:
                parts.append({"functionCall": {"name": c.name, "args": c.arguments}})
            out.append({"role": "model", "parts": parts})
            continue

        out.append(
            {
                "role": "model" if role == "assistant" else "user",
                "parts": [{"text": m.get("content") or ""}],
            }
        )

    return out


def _is_function_response(content: dict[str, Any]) -> bool:
    parts = content.get("parts") or []
    return bool(parts) and all("functionResponse" in p for p in parts)


def _as_object(raw: str) -> dict[str, Any]:
    """`response` must be an object; our tool results are JSON text.

    A tool returning a JSON array or a bare value is wrapped rather than
    rejected - the model reads the content either way, and failing the whole
    request over the envelope would be a poor trade.
    """
    try:
        parsed = json.loads(raw)
    except (json.JSONDecodeError, TypeError):
        return {"result": raw}
    if isinstance(parsed, dict):
        return parsed
    return {"result": parsed}

