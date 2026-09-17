"""
The tools themselves.

The description strings here are the product. They are sent to the model on
every request and become part of how it reasons, so they carry the things a
model cannot know from the data alone: that a sequence gap means nothing under
one configuration and means lost messages under another, that two sessions may
be two halves of one connection, and above all what to do when the answer is
"unknown" - which is to say so rather than to fall back on the textbook.
"""

from __future__ import annotations

from typing import Any

from .fixmon import FixmonUnavailable, resolve_session, rows_to_dicts
from .registry import Tool, ToolContext, ToolError

# Mirrors SessionEventType in fix_monitor/src/event.cpp, and must keep doing so.
#
# A wrong value here fails silently and convincingly: the model picks the
# plausible name, the query matches nothing, and "no logon events in the window"
# is reported for a session that logged on without trouble. Nothing errors, so
# nothing prompts anyone to look. schema_check.py compares this list against the
# C++ source; run it whenever either side moves.
#
# Note what is absent: there is no "reject" here. A Reject is a message, not a
# session event, and belongs to the msg_type filter. Offering it would send the
# model down a branch that can only come back empty.
SESSION_EVENT_TYPES = [
    "session_created",
    "session_scheduled",
    "connecting",
    "connect_failed",
    "logon_sent",
    "logon_received",
    "logon_rejected",
    "logout_sent",
    "logout_received",
    "disconnected",
    "reconnect_attempt",
    "heartbeat_timeout",
    "test_request_sent",
    "seq_num_too_high",
    "seq_num_too_low",
    "resend_requested",
    "sequence_reset",
    "config_error",
    "unparsed",
]

# ---------------------------------------------------------------------------

SYSTEM_PROMPT = """\
You are a FIX session monitoring assistant for an operations team. You answer
from the tools only.

Rules that matter more than fluency:

1. Never state a cause you cannot support from tool output. If the data does
   not show why something happened, say what it does show and what is missing.

2. A sequence gap has no meaning on its own. Read `seq_reset_policy` before
   interpreting one:
     reset_each_logon  - restarting at 1 is configured behaviour, not a fault
     reset_each_logout - resets on disconnect, so judge it against the timeline
     persistent        - numbers carry across reconnects, so a gap is real loss
     unknown           - the engine config was not read. Do NOT guess. Say the
                         engine config is needed to judge it.

3. Before advising a resend, check `resend_capability`. `gap_fill_only` means
   the engine keeps no outgoing messages and can only answer a ResendRequest
   with a SequenceReset, so "ask them to resend" cannot work there.

4. Sessions often come in mirrored pairs - the same link seen from both
   engines, e.g. A->B and B->A. Never add their message counts together.
   Compare them: one side's `last_outgoing_seq` should track the other side's
   `next_expected_in` minus one, and a divergence localises which direction is
   losing messages.

5. Order details (price, quantity, symbol, account, ClOrdID) are masked in
   storage as `<masked>` or `<redacted>`. That is policy, not corruption, and
   not something to report as a fault. Session-layer fields are all intact.

6. Order matters when reasoning about causes. A gap recorded before a
   disconnect is a different story from a gap recorded after one.

7. Tool results in the conversation history are snapshots, each stamped with
   the time it was retrieved. They do not update. Any question about the
   present - "is it still up", "what is the state now", "did it recover" -
   requires calling the tool again. Answering such a question from an earlier
   result is wrong even when that result was correct when taken.

Be brief. Lead with what happened, then the evidence, then what to do. Say
plainly when you are not sure.
"""

# ---------------------------------------------------------------------------


def _live(ctx: ToolContext) -> list[dict[str, Any]]:
    try:
        return ctx.fixmon.sessions()
    except FixmonUnavailable as e:
        raise ToolError(str(e)) from e


def _visible(ctx: ToolContext, sessions: list[dict]) -> list[dict]:
    """Authorisation, applied where the data is read rather than where it is asked for."""
    return [s for s in sessions if ctx.user.can_see(s["session_id"])]


def _pick(ctx: ToolContext, session_id: str) -> dict[str, Any]:
    sessions = _visible(ctx, _live(ctx))
    ids = [s["session_id"] for s in sessions]

    resolved, alternatives = resolve_session(ids, session_id)
    if resolved is None:
        raise ToolError(
            f"'{session_id}' did not identify one session. "
            f"Candidates: {alternatives}. Ask the user which one, or use a full id."
        )
    return next(s for s in sessions if s["session_id"] == resolved)


# ---------------------------------------------------------------------------
# list_sessions
# ---------------------------------------------------------------------------


