"""Round-trip checks for the provider adapters.

History conversion is where the providers genuinely differ, so it is where the
bugs are. None of these call a network: the point is the shape of the request,
which is exactly the part that is wrong for months without anyone noticing
because the model usually copes.

What is being defended against, concretely:

  - a tool result that loses its pairing, so the next request is rejected or,
    worse, accepted with the result attached to the wrong call
  - consecutive tool results sent as separate messages, which Anthropic and
    Gemini both refuse - and which only happens once the model starts calling
    two tools in one turn, long after the first demo worked
  - the OpenAI family's arguments-as-a-string rule, applied in one direction
    but not the other
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from app.providers.anthropic import _to_wire as anthropic_wire  # noqa: E402
from app.providers.base import ToolCall  # noqa: E402
from app.providers.gemini import _to_wire as gemini_wire  # noqa: E402
from app.providers.gemini import _sanitise  # noqa: E402
from app.providers.openai_compat import _loads  # noqa: E402
from app.providers.openai_compat import _to_wire as openai_wire  # noqa: E402

_failures = 0


def check(cond: bool, label: str) -> None:
    global _failures
    if cond:
        print(f"  ok: {label}")
    else:
        _failures += 1
        print(f"  FAIL: {label}")


def section(name: str) -> None:
    print(f"\n[{name}]")


# A turn where the model asked for two different tools at once, which is the
# case that separates a working adapter from one that has only ever been tried
# with a single call.
HISTORY = [
    {"role": "user", "content": "what is wrong with VENUEX"},
    {
        "role": "assistant",
        "content": "Let me look.",
        "tool_calls": [
            ToolCall(id="call_1", name="list_sessions", arguments={}),
            ToolCall(
                id="call_2",
                name="get_session_diagnosis",
                arguments={"session_id": "FIX.4.4:BROKER1->VENUEX", "window": "1h"},
            ),
        ],
    },
    {"role": "tool", "tool_call_id": "call_1", "name": "list_sessions",
     "content": '{"count": 2}'},
    {"role": "tool", "tool_call_id": "call_2", "name": "get_session_diagnosis",
     "content": '{"seq_gaps": 3}'},
    {"role": "assistant", "content": "Three sequence gaps."},
    {"role": "user", "content": "and the other side?"},
]


def test_openai() -> None:
    section("openai-compatible")
    wire = [openai_wire(m) for m in HISTORY]

    roles = [m["role"] for m in wire]
    check(roles == ["user", "assistant", "tool", "tool", "assistant", "user"],
          "roles preserved, tool results stay separate messages")

    assistant = wire[1]
    check(len(assistant["tool_calls"]) == 2, "both calls survive the turn")
    check(
        all(isinstance(c["function"]["arguments"], str) for c in assistant["tool_calls"]),
        "arguments serialised to a string, as this family requires",
    )
    args = json.loads(assistant["tool_calls"][1]["function"]["arguments"])
    check(args["session_id"] == "FIX.4.4:BROKER1->VENUEX", "and they survive the round trip")

    check(wire[2]["tool_call_id"] == "call_1", "result 1 keeps its pairing")
    check(wire[3]["tool_call_id"] == "call_2", "result 2 keeps its pairing")

    check(_loads('{"a": 1}') == {"a": 1}, "string arguments parse")
    check(_loads({"a": 1}) == {"a": 1}, "object arguments pass through")
    check(_loads("not json") == {}, "malformed arguments degrade to empty, not an exception")
    check(_loads(None) == {}, "absent arguments degrade to empty")


def test_anthropic() -> None:
    section("anthropic")
    wire = anthropic_wire(HISTORY)

    roles = [m["role"] for m in wire]
    check(roles == ["user", "assistant", "user", "assistant", "user"],
          "the two tool results merge into one user turn")

    assistant = wire[1]
    kinds = [b["type"] for b in assistant["content"]]
    check(kinds == ["text", "tool_use", "tool_use"], "text then both tool_use blocks")

    results = wire[2]["content"]
    check(len(results) == 2, "both results in the one message")
    check(
        [b["tool_use_id"] for b in results] == ["call_1", "call_2"],
        "each result still names the call it answers",
    )
    check(
        assistant["content"][1]["input"] == {},
        "empty arguments stay an object rather than becoming null",
    )


def test_gemini() -> None:
    section("gemini")
    wire = gemini_wire(HISTORY)

    roles = [m["role"] for m in wire]
    check(roles == ["user", "model", "user", "model", "user"],
          "assistant becomes model; tool results merge into one user turn")

    parts = wire[1]["parts"]
    check("text" in parts[0], "leading text kept")
    check(
        [p["functionCall"]["name"] for p in parts[1:]]
        == ["list_sessions", "get_session_diagnosis"],
        "both calls present and named",
    )

    responses = wire[2]["parts"]
    check(len(responses) == 2, "both responses in the one turn")
    check(
        [p["functionResponse"]["name"] for p in responses]
        == ["list_sessions", "get_session_diagnosis"],
        "paired by name, which is all Gemini offers",
    )
    check(
        all(isinstance(p["functionResponse"]["response"], dict) for p in responses),
        "response is an object, never the raw string",
    )
    check(
        not any("tool_call_id" in json.dumps(p) for p in responses),
        "the synthetic id is not leaked into the request",
    )


def test_gemini_schema() -> None:
    section("gemini schema dialect")
    schema = {
        "type": "object",
        "additionalProperties": False,
        "title": "ignored",
        "properties": {
            "session_id": {"type": "string", "description": "id", "default": "x"},
            "window": {"type": "string", "enum": ["1h", "24h"]},
        },
        "required": ["session_id"],
    }
    clean = _sanitise(schema)
    check("additionalProperties" not in clean, "additionalProperties stripped")
    check("title" not in clean, "title stripped")
    check("default" not in clean["properties"]["session_id"], "nested default stripped")
    check(clean["properties"]["window"]["enum"] == ["1h", "24h"], "enum kept - it steers the model")
    check(clean["required"] == ["session_id"], "required kept")


def test_gemini_same_tool_twice() -> None:
    section("gemini: the documented limit")
    history = [
        {"role": "user", "content": "compare both sides"},
        {
            "role": "assistant",
            "content": "",
            "tool_calls": [
                ToolCall(id="a", name="get_session_diagnosis", arguments={"session_id": "A->B"}),
                ToolCall(id="b", name="get_session_diagnosis", arguments={"session_id": "B->A"}),
            ],
        },
        {"role": "tool", "tool_call_id": "a", "name": "get_session_diagnosis",
         "content": '{"side": "A->B"}'},
        {"role": "tool", "tool_call_id": "b", "name": "get_session_diagnosis",
         "content": '{"side": "B->A"}'},
    ]
    wire = gemini_wire(history)
    names = [p["functionResponse"]["name"] for p in wire[2]["parts"]]

    # Both responses carry the same name, so nothing in the request says which
    # is which. Order is the only signal, and it is preserved here. This is a
    # property of Gemini's format, not a defect in the adapter - the check
    # exists so that if it ever stops holding, it is noticed here rather than
    # in an answer that quietly swapped two sessions.
    check(names == ["get_session_diagnosis"] * 2, "same-tool responses both present")
    check(
        [p["functionResponse"]["response"]["side"] for p in wire[2]["parts"]]
        == ["A->B", "B->A"],
        "and in call order, which is the only thing that pairs them",
    )


def main() -> int:
    test_openai()
    test_anthropic()
    test_gemini()
    test_gemini_schema()
    test_gemini_same_tool_twice()

    print()
    if _failures:
        print(f"{_failures} check(s) failed")
        return 1
    print("all provider checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

