"""
The tool layer, deliberately knowing nothing about any model provider.

Everything that decides answer quality lives here: the tool names, their JSON
Schemas, and above all the description text. That text is not documentation -
it is fed to the model verbatim and becomes part of its reasoning. A provider
swap must never put it at risk, which is why this module has no import from
anything under providers/.

The other half of the split is security. Arguments come from the model and are
untrusted; identity comes from the session and is not. They are kept in
separate parameters so the two can never be confused: `args` is whatever the
model produced, `ctx` is whatever the backend knows.
"""

from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass, field
from typing import Any, Callable, Protocol


@dataclass(frozen=True)
class User:
    """Who is asking. Established by the backend, never by the model."""

    id: str
    name: str
    # Session ids this user may see. Empty means all of them; a real
    # deployment fills this from the directory or an access table.
    allowed_sessions: frozenset[str] = frozenset()

    def can_see(self, session_id: str) -> bool:
        return not self.allowed_sessions or session_id in self.allowed_sessions


@dataclass
class ToolContext:
    """
    Per-request state handed to a tool.

    Not a global, on purpose. Two operators asking different questions at the
    same moment get two of these, and the sqlite connection inside is theirs
    alone - sqlite3 connection objects are not safe to share across threads.
    """

    user: User
    db: sqlite3.Connection
    fixmon: Any  # FixmonClient; typed loosely to keep this module dependency-free
    max_rows: int = 200


class ToolFn(Protocol):
    def __call__(self, *, ctx: ToolContext, **kwargs: Any) -> Any: ...


@dataclass(frozen=True)
class Tool:
    """
    One callable the model may ask for.

    `parameters` is plain JSON Schema. Every provider accepts that shape; only
    the envelope around it differs, and that is the adapters' problem.
    """

    name: str
    description: str
    parameters: dict[str, Any]
    fn: ToolFn

    def schema(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "description": self.description,
            "parameters": self.parameters,
        }


class ToolError(Exception):
    """
    A failure the model is expected to read and recover from.

    Raised rather than returned so a tool body can bail out mid-way, but always
    converted back into a normal tool result: an exception that reaches the
    model as a stack trace teaches it nothing, while "no such session, here are
    the ones that exist" gets the next call right.
    """


@dataclass
class ToolRegistry:
    tools: list[Tool] = field(default_factory=list)

    def __post_init__(self) -> None:
        self._by_name = {t.name: t for t in self.tools}
        if len(self._by_name) != len(self.tools):
            raise ValueError("two tools share a name")

    def schemas(self) -> list[dict[str, Any]]:
        """Neutral schemas. Adapters translate; nothing here is provider shaped."""
        return [t.schema() for t in self.tools]

    def execute(self, name: str, args: dict[str, Any], ctx: ToolContext) -> str:
        """
        Run one tool and return what the model will see.

        Always a string, always JSON, never an exception. A tool that blows up
        must still produce a turn the conversation can continue from - the
        alternative is a dead conversation where the operator is told nothing.
        """
        tool = self._by_name.get(name)
        if tool is None:
            return _json({
                "error": f"unknown tool: {name}",
                "available": sorted(self._by_name),
            })

        if not isinstance(args, dict):
            return _json({"error": "arguments must be an object", "received": repr(args)})

        try:
            result = tool.fn(ctx=ctx, **args)
        except ToolError as e:
            return _json({"error": str(e)})
        except TypeError as e:
            # Almost always the model inventing or omitting a parameter.
            return _json({
                "error": f"bad arguments: {e}",
                "expected": tool.parameters.get("properties", {}),
            })
        except Exception as e:  # noqa: BLE001 - the model gets a usable message
            return _json({"error": f"{type(e).__name__}: {e}"})

        return _json(result)


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, default=str)

