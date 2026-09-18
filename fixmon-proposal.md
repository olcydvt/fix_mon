# fixmon — proposal for [FIRM]

Prepared by: [YOUR NAME]
Date: [DATE]
Valid until: [DATE + 30 days]

---

## 1. What we understood

From our conversation on [DATE]:

- [FIRM] runs [N] FIX sessions with [N] counterparties
- Session problems are currently noticed [how — alert / client call / manual check]
- Investigating one takes roughly [N] people and [N] hours
- The people who most need this information today are [teams], and they
  currently ask [team] for it

If any of this is wrong, tell us before you read the rest — the scope and the
price both follow from it.

## 2. The problem we are solving

When a FIX session drops, the answer to "why" exists, but it is split across
two log files. The message log shows the flow stopped. The event log holds the
reason. Correlating them by hand is slow enough that it is rarely done under
pressure.

So a routine disconnect occupies several people, and the client sometimes
notices first.

## 3. What we will put in place

**One timeline from both logs:**

```
14:29:10  event log     seq_num_too_high     MsgSeqNum too high, expecting 67 but received 73
14:29:14  message log   Reject               Required tag missing
14:29:17  message log   ExecutionReport      Instrument not tradable at this time
14:29:21  event log     heartbeat_timeout    Test Request timed out
14:29:23  event log     disconnected         Socket exception, connection reset by peer
```

**Live dashboards** for [ops team] — session state, time since last message,
message rates, rejects by reason, sequence health.

**Alerts** for session down, stale session, flapping, sequence problems and
rising reject rates, with thresholds set per counterparty during installation.

**A searchable history** of every message and session event, each kept with its
original log line.

**A plain-language assistant** so [back office / mid office / front office] can
ask directly instead of raising a ticket:

> "What happened to [COUNTERPARTY] last night?"

Every answer carries the records it came from, so it can be checked. When the
logs cannot answer, it says so rather than guessing.

## 4. What it touches in your environment

**Nothing in your order path.** Not a proxy, no network capture, no agent
inside the engine. It requires read access to log files your engine already
writes.

| | |
|---|---|
| Order flow | Untouched — nothing intercepted, delayed or modified |
| Your engine config | Read-only. Never written back |
| Installation | A single executable. No runtime, no database server, no agent |
| Privileges | No administrator or root access |
| Platform | Windows and Linux |
| Change to your trading system | None |

Session discovery comes from your engine's own configuration, so there is no
second session list for your team to maintain and no risk of it drifting out of
date.

## 5. Where your data goes

**The monitoring component has no outbound network access.** It reads files and
serves data on your network only.

The assistant is a separate process, which means it can sit on its own network
segment, and if it is stopped, monitoring continues unaffected.

**The language model runs where you decide.** For [FIRM] we propose
[self-hosted inside your network / provider], configurable later by a single
setting if that decision changes.

**Credentials never enter the system.** Passwords and usernames are discarded
as the engine config is read — the value never reaches a variable, a metric,
the database or a log line. A verification script reads the real values out of
your config and confirms they appear nowhere in the output. We will run it with
your team during installation.

**Every question is recorded** — who asked, what they asked, which data was
reached for. Answers are deliberately not stored, so no second copy of your
session data is created. Refused access attempts are recorded as well.

## 6. Scope of this proposal

### Included

- Installation on [N] servers, connected to your existing engine configuration
- [N] sessions monitored, covering [counterparties]
- Dashboards and alert rules, with thresholds tuned to your counterparties
- Event store, retention [N] days
- Chat assistant for [N] teams
- Order-level detail [masked / retained] in storage
- Half-day walkthrough for [teams]
- [Support level] support

### Not included

- [e.g. network-level monitoring, TLS certificate monitoring]
- Anything requiring changes to your FIX engine
- [Other exclusions agreed]

## 7. What it cannot do

Stated up front, because finding out later is worse.

fixmon sees what your engine writes to its logs. It cannot see TCP-level health
(retransmits, round-trip time), wire latency, or anything that never reached
the engine at all — a TLS handshake failure, a firewall drop, a venue not
answering the connection.

If [FIRM] needs those, they are a separate piece of work and we would rather
say so now than imply otherwise.

Two operational notes: adding a session to your engine requires a restart of
fixmon to pick it up, and with order detail masked, "why was this specific
order rejected" gets a narrower answer than session-level questions do.

## 8. Timeline

| Stage | Duration | Who is needed from [FIRM] |
|---|---|---|
| Installation, first session connected | [X] days | [role], [N] hours |
| Remaining sessions | [X] days | — |
| Dashboards and alert thresholds tuned | [X] days | [ops team], [N] hours |
| Credential verification run with your team | Half a day | [security/ops] |
| Walkthrough for [teams] | Half a day | Attendees |
| Review | End of month 3 | [sponsor] |

Total: **[X] weeks** from start date.

## 9. Investment

| | |
|---|---|
| Annual subscription — [N] sessions | **[AMOUNT] / year** |
| Setup and adaptation | **[AMOUNT]** one time |
| **Year one total** | **[AMOUNT]** |

Priced on sessions monitored, **not on users**. How many people at [FIRM] use
it does not change the price, so there is no reason to limit access.

The subscription covers updates, new releases and [support level] support.

### Early customer terms

[FIRM] would be one of our first customers, and the terms reflect that:

| | Value |
|---|---|
| **Setup fee waived** | [AMOUNT] |
| **Price locked for three years** — no increases | Inflation risk stays with us |
| **No session limit in year one** | Add counterparties at no extra cost |
| **Exit within three months** | Not satisfied, remaining fees refunded |
| **Enterprise features included in year one** | [AMOUNT] |

Total value: **[AMOUNT]**.

In return we ask for one thing: **two hours a month, for three months, of
feedback** from the teams using it. The direction of the product will largely
be set in those sessions.

## 10. Payment terms

- Setup: [waived]
- Subscription: [annual in advance / quarterly], first invoice on [milestone]
- Payment terms: [N] days
- Exit right: written notice within three months of go-live, remaining
  subscription refunded in full

## 11. Why us

The product has been run against a real QuickFIX initiator and acceptor pair,
not just test data. Disconnects, stale sessions and sequence mismatches were
all observed and caught live — including a sequence number altered by hand to
confirm detection.

The monitoring component carries 100+ automated checks and the assistant 93,
including deliberately difficult cases.

We also write down what the product cannot do, which you have already seen two
sections ago. We would rather lose a deal on scope than win one and disappoint
you in month two.

## 12. Next step

Valid until [DATE].

The next step is a [X]-day installation against one session in your
environment. You will see your own sessions on a dashboard the same day, and
nothing in your trading system will have changed.

Shall we set a date?

**[YOUR NAME]**
[TITLE]
[EMAIL] · [PHONE]