def list_sessions(*, ctx: ToolContext, state: str | None = None) -> dict[str, Any]:
    sessions = _visible(ctx, _live(ctx))
    if state:
        sessions = [s for s in sessions if s.get("state") == state]

    # Config travels with the summary on purpose. Without it the model has to
    # make a second call before it can say whether anything here is wrong, and
    # each extra call is another round trip the operator waits through.
    slim = [
        {
            "session_id": s["session_id"],
            "state": s.get("state"),
            "msgs_in": s.get("msgs_in"),
            "msgs_out": s.get("msgs_out"),
            "next_expected_in": s.get("next_expected_in"),
            "last_outgoing_seq": s.get("last_outgoing_seq"),
            "seq_gaps": s.get("seq_gaps"),
            "disconnects": s.get("disconnects"),
            "session_rejects": s.get("session_rejects"),
            "last_disconnect_reason": s.get("last_disconnect_reason") or None,
            "seq_reset_policy": (s.get("config") or {}).get("seq_reset_policy", "unknown"),
            "resend_capability": (s.get("config") or {}).get("resend_capability", "unknown"),
            "connection_type": (s.get("config") or {}).get("connection_type", "unknown"),
        }
        for s in sessions
    ]
    return {"count": len(slim), "sessions": slim}


LIST_SESSIONS = Tool(
    name="list_sessions",
    description=(
        "Every FIX session being monitored, with its current state and the "
        "configuration needed to interpret it.\n\n"
        "Use for an overview, for a health summary, or whenever you need a "
        "session's exact id before calling another tool. For a single session "
        "under investigation use get_session_diagnosis instead - this tool "
        "returns no timeline and no event history.\n\n"
        "Sessions frequently appear in mirrored pairs (A->B and B->A) because "
        "both engines of one link are being watched. They are two views of the "
        "same traffic: compare them, never sum them."
    ),
    parameters={
        "type": "object",
        "properties": {
            "state": {
                "type": "string",
                "enum": ["disconnected", "connecting", "logon_pending", "logged_on", "stale"],
                "description": (
                    "Optional filter. Use only when the user narrowed the question "
                    "('which ones are down'). Omit for a general listing."
                ),
            }
        },
        "required": [],
    },
    fn=list_sessions,
)


# ---------------------------------------------------------------------------
# get_session_diagnosis
# ---------------------------------------------------------------------------

_TIMELINE_SQL = """
SELECT ts_ns, source, direction, msg_type, msg_type_name, session_event,
       msg_seq_num, expected_seq, received_seq, reject_reason, ref_seq_num,
       ord_status, text
FROM events
WHERE session_id = ?
  AND ts_ns >= ?
  AND ( session_event IS NOT NULL
        OR msg_type IN ('3','j','4','2','A','5')
        OR (msg_type = '8' AND ord_status = '8') )
ORDER BY ts_ns DESC
LIMIT ?
"""


def get_session_diagnosis(
    *, ctx: ToolContext, session_id: str, window: str = "1h"
) -> dict[str, Any]:
    session = _pick(ctx, session_id)
    sid = session["session_id"]
    since = _since_ns(window)

    rows = ctx.db.execute(_TIMELINE_SQL, (sid, since, ctx.max_rows)).fetchall()
    timeline = rows_to_dicts(rows)
    timeline.reverse()  # oldest first: causation reads forwards

    # The mirror, if it is being watched. Offered rather than described - the
    # model decides whether the comparison is relevant to the question asked.
    mirror = None
    cfg = session.get("config") or {}
    if "->" in sid and ":" in sid:
        head, tail = sid.split(":", 1)
        a, b = tail.split("->", 1)
        wanted = f"{head}:{b}->{a}"
        for s in _visible(ctx, _live(ctx)):
            if s["session_id"] == wanted:
                mirror = {
                    "session_id": s["session_id"],
                    "state": s.get("state"),
                    "next_expected_in": s.get("next_expected_in"),
                    "last_outgoing_seq": s.get("last_outgoing_seq"),
                    "seq_gaps": s.get("seq_gaps"),
                }
                break

    return {
        "session_id": sid,
        "window": window,
        "state": session.get("state"),
        "counters": {
            k: session.get(k)
            for k in (
                "msgs_in", "msgs_out", "next_expected_in", "last_outgoing_seq",
                "seq_gaps", "seq_gaps_reported", "seq_too_low", "resend_requests",
                "heartbeat_timeouts", "disconnects", "reconnect_attempts",
                "logon_rejects", "session_rejects", "business_rejects", "exec_rejects",
            )
        },
        "last_disconnect_reason": session.get("last_disconnect_reason") or None,
        "last_reject_text": session.get("last_reject_text") or None,
        "clock_skew_ns": session.get("clock_skew_ns"),
        "config": cfg,
        "mirror_session": mirror,
        "timeline": timeline,
        "timeline_truncated": len(rows) >= ctx.max_rows,
    }


