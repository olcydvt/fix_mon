"""
Exercises the tools with no model and no API key.

Worth having for the same reason the collector has a selftest: most of what can
go wrong here is ordinary software - a column renamed, a filter that matches
nothing, a session id that will not resolve - and finding that out by spending
tokens and reading prose is a slow way to debug a SQL typo.

    python tools_selftest.py
"""

from __future__ import annotations

import json
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from app.fixmon import resolve_session, rows_to_dicts  # noqa: E402
from app.registry import Tool, ToolContext, ToolRegistry, User  # noqa: E402
from app.tools import ALL_TOOLS  # noqa: E402

FAILURES = 0


def check(cond: bool, msg: str) -> None:
    global FAILURES
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}")
        FAILURES += 1


# --- the collector's schema, kept in step with src/event_store.cpp ----------

SCHEMA = """
CREATE TABLE events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_ns INTEGER NOT NULL, ingest_ts_ns INTEGER NOT NULL, capture_ts_ns INTEGER,
  session_id TEXT NOT NULL, event_class TEXT NOT NULL, source TEXT NOT NULL,
  direction TEXT, msg_type TEXT, msg_type_name TEXT, msg_seq_num INTEGER,
  poss_dup INTEGER, poss_resend INTEGER, session_event TEXT,
  expected_seq INTEGER, received_seq INTEGER, reject_reason INTEGER,
  ref_tag_id INTEGER, ref_seq_num INTEGER, ord_status TEXT, text TEXT, raw TEXT
);
CREATE INDEX idx_events_session_ts ON events(session_id, ts_ns);
"""

SID = "FIX.4.4:BROKER1->VENUEX"
MIRROR = "FIX.4.4:VENUEX->BROKER1"


class FakeFixmon:
    """Stands in for the collector so the suite needs nothing running."""

    def __init__(self, db_path: str):
        self.db_path = db_path

    def sessions(self):
        return [
            {
                "session_id": SID, "state": "logged_on",
                "msgs_in": 8390, "msgs_out": 8421,
                "next_expected_in": 8391, "last_outgoing_seq": 8421,
                "seq_gaps": 1, "seq_gaps_reported": 1, "seq_too_low": 0,
                "resend_requests": 1, "heartbeat_timeouts": 1, "disconnects": 1,
                "reconnect_attempts": 1, "logon_rejects": 0, "session_rejects": 1,
                "business_rejects": 0, "exec_rejects": 0,
                "clock_skew_ns": -1_200_000_000,
                "last_disconnect_reason": "Socket exception",
                "last_reject_text": "Invalid password for user <redacted>",
                "config": {
                    "connection_type": "initiator", "heartbeat_interval": 30,
                    "source": "quickfix_config",
                    "reset_on_logon": "no", "reset_on_logout": "no",
                    "reset_on_disconnect": "no", "persist_messages": "yes",
                    "seq_reset_policy": "persistent", "resend_capability": "full",
                },
            },
            {
                "session_id": MIRROR, "state": "stale",
                "msgs_in": 8421, "msgs_out": 8390,
                "next_expected_in": 8422, "last_outgoing_seq": 8390,
                "seq_gaps": 0, "disconnects": 1, "session_rejects": 0,
                "last_disconnect_reason": "", "last_reject_text": "",
                "config": {
                    "connection_type": "acceptor",
                    "seq_reset_policy": "unknown", "resend_capability": "unknown",
                },
            },
        ]

    def connect(self):
        conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True)
        conn.row_factory = sqlite3.Row
        return conn


def seed(path: str) -> None:
    now = time.time()
    conn = sqlite3.connect(path)
    conn.executescript(SCHEMA)

    def ev(offset_s, **kw):
        ts = int((now - offset_s) * 1e9)
        cols = {
            "ts_ns": ts, "ingest_ts_ns": ts, "session_id": SID,
            "event_class": "session_event", "source": "event_log",
        }
        cols.update(kw)
        conn.execute(
            f"INSERT INTO events ({','.join(cols)}) VALUES ({','.join('?' * len(cols))})",
            list(cols.values()),
        )

    ev(300, session_event="seq_num_too_high", expected_seq=4471, received_seq=4478,
       text="MsgSeqNum too high, expecting 4471 but received 4478")
    ev(299, event_class="fix_message", source="message_log", msg_type="2",
       msg_type_name="ResendRequest", direction="outgoing", session_event=None)
    ev(277, session_event="heartbeat_timeout", text="Test Request timed out")
    ev(275, session_event="disconnected", text="Socket exception, connection reset by peer")
    ev(245, session_event="logon", text="Received logon response")
    ev(240, event_class="fix_message", source="message_log", msg_type="3",
       msg_type_name="Reject", direction="incoming", session_event=None,
       reject_reason=1, ref_seq_num=12, text="Required tag missing",
       raw="8=FIX.4.4|35=3|34=12|58=Required tag missing|")
    # Noise that must not appear in the diagnosis timeline.
    for i in range(20):
        ev(100 + i, event_class="fix_message", source="message_log", msg_type="0",
           msg_type_name="Heartbeat", direction="incoming", session_event=None)

    conn.commit()
    conn.close()


