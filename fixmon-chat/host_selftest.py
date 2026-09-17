"""End-to-end checks for the host: the agent loop, identity, and the audit trail.

The loop in main.py is the part no unit test touches and the part that is
easiest to get subtly wrong. It runs here against a scripted provider and a
temporary store, so no API key is needed and nothing reaches the network.

The scripted provider is the point. A real one would make these tests slow,
non-deterministic and dependent on a key; it would also make it impossible to
force the cases that matter - a model that loops forever, a model that calls a
tool that does not exist, a model that asks for two tools at once.
"""

from __future__ import annotations

import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The loader refuses to start without the configured provider's key, which is
# the behaviour we want in production and an obstacle here. A dummy satisfies
# it; the provider object it builds is replaced before any request is served.
os.environ.setdefault("DEEPSEEK_API_KEY", "not-used-by-this-test")

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


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

SESSIONS = [
    {
        "session_id": "FIX.4.4:BROKER1->VENUEX",
        "state": "logged_on",
        "msgs_in": 8390, "msgs_out": 8421,
        "next_expected_in": 8391, "last_outgoing_seq": 8421,
        "seq_gaps": 0, "seq_gaps_reported": 0, "seq_too_low": 0,
        "resend_requests": 0, "heartbeat_timeouts": 0, "disconnects": 0,
        "reconnect_attempts": 0, "logon_rejects": 0, "session_rejects": 0,
        "business_rejects": 0, "exec_rejects": 0,
        "last_incoming_ts_ns": 0, "clock_skew_ns": 1_200_000,
        "last_disconnect_reason": "", "last_reject_text": "",
        "config": {
            "connection_type": "initiator", "heartbeat_interval": 30,
            "source": "quickfix_config",
            "reset_on_logon": "no", "reset_on_logout": "no",
            "reset_on_disconnect": "no", "persist_messages": "yes",
            "seq_reset_policy": "persistent", "resend_capability": "full",
        },
    },
    {
        "session_id": "FIX.4.4:VENUEX->BROKER1",
        "state": "stale",
        "msgs_in": 8421, "msgs_out": 8390,
        "next_expected_in": 8422, "last_outgoing_seq": 8390,
        "seq_gaps": 2, "seq_gaps_reported": 2, "seq_too_low": 0,
        "resend_requests": 1, "heartbeat_timeouts": 1, "disconnects": 1,
        "reconnect_attempts": 1, "logon_rejects": 0, "session_rejects": 0,
        "business_rejects": 0, "exec_rejects": 0,
        "last_incoming_ts_ns": 0, "clock_skew_ns": 900_000,
        "last_disconnect_reason": "Socket exception", "last_reject_text": "",
        "config": {
            "connection_type": "acceptor", "heartbeat_interval": 30,
            "source": "quickfix_config",
            "reset_on_logon": "no", "reset_on_logout": "no",
            "reset_on_disconnect": "no", "persist_messages": "yes",
            "seq_reset_policy": "persistent", "resend_capability": "full",
        },
    },
]


def make_store(path: Path) -> None:
    conn = sqlite3.connect(path)
    conn.executescript(
        """
        CREATE TABLE events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts_ns INTEGER NOT NULL, ingest_ts_ns INTEGER NOT NULL,
            capture_ts_ns INTEGER, session_id TEXT NOT NULL,
            event_class TEXT NOT NULL, source TEXT NOT NULL,
            direction TEXT, msg_type TEXT, msg_type_name TEXT,
            msg_seq_num INTEGER, poss_dup INTEGER, poss_resend INTEGER,
            session_event TEXT, expected_seq INTEGER, received_seq INTEGER,
            reject_reason INTEGER, ref_tag_id INTEGER, ref_seq_num INTEGER,
            ord_status TEXT, text TEXT, raw TEXT
        );
        """
    )
    conn.commit()
    conn.close()


