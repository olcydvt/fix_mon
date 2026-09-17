"""
Conversation storage and the audit trail.

Both are per-user by construction rather than by care. A single shared history
would let one operator's question appear in another's context, which is a data
leak that looks like a bug and is easy to write by accident.

The audit trail is the reason for running our own host rather than pointing
people at an off-the-shelf client: who asked what, which tool ran, over whose
data. With a third-party app that record lives on the user's machine, if it
exists at all.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class Conversation:
    id: str
    user_id: str
    messages: list[dict[str, Any]] = field(default_factory=list)
    created_at: float = field(default_factory=time.time)
    updated_at: float = field(default_factory=time.time)


class ConversationStore:
    """
    In-memory, with a lock because a web server is multi-threaded.

    Deliberately not durable: these hold an operator's questions and the tool
    output that answered them, and keeping that forever recreates the retention
    problem the collector already had to solve. Swap in Redis or Postgres if
    conversations need to survive a restart, and give them an expiry when you do.
    """

    def __init__(self, max_messages: int = 100):
        self._data: dict[str, Conversation] = {}
        self._lock = threading.Lock()
        self._max = max_messages

    def get_or_create(self, conversation_id: str, user_id: str) -> Conversation:
        with self._lock:
            conv = self._data.get(conversation_id)
            if conv is None:
                conv = Conversation(id=conversation_id, user_id=user_id)
                self._data[conversation_id] = conv
            elif conv.user_id != user_id:
                # Someone guessed or reused an id. Refuse rather than serve
                # another user's history back to them.
                raise PermissionError("conversation belongs to another user")
            return conv

    def save(self, conv: Conversation) -> None:
        with self._lock:
            # Trim from the front, keeping whole turns. An assistant message
            # holding tool_calls must never be separated from the tool results
            # that answer it, or the next request is rejected by the provider.
            if len(conv.messages) > self._max:
                cut = len(conv.messages) - self._max
                while cut < len(conv.messages) and conv.messages[cut]["role"] == "tool":
                    cut += 1
                conv.messages = conv.messages[cut:]
            conv.updated_at = time.time()
            self._data[conv.id] = conv

    def reset(self, conversation_id: str) -> None:
        with self._lock:
            self._data.pop(conversation_id, None)


class AuditLog:
    """Append-only JSON lines. One line per tool execution and per model round."""

    def __init__(self, path: str):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()

    def write(self, **fields: Any) -> None:
        record = {"ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"), **fields}
        line = json.dumps(record, ensure_ascii=False, default=str)
        with self._lock:
            with self.path.open("a", encoding="utf-8") as f:
                f.write(line + "\n")

    def tool_call(
        self, *, user: str, conversation: str, tool: str, args: dict, ok: bool, ms: int
    ) -> None:
        # Arguments are recorded, results are not. The arguments say which data
        # was reached for; the results would put a copy of it in a second place,
        # which is the shadow-record problem again one layer up.
        self.write(
            kind="tool_call", user=user, conversation=conversation,
            tool=tool, args=args, ok=ok, duration_ms=ms,
        )

    def model_round(
        self, *, user: str, conversation: str, provider: str, model: str, usage: dict
    ) -> None:
        self.write(
            kind="model_round", user=user, conversation=conversation,
            provider=provider, model=model, usage=usage,
        )

    def question(self, *, user: str, conversation: str, text: str) -> None:
        self.write(kind="question", user=user, conversation=conversation, text=text)

