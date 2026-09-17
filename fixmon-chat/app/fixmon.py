"""
Read-only access to what the collector produces.

Two sources, because they answer different questions:
  - GET /sessions  - live state and the configuration needed to interpret it
  - the sqlite file - history, which the endpoints do not serve

Nothing here writes. The database is opened with mode=ro so that is enforced by
sqlite rather than by intention, and the HTTP calls are GETs against a server
that has no mutating routes.
"""

from __future__ import annotations

import sqlite3
from typing import Any

import httpx


class FixmonUnavailable(Exception):
    pass


class FixmonClient:
    def __init__(self, base_url: str, db_path: str, timeout_s: float = 5.0):
        self.base_url = base_url.rstrip("/")
        self.db_path = db_path
        self.timeout_s = timeout_s

    # ---- live state -------------------------------------------------------

    def sessions(self) -> list[dict[str, Any]]:
        try:
            # trust_env=False: the collector is on loopback and a corporate
            # proxy that intercepts it would turn "session data" into an
            # outbound request. follow_redirects=False for the same reason -
            # a 302 here means something answered that should not have.
            with httpx.Client(
                    trust_env=False,
                    follow_redirects=False,
                    timeout=self.timeout_s,
            ) as client:
                r = client.get(f"{self.base_url}/sessions")
            if r.is_redirect:
                raise FixmonUnavailable(
                    f"{self.base_url}/sessions answered {r.status_code} redirecting to "
                    f"{r.headers.get('location', '?')!r}; the request was intercepted "
                    "before it reached the collector - check proxy settings."
                )
            r.raise_for_status()
        except httpx.HTTPError as e:
            raise FixmonUnavailable(
                f"collector not reachable at {self.base_url} ({e}). "
                "It may not be running; historical queries still work."
            ) from e
        return r.json().get("sessions", [])

    # ---- history ----------------------------------------------------------

    def connect(self) -> sqlite3.Connection:
        """
        A fresh read-only connection.

        Per request, never shared: sqlite3 connection objects are not safe
        across threads, and two operators asking questions at the same moment
        are exactly that case. WAL lets these readers run while the collector
        keeps writing.
        """
        conn = sqlite3.connect(f"file:{self.db_path}?mode=ro", uri=True, timeout=5.0)
        conn.row_factory = sqlite3.Row
        return conn


def rows_to_dicts(rows: list[sqlite3.Row]) -> list[dict[str, Any]]:
    return [{k: r[k] for k in r.keys()} for r in rows]


def resolve_session(
    candidates: list[str], wanted: str
) -> tuple[str | None, list[str]]:
    """
    Turn whatever the model produced into a real session id.

    The model will write "VENUEX" when the id is "FIX.4.4:BROKER1->VENUEX", and
    no amount of schema text reliably stops that. Meeting it halfway here is
    worth more than another sentence in the description.

    The tie-break matters more than it looks. When both engines of a link are
    monitored the ids are mirrors - BROKER1->VENUEX and VENUEX->BROKER1 - so a
    bare counterparty name matches both, every time. That is the normal case in
    a paired deployment, not an edge case, and answering "ambiguous" to it would
    make the tool useless exactly where it is most used. Someone naming a
    counterparty almost always means our session *with* them, so a match on the
    target side wins. A fragment that is genuinely ambiguous still comes back as
    a list for the operator to choose from.
    """
    if wanted in candidates:
        return wanted, []

    needle = wanted.strip().lower()
    hits = [c for c in candidates if needle in c.lower()]

    if len(hits) == 1:
        return hits[0], []

    if len(hits) > 1:
        as_target = [c for c in hits if needle in c.split("->", 1)[-1].lower()]
        if len(as_target) == 1:
            return as_target[0], []
        return None, hits

    return None, candidates

