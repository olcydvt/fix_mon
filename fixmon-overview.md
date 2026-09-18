# fixmon

**Find out what happened to a FIX session — without knowing FIX.**

---

## The problem

A FIX session drops. The first question is always the same: **why?**

The answer exists, but it is split across two files. The message log shows that
the flow stopped. The event log holds the reason it stopped. Nobody reads both
at the same time, because correlating them by hand means two terminals, two
timestamp formats and a lot of scrolling.

So a routine disconnect occupies several people for hours. And often the client
notices before you do.

## What fixmon does

It reads both logs and merges them onto one timeline:

```
14:29:10  event log     seq_num_too_high     MsgSeqNum too high, expecting 67 but received 73
14:29:14  message log   Reject               Required tag missing
14:29:17  message log   ExecutionReport      Instrument not tradable at this time
14:29:21  event log     heartbeat_timeout    Test Request timed out
14:29:23  event log     disconnected         Socket exception, connection reset by peer
```

Five lines, in order, from two sources. Neither log produces this on its own.

That timeline feeds three things: **live dashboards** for the operations team,
**alerts** that fire before a client calls, and a **searchable history** of
every message and every session event — kept with the original log line, so any
answer can be checked rather than trusted.

## Ask in plain language

Most of the people who need this information do not read FIX logs, and should
not have to.

> **"What happened to VENUEX last night?"**
>
> The session dropped at 14:29:23. The engine reported a socket exception —
> connection reset by the counterparty. Before that, at 14:29:21, a Test
> Request timed out, so the link was already unresponsive for a few seconds.
> It reconnected at 14:29:28 and logged on successfully.
>
> *Based on 4 event log entries — [view]*

Every answer carries the records it came from. When the logs cannot answer a
question, the answer says so instead of guessing.

Other questions it handles today:

- Which counterparties are we connected to right now?
- How many times did we disconnect this week?
- Which reject reason came up most this month?
- Was anything unusual between 14:00 and 15:00 yesterday?

## What it touches: nothing

This is usually the first question, so it is answered first.

fixmon is **not in your order path.** It is not a proxy, it does not capture
network traffic, and it does not run inside your FIX engine. It asks for **read
access** to log files your engine already writes, and nothing else.

| | |
|---|---|
| Order flow | Untouched. Nothing is intercepted, delayed or modified |
| Your engine | Not modified. Its config file is read, never written |
| Installation | A single executable. No runtime, no database server, no agent |
| Privileges | No administrator or root access required |
| Platform | Windows and Linux |

Because it only reads files, there is no certificate to install, no port to
open into the engine, and no change request against a production trading
system.

## Where your data goes

For most firms this decides whether the conversation continues, so it is
answered plainly.

**The collector has no outbound network access.** It reads files and serves
metrics on your own network. It cannot reach the internet.

The chat assistant is a **separate process**, and that separation does real
work:

- It can sit on its own network segment, with its own firewall rules
- If it stops, monitoring continues — the collector does not depend on it
- **The language model can be one you host yourself.** One line of
  configuration points it at a model inside your network. Nothing leaves the
  building.

If you prefer a commercial model provider, that is supported too — the choice
is yours, and it is a single setting, not a rebuild.

### Credentials never enter the system

Passwords, usernames and key passphrases are discarded **as the engine config
is read**. The value never reaches a variable, a metric, the database or a log
line. Only the fact that a credential was present is kept, so startup can
report that it was deliberately ignored.

There is a second layer behind it: the metrics component refuses to publish any
label whose name looks like a credential, and counts every time it has to. That
counter should read zero forever; if it ever moves, something is wrong and you
will know.

A verification script ships with the product. It reads the real credential
values out of your config and confirms they appear nowhere in the monitoring
output.

### Every question is recorded

Who asked, what they asked, which data was reached for, and when:

```json
{"ts":"2026-09-17T14:22:31+03:00","kind":"question","user":"a.yilmaz","text":"what is wrong with VENUEX"}
{"ts":"2026-09-17T14:22:32+03:00","kind":"tool_call","user":"a.yilmaz","tool":"get_session_diagnosis","ok":true,"duration_ms":14}
```

The *answers* are deliberately not stored. Keeping them would create a second
copy of your session data in a new place, which is exactly what a firm does not
want.

Refused access attempts are recorded too — that is usually the entry an auditor
is looking for.

## Who uses it

| | Typical question |
|---|---|
| **Operations / IT** | Why did it drop, and when did it come back? |
| **Back office** | What was rejected last night, and why? |
| **Mid office** | Which error repeated most this week? |
| **Front office** | Which counterparties are we connected to right now? |
| **Risk / Compliance** | Who asked the system what? |