class ScriptedProvider:
    """Replays a fixed list of Completions and records what it was sent."""

    def __init__(self, script):
        self.script = list(script)
        self.calls = []

    def complete(self, history, tools, system):
        self.calls.append({
            "history": [dict(m) for m in history],
            "tools": tools,
            "system": system,
        })
        if self.script:
            return self.script.pop(0)
        from app.providers.base import Completion
        return Completion(text="(script exhausted)")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def main() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="fixmon-chat-test-"))
    db_path = tmp / "fixmon.db"
    make_store(db_path)

    import app.main as m
    from app.providers.base import Completion, ToolCall
    from fastapi.testclient import TestClient

    # Point the host at the fixtures instead of a live collector.
    m.fixmon.db_path = str(db_path)
    m.fixmon.sessions = lambda: list(SESSIONS)  # type: ignore[method-assign]
    m.audit.path = tmp / "audit.log"
    m.cfg.fixmon_db_path = str(db_path)

    client = TestClient(m.app)

    # -- the loop ----------------------------------------------------------
    section("agent loop")

    provider = ScriptedProvider([
        Completion(
            text="Let me look.",
            tool_calls=[ToolCall(id="c1", name="list_sessions", arguments={})],
            usage={"prompt": 100, "completion": 20},
        ),
        Completion(
            text="Two sessions. The acceptor side is stale with 2 sequence gaps.",
            usage={"prompt": 400, "completion": 40},
        ),
    ])
    m.cfg.provider = provider  # type: ignore[assignment]

    r = client.post(
        "/api/chat",
        json={"message": "list the sessions"},
        headers={"X-User": "aysegul"},
    )
    check(r.status_code == 200, "request succeeds")
    body = r.json()
    check(body["rounds"] == 2, "two rounds: one to ask for the tool, one to answer")
    check(body["tools_used"] == ["list_sessions"], "the tool actually ran")
    check("stale" in body["reply"], "the model's final text is what comes back")
    check(body["usage"] == {"prompt": 500, "completion": 60}, "token usage accumulates across rounds")

    # -- the trap that breaks tool use silently ----------------------------
    section("tools are re-sent every round")

    check(len(provider.calls) == 2, "the provider was called twice")
    check(
        all(c["tools"] for c in provider.calls),
        "schemas go out on BOTH rounds - omitting them on the second is the "
        "classic bug where the model 'forgets' its tools",
    )
    check(
        all(c["system"] for c in provider.calls),
        "and so does the system prompt",
    )

    second = provider.calls[1]["history"]
    roles = [msg["role"] for msg in second]
    check(
        roles == ["user", "assistant", "tool"],
        "the assistant turn carrying tool_calls is kept in history verbatim; "
        "dropping it orphans the tool result",
    )
    check(
        json.loads(second[2]["content"])["count"] == 2,
        "the tool result reaching the model is the real one",
    )

    # -- conversation continuity -------------------------------------------
    section("conversation continuity")

    conv_id = body["conversation_id"]
    m.cfg.provider = ScriptedProvider([Completion(text="The initiator side.")])
    r2 = client.post(
        "/api/chat",
        json={"message": "and the other one?", "conversation_id": conv_id},
        headers={"X-User": "aysegul"},
    )
    check(r2.status_code == 200, "follow-up in the same conversation works")
    hist = m.cfg.provider.calls[0]["history"]  # type: ignore[union-attr]
    check(
        hist[0]["content"] == "list the sessions",
        "the earlier turn is still in the history - these APIs keep no state, "
        "so continuity is entirely the host's job",
    )

    # -- identity -----------------------------------------------------------
    section("identity and isolation")

    r3 = client.post("/api/chat", json={"message": "hello"})
    check(r3.status_code == 401, "no identity, no answer")

    m.cfg.provider = ScriptedProvider([Completion(text="nope")])
    r4 = client.post(
        "/api/chat",
        json={"message": "show me", "conversation_id": conv_id},
        headers={"X-User": "someone-else"},
    )
    check(
        r4.status_code == 403,
        "another user's conversation id is refused, not served back to them",
    )

    # -- runaway ------------------------------------------------------------
    section("runaway model")

    looping = ScriptedProvider([
        Completion(tool_calls=[ToolCall(id=f"c{i}", name="list_sessions", arguments={})])
        for i in range(20)
    ])
    m.cfg.provider = looping  # type: ignore[assignment]
    r5 = client.post(
        "/api/chat",
        json={"message": "loop forever"},
        headers={"X-User": "aysegul"},
    )
    check(r5.status_code == 200, "a looping model still gets a response")
    check(
        r5.json()["rounds"] == m.cfg.limits.max_tool_rounds,
        "capped at max_tool_rounds rather than running until the tokens run out",
    )
    check(
        "narrow" in r5.json()["reply"].lower(),
        "and the reply says what happened instead of being empty",
    )

    # -- bad tool name ------------------------------------------------------
    section("model invents a tool")

    m.cfg.provider = ScriptedProvider([
        Completion(tool_calls=[ToolCall(id="x", name="get_everything", arguments={})]),
        Completion(text="I only have three tools."),
    ])
    r6 = client.post(
        "/api/chat",
        json={"message": "use a made up tool"},
        headers={"X-User": "aysegul"},
    )
    check(r6.status_code == 200, "an invented tool name does not crash the request")
    result = m.cfg.provider.calls[1]["history"][-1]["content"]  # type: ignore[union-attr]
    check("available" in result, "the model is told which tools do exist, so it can recover")

    # -- audit --------------------------------------------------------------
    section("audit trail")

    lines = [json.loads(x) for x in m.audit.path.read_text(encoding="utf-8").splitlines()]
    kinds = {line["kind"] for line in lines}
    check({"question", "tool_call", "model_round"} <= kinds, "all three record kinds present")

    questions = [x for x in lines if x["kind"] == "question"]
    check(
        all("user" in q and "conversation" in q for q in questions),
        "every question names who asked and in which conversation",
    )

    tool_calls = [x for x in lines if x["kind"] == "tool_call"]
    check(any(t["tool"] == "list_sessions" for t in tool_calls), "tool executions are recorded")
    check(
        all("args" in t for t in tool_calls),
        "with their arguments - that is what says which data was reached for",
    )
    check(
        not any("seq_gaps" in json.dumps(t) for t in tool_calls),
        "but not their results: a second copy of session data is the shadow "
        "record problem one layer up",
    )
    check(
        any(x.get("user") == "someone-else" for x in lines),
        "the refused attempt is recorded too - a denial is worth more to an "
        "auditor than a success",
    )

    # -- info ---------------------------------------------------------------
    section("info endpoint")

    info = client.get("/api/info", headers={"X-User": "aysegul"}).json()
    check(
        set(info["tools"]) == {"list_sessions", "get_session_diagnosis", "query_events"},
        "reports the tools actually registered",
    )
    check("provider" in info and "model" in info, "and which model is answering")

    print()
    if _failures:
        print(f"{_failures} check(s) failed")
        return 1
    print("all host checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