def main() -> int:
    tmp = Path(tempfile.mkdtemp())
    db = str(tmp / "fixmon.db")
    seed(db)

    fake = FakeFixmon(db)
    registry = ToolRegistry(ALL_TOOLS)
    conn = fake.connect()
    ctx = ToolContext(user=User(id="u1", name="op"), db=conn, fixmon=fake)

    print("\n[registry]")
    names = [s["name"] for s in registry.schemas()]
    check(set(names) == {"list_sessions", "get_session_diagnosis", "query_events"},
          "three tools registered")
    for s in registry.schemas():
        check(len(s["description"]) > 200,
              f"{s['name']}: description carries real guidance, not a label")
        check(s["parameters"]["type"] == "object", f"{s['name']}: schema is an object")
        check("user_id" not in s["parameters"].get("properties", {}),
              f"{s['name']}: no identity parameter the model could fill in")

    print("\n[list_sessions]")
    out = json.loads(registry.execute("list_sessions", {}, ctx))
    check(out["count"] == 2, "both sessions listed")
    check(out["sessions"][0]["seq_reset_policy"] == "persistent",
          "policy travels with the summary, so no second call is needed to judge a gap")
    filtered = json.loads(registry.execute("list_sessions", {"state": "stale"}, ctx))
    check(filtered["count"] == 1 and filtered["sessions"][0]["session_id"] == MIRROR,
          "state filter narrows correctly")

    print("\n[session id resolution]")
    ids = [SID, MIRROR]
    check(resolve_session(ids, SID)[0] == SID, "exact id matches")
    check(resolve_session(ids, "BROKER1->VENUEX")[0] == SID, "unique fragment resolves")
    resolved, alts = resolve_session(ids, "FIX.4.4")
    check(resolved is None and len(alts) == 2,
          "ambiguous fragment returns candidates instead of guessing")

    print("\n[get_session_diagnosis]")
    diag = json.loads(registry.execute(
        "get_session_diagnosis", {"session_id": "VENUEX", "window": "1h"}, ctx))
    check("error" not in diag, "a bare counterparty name is enough to identify the session")
    check(diag["session_id"] == SID, "resolved to the full id")
    check(diag["config"]["seq_reset_policy"] == "persistent", "config included")
    check(diag["mirror_session"] is not None, "the other half of the link was found")
    check(diag["mirror_session"]["session_id"] == MIRROR, "and it is the right one")

    tl = diag["timeline"]
    check(len(tl) > 0, "timeline is not empty")
    check(all(tl[i]["ts_ns"] <= tl[i + 1]["ts_ns"] for i in range(len(tl) - 1)),
          "oldest first - causation reads forwards")
    kinds = [r["session_event"] for r in tl if r["session_event"]]
    check("seq_num_too_high" in kinds and "disconnected" in kinds,
          "session-layer events present")
    check(kinds.index("seq_num_too_high") < kinds.index("disconnected"),
          "the gap precedes the disconnect, which is what decides the causal story")
    check(not any(r["msg_type"] == "0" for r in tl),
          "heartbeats filtered out - they would crowd the context for nothing")
    check(any(r["msg_type"] == "3" for r in tl), "but rejects are kept")

    print("\n[query_events]")
    q = json.loads(registry.execute(
        "query_events", {"session_id": SID, "msg_type": "3"}, ctx))
    check(q["count"] == 1, "msg_type filter works")
    check("<redacted>" not in (q["events"][0]["raw"] or ""), "this row had nothing to mask")
    q2 = json.loads(registry.execute(
        "query_events", {"session_id": SID, "event_type": "disconnected"}, ctx))
    check(q2["count"] == 1, "session event filter works")

    print("\n[failure handling]")
    bad = json.loads(registry.execute("get_session_diagnosis", {"session_id": "NOPE"}, ctx))
    check("error" in bad, "unknown session reports an error")
    check("Candidates" in bad["error"] or "candidates" in bad["error"],
          "and names what does exist, so the next call can succeed")
    missing = json.loads(registry.execute("get_session_diagnosis", {}, ctx))
    check("error" in missing and "expected" in missing,
          "a missing argument returns the expected shape rather than a stack trace")
    unknown = json.loads(registry.execute("no_such_tool", {}, ctx))
    check("available" in unknown, "an invented tool name lists the real ones")

    print("\n[authorisation]")
    limited = ToolContext(user=User(id="u2", name="op2",
                                    allowed_sessions=frozenset({MIRROR})),
                          db=conn, fixmon=fake)
    scoped = json.loads(registry.execute("list_sessions", {}, limited))
    check(scoped["count"] == 1 and scoped["sessions"][0]["session_id"] == MIRROR,
          "a user sees only their sessions")
    denied = json.loads(registry.execute(
        "get_session_diagnosis", {"session_id": SID}, limited))
    check("error" in denied,
          "and cannot reach another session by naming it directly")

    conn.close()
    print()
    if FAILURES:
        print(f"{FAILURES} FAILURE(S)")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