GET_SESSION_DIAGNOSIS = Tool(
    name="get_session_diagnosis",
    description=(
        "Everything needed to explain one session: live counters, the "
        "configuration that gives those counters meaning, the mirrored session "
        "if its other half is also monitored, and a timeline of notable events "
        "in the window.\n\n"
        "Use when a specific session is being investigated. If you do not know "
        "the exact id, call list_sessions first - though a distinctive fragment "
        "such as 'VENUEX' is usually enough here.\n\n"
        "Reading the result:\n"
        "- `seq_gaps` counts gaps derived from the message flow; "
        "`seq_gaps_reported` counts gaps the engine itself logged. They should "
        "roughly agree. A large disagreement is itself a finding and worth "
        "mentioning.\n"
        "- `config.seq_reset_policy` decides whether a gap is a fault at all. "
        "If it is 'unknown', say the engine configuration is needed rather than "
        "assuming a default.\n"
        "- `clock_skew_ns` is ingest time minus the engine's own timestamp. It "
        "is never exactly zero. A steady value is normal; a large or growing one "
        "means the hosts disagree about time, and every duration in the timeline "
        "is wrong by that much.\n"
        "- `mirror_session`, when present, is the same link seen from the other "
        "engine. Its `last_outgoing_seq` should track this session's "
        "`next_expected_in` minus one.\n"
        "- `timeline` is oldest first. The order of a gap relative to a "
        "disconnect usually decides which caused which."
    ),
    parameters={
        "type": "object",
        "properties": {
            "session_id": {
                "type": "string",
                "description": (
                    "Session identifier. Full form is BeginString:Sender->Target, "
                    "e.g. 'FIX.4.4:BROKER1->VENUEX'. A unique fragment such as "
                    "'VENUEX' is accepted; if it matches more than one session "
                    "the candidates are returned."
                ),
            },
            "window": {
                "type": "string",
                "enum": ["15m", "1h", "6h", "24h", "7d"],
                "description": "How far back the timeline reaches. Default 1h.",
            },
        },
        "required": ["session_id"],
    },
    fn=get_session_diagnosis,
)


# ---------------------------------------------------------------------------
# query_events
# ---------------------------------------------------------------------------


def query_events(
    *,
    ctx: ToolContext,
    session_id: str,
    window: str = "1h",
    event_type: str | None = None,
    msg_type: str | None = None,
    limit: int = 50,
) -> dict[str, Any]:
    session = _pick(ctx, session_id)
    sid = session["session_id"]

    sql = [
        "SELECT ts_ns, source, direction, msg_type, msg_type_name, session_event,",
        "       msg_seq_num, expected_seq, received_seq, reject_reason, text, raw",
        "FROM events WHERE session_id = ? AND ts_ns >= ?",
    ]
    args: list[Any] = [sid, _since_ns(window)]

    if event_type:
        sql.append("AND session_event = ?")
        args.append(event_type)
    if msg_type:
        sql.append("AND msg_type = ?")
        args.append(msg_type)

    sql.append("ORDER BY ts_ns DESC LIMIT ?")
    args.append(min(limit, ctx.max_rows))

    rows = ctx.db.execute(" ".join(sql), args).fetchall()
    events = rows_to_dicts(rows)
    events.reverse()
    return {"session_id": sid, "window": window, "count": len(events), "events": events}


QUERY_EVENTS = Tool(
    name="query_events",
    description=(
        "Raw rows from the event store for one session, with optional filters. "
        "Includes the stored FIX body in `raw`.\n\n"
        "Use to drill into a specific moment after get_session_diagnosis has "
        "shown roughly where the problem is - not as a first step, because the "
        "diagnosis tool already returns a filtered timeline and this one returns "
        "far more text.\n\n"
        "Client identity and commercial values in `raw` appear as `<masked>`, "
        "and credentials as `<redacted>`. That is the configured policy, not "
        "missing data: sequence numbers, message types, comp ids and reject "
        "codes are all intact, and those are what session diagnosis runs on."
    ),
    parameters={
        "type": "object",
        "properties": {
            "session_id": {"type": "string", "description": "Full id or a unique fragment."},
            "window": {
                "type": "string",
                "enum": ["15m", "1h", "6h", "24h", "7d"],
                "description": "How far back to search. Default 1h.",
            },
            "event_type": {
                "type": "string",
                "enum": SESSION_EVENT_TYPES,
                "description": (
                    "Session-layer event filter. These are engine lifecycle events "
                    "only. Rejects are not among them - a Reject is a message, so "
                    "filter those with msg_type '3' (session reject) or 'j' "
                    "(business reject). 'unparsed' means the engine wrote wording "
                    "no rule recognised, which is worth reporting as a monitoring "
                    "gap rather than a session fault."
                ),
            },
            "msg_type": {
                "type": "string",
                "description": "FIX MsgType(35) filter, e.g. '3' Reject, 'D' NewOrderSingle, '8' ExecutionReport.",
            },
            "limit": {"type": "integer", "description": "Max rows, default 50."},
        },
        "required": ["session_id"],
    },
    fn=query_events,
)


# ---------------------------------------------------------------------------

ALL_TOOLS = [LIST_SESSIONS, GET_SESSION_DIAGNOSIS, QUERY_EVENTS]

_WINDOW_S = {"15m": 900, "1h": 3600, "6h": 21600, "24h": 86400, "7d": 604800}


def _since_ns(window: str) -> int:
    import time

    seconds = _WINDOW_S.get(window, 3600)
    return int((time.time() - seconds) * 1_000_000_000)