Dashboards for the people who live in them. Plain language for everyone else.

Pricing is based on sessions monitored, **not on users** — so there is no
reason to ration access.

## What you get to see

**Right now**

- Whether each session is up, and how long since its last message
- Sequence number health, including a cross-check between the two sides of a
  link when both are monitored
- Message rates by direction and type
- Rejects broken down by reason

**Over time**

- Disconnects, reconnects, heartbeat timeouts, logon failures
- Every message and session event, searchable, with the original log line
- A snapshot of each session's state every 30 seconds, so "what did this look
  like at 14:32" is answerable without replaying anything

**Alerts** for session down, stale session, flapping, sequence problems, rising
reject rates and heartbeat timeouts. Thresholds are yours to set per
counterparty.

**And one that most monitoring misses:** whether the monitor is actually
reading anything. A session that is configured but has no log attached is the
one failure where every other indicator looks perfectly healthy. fixmon reports
it explicitly.

## Installation

fixmon reads your engine's own configuration to learn which sessions exist and
where their logs are written. You do not maintain a second list.

This matters more than it sounds. A separate list drifts: a session gets added
to the engine, the monitor is never updated, and the monitor keeps reporting
green for something it is not watching.

It also handles the cases that come up in practice — different log file naming
conventions between engine versions, an engine on Windows with the monitor on
Linux reading the same disk over a mount, and acceptors serving multiple
counterparties.

Typical installation is measured in days, not weeks. Nothing in your existing
setup changes.

## What it cannot do

Stated up front, because finding out later is worse.

fixmon sees what your engine writes to its logs. It therefore **cannot** see:

- TCP-level health — retransmits, zero-window, round-trip time
- Wire latency — only the engine's own timestamps exist
- Anything that never reached the engine: a TLS handshake failure, a firewall
  drop, a venue not answering the connection at all
- Anything at all, if the engine's logging is switched off

Two further limits worth knowing:

**Order detail can be masked.** Price, quantity, symbol and account can be
withheld from storage. Most firms want this. Session diagnosis is unaffected —
sequence numbers, message types, counterparty IDs and reject codes stay intact
— but "why was this specific order rejected" then gets a narrower answer. It is
a per-deployment choice and it is reversible.

**New sessions need a restart.** The engine config is read at startup. Adding a
session to the engine while fixmon is running means restarting fixmon to pick
it up.

When the engine writes a log line fixmon does not recognise, the line is **kept
and counted**, never discarded. A rising count of unrecognised lines is the
signal that an engine version or a vendor changed its wording — and the raw
lines are there to build the new rule from.

## Status

Honest about what has been run, and what has not.

| | |
|---|---|
| Collector, log adapters, state tracking, event store | Proven. 100+ automated checks |
| Against a real QuickFIX engine pair | **Run.** Initiator and acceptor, both directions monitored |
| Disconnect and stale detection | Verified live — one end killed, both states observed |
| Sequence mismatch detection | Verified live — sequence number altered by hand, caught and shown |
| Dashboards, alerts, full stack | Run end to end, verified by an automated script |
| Chat assistant | 93 automated checks. **Early access** — first production deployments now |

Built for QuickFIX engines (C++ and Java). Other engines that write comparable
logs can be supported; the parsing rules are a configuration table, not code.

## Packages

**Standard** — installation against your existing engine configuration,
dashboards, alert rules, the event store, and the chat assistant.

**Adaptation** — your alert thresholds, rules for your engine's specific log
wording, and the reports and panels your teams ask for.

**Enterprise** — authorisation, so everyone can ask but not everyone sees
everything; a self-hosted language model, so no data leaves your network; and a
service level commitment.

## Common questions

**Does it need access to our order flow?**
No. It reads log files the engine has already written.

**Do we have to change our engine configuration?**
No. It is opened read-only and never written back.

**Our engine runs on Windows, our monitoring is on Linux. Is that a problem?**
No. That combination is explicitly supported, including the log paths not
matching between the two.

**Does our data go to an AI provider?**
Only if you choose one. The language model can run entirely inside your
network, and the monitoring component itself has no internet access at all.

**What if the chat component goes down?**
Monitoring continues unaffected. The two are separate processes, and the
collector does not know the chat exists.

**Can the assistant change anything?**
No. It has read-only access, enforced by the database rather than by intention,
and the monitoring component exposes no route that modifies anything.

**How long does installation take?**
Days. The first session is usually connected on day one.

---

## Next step

A short installation in your environment, against one session, with your own
logs. You will see your own sessions on the dashboard the same day.

[YOUR NAME] · [EMAIL] · [PHONE]

