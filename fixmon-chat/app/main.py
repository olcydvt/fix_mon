"""
The host: HTTP surface, identity, and the agentic loop.

The loop is the whole of it - ask the model, run what it asks for, hand the
result back, repeat until it stops asking. Two things in it are load-bearing
and easy to get wrong:

  * the tool schemas go out on *every* round, because these APIs keep no state
    and a request without them is a request where the model has no tools;

  * identity is injected here and never taken from the model's arguments. A
    `user_id` parameter in a schema would be an instruction the model could be
    talked into filling differently.
"""

from __future__ import annotations

import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from fastapi import Depends, FastAPI, Header, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import config as config_mod
from .fixmon import FixmonClient
from .registry import ToolContext, ToolRegistry, User
from .store import AuditLog, ConversationStore
from .tools import ALL_TOOLS, SYSTEM_PROMPT

# Anchored to the package, not to the working directory. A service manager or a
# container image rarely starts a process in the directory its files happen to
# live in, and the failure that causes is a confusing one: either "config.yaml
# not found" at boot or a 404 on the UI while the API works fine.
ROOT = Path(__file__).resolve().parent.parent

cfg = config_mod.load(ROOT / "config.yaml")

# Built once and shared: definitions only, no per-user state in any of them.
registry = ToolRegistry(ALL_TOOLS)
fixmon = FixmonClient(cfg.fixmon_base_url, cfg.fixmon_db_path)
conversations = ConversationStore()
audit = AuditLog(cfg.audit_path)

app = FastAPI(title="fixmon-chat")


# ---------------------------------------------------------------------------
# Identity
# ---------------------------------------------------------------------------

def current_user(
        x_user: str | None = Header(default=None),
        x_user_name: str | None = Header(default=None),
) -> User:
    """
    Placeholder for whatever the deployment actually uses.

    Replace with OIDC, SAML or a reverse-proxy header you trust. What must not
    change is where it is called from: identity is established here, at the
    edge, before any tool runs - not derived from anything in the conversation.
    """
    if not x_user:
        raise HTTPException(401, "X-User header required")
    return User(id=x_user, name=x_user_name or x_user, allowed_sessions=frozenset())


# ---------------------------------------------------------------------------
# API
# ---------------------------------------------------------------------------

class ChatRequest(BaseModel):
    message: str
    conversation_id: str | None = None


class ChatResponse(BaseModel):
    conversation_id: str
    reply: str
    tools_used: list[str]
    rounds: int
    usage: dict[str, int]


@app.post("/api/chat", response_model=ChatResponse)
def chat(req: ChatRequest, user: User = Depends(current_user)) -> ChatResponse:
    conversation_id = req.conversation_id or str(uuid.uuid4())

    try:
        conv = conversations.get_or_create(conversation_id, user.id)
    except PermissionError:
        # Audited before the refusal, not after. This branch is the one an
        # auditor actually cares about - someone reaching for another
        # operator's conversation - and it was previously the only path that
        # left no trace at all, because the audit call sat below the check.
        # Successful reads were recorded and denied ones were not, which is
        # backwards: a denial says more than a success.
        audit.write(
            kind="access_denied",
            user=user.id,
            conversation=conversation_id,
            reason="conversation belongs to another user",
        )
        raise HTTPException(403, "conversation belongs to another user") from None

    audit.question(user=user.id, conversation=conversation_id, text=req.message)
    conv.messages.append({"role": "user", "content": req.message})

    schemas = registry.schemas()
    used: list[str] = []
    total = {"prompt": 0, "completion": 0}

    # One read-only connection for this request. Not shared, not cached:
    # sqlite3 connections are not safe across threads and this endpoint is.
    db = fixmon.connect()
    try:
        ctx = ToolContext(user=user, db=db, fixmon=fixmon, max_rows=cfg.limits.max_rows)

        rounds = 0
        while rounds < cfg.limits.max_tool_rounds:
            rounds += 1

            completion = cfg.provider.complete(conv.messages, schemas, SYSTEM_PROMPT)
            for k in total:
                total[k] += completion.usage.get(k, 0)
            audit.model_round(
                user=user.id, conversation=conversation_id,
                provider=cfg.provider_name, model=cfg.model, usage=completion.usage,
            )

            if not completion.wants_tools:
                conv.messages.append({"role": "assistant", "content": completion.text})
                conversations.save(conv)
                return ChatResponse(
                    conversation_id=conversation_id,
                    reply=completion.text,
                    tools_used=used,
                    rounds=rounds,
                    usage=total,
                )

            # The assistant turn carrying the tool_calls has to go into the
            # history verbatim. Drop it and the tool results that follow refer
            # to a call the provider cannot find.
            conv.messages.append(completion.as_history())

            for call in completion.tool_calls:
                started = time.monotonic()
                result = registry.execute(call.name, call.arguments, ctx)
                elapsed_ms = int((time.monotonic() - started) * 1000)

                used.append(call.name)
                audit.tool_call(
                    user=user.id, conversation=conversation_id, tool=call.name,
                    args=call.arguments, ok='"error"' not in result[:200],
                    ms=elapsed_ms,
                )
                # Stamped with the moment it was read, because a tool result
                # stops being true the instant it lands in the history and
                # nothing in the transcript says so. A model asked "is it still
                # logged on?" will otherwise answer from the snapshot taken
                # three questions ago - correct when it was taken, wrong now,
                # and stated with the same confidence either way. The stamp is
                # what makes staleness visible; the system prompt is what makes
                # the model act on it.
                as_of = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
                conv.messages.append({
                    "role": "tool",
                    "tool_call_id": call.id,
                    "name": call.name,
                    "content": f"[retrieved at {as_of}]\n{result}",
                })

        # Out of rounds. Say so rather than returning an empty answer - a model
        # looping on tools is usually a sign the question needs narrowing.
        conversations.save(conv)
        return ChatResponse(
            conversation_id=conversation_id,
            reply=(
                f"Stopped after {rounds} tool rounds without reaching an answer. "
                "Try narrowing the question - a specific session and a shorter window."
            ),
            tools_used=used,
            rounds=rounds,
            usage=total,
        )
    finally:
        db.close()


@app.post("/api/conversations/{conversation_id}/reset")
def reset(conversation_id: str, user: User = Depends(current_user)) -> dict[str, str]:
    conversations.reset(conversation_id)
    audit.write(kind="reset", user=user.id, conversation=conversation_id)
    return {"status": "reset"}


@app.get("/api/info")
def info(user: User = Depends(current_user)) -> dict[str, Any]:
    """What the operator is talking to. Useful when a provider swap is in doubt."""
    return {
        "provider": cfg.provider_name,
        "model": cfg.model,
        "tools": [t["name"] for t in registry.schemas()],
        "collector": cfg.fixmon_base_url,
        "user": user.name,
    }


@app.get("/api/healthz")
def healthz() -> dict[str, Any]:
    try:
        n = len(fixmon.sessions())
        return {"status": "ok", "sessions": n}
    except Exception as e:  # noqa: BLE001
        return {"status": "degraded", "detail": str(e)}


# ---------------------------------------------------------------------------

app.mount("/static", StaticFiles(directory=ROOT / "static"), name="static")


@app.get("/")
def index() -> FileResponse:
    return FileResponse(ROOT / "static" / "index.html")