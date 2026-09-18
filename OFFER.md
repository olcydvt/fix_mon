# Customer Offer — template

Two parts:

1. **Notes for you** — internal, never sent
2. **The offer** — fill in the brackets and send as-is

Every number is an example. Replace with your own.

---

# Part 1 — Notes for you

## Who this is for

The first two or three customers. After that the "early customer" terms come
off and the list price stays exactly where it was. That is the whole purpose of
this document: **win early customers without breaking the price.**

## What never moves

The list price. Not discounted, not negotiated. Discount it once and:

- That number becomes your real price, permanently
- Returning to list next year reads as a price rise and poisons the renewal
- The second customer eventually learns what the first one paid
- In financial services, cheap reads as *unproven*

## What can move

Items that cost the customer nothing to receive and cost you little to give,
but never touch the list price:

| Item | Cost to you | Value to them |
|---|---|---|
| Waived setup fee | Effort, once | Direct cash |
| 3-year price lock | Low — you want to keep them anyway | Real money under inflation |
| Unlimited sessions in year one | Near zero | No penalty for growing |
| Exit right within 3 months | Risk | The single strongest item for getting a yes |
| Roadmap priority | Time | Medium-high |
| Enterprise tier included year one | Effort | High |

None of these change what you tell the next prospect.

## The reference — do not ask for it up front

An earlier draft of this advice had the customer pay full price *and* hand over
a reference. That trade is not balanced and a buyer will say so.

**Being a reference has a real cost.** It needs compliance approval, it carries
reputational risk, and it exposes operational infrastructure to competitors. It
is something you are asking them to pay. It must be paid for.

The rule:

- Do **not** ask at signature. Say "let's talk about it later."
- Ask after they have results.
- When you ask, **pay for it** — 20% off the second year. That is a trade for a
  service, not a discount on the product. Clean to explain, clean to book.

Some firms — usually the largest — can never be a public reference. Accept it
and offer alternatives, in this order:

1. **A reference call** — name stays private, but a prospect can phone and ask.
   *This is the strongest one. It closes more business than a logo.*
2. **Permission to quote numbers** — "investigation time went from 3 hours to
   20 minutes", firm unnamed
3. **An anonymised case study** — "a broker in Turkey, 12 sessions, 6 months"

## The feedback sessions

Two hours a month for three months. This is not payment for a discount, it is
your **roadmap input**. Hearing what is missing from a real user beats guessing
by a wide margin.

Run them. Send an agenda beforehand. A session that opens with "so, how's it
going?" ends in twenty minutes and teaches you nothing.

## If they ask the price early

Do not quote before you have heard who needs this but cannot get to it today.
If pressed:

> "I'll give you the range, but the number depends on how many sessions you
> want watched. It runs between [LOW] and [HIGH] a year. Let's work out the
> session count and I'll give you the exact figure."

Giving a range is honest. Refusing to give one kills the call.

## Before you send

- [ ] Confirm the session / counterparty count — it sets the price
- [ ] Confirm whether the model must be self-hosted — it sets the tier
- [ ] Confirm whether order-level detail is wanted or must stay masked
- [ ] Body of the email is the summary; the offer goes as a PDF attachment
- [ ] Subject: `[FIRM] — fixmon proposal`
- [ ] One follow-up after three days. Then stop.

---

# Part 2 — The offer

*(Everything below goes to the customer. Fill in the brackets, delete this line.)*

---

**fixmon — proposal for [FIRM]**

Prepared by: [YOUR NAME]
Date: [DATE]
Valid until: [DATE + 30 days]

---

## The problem

When a FIX session drops, the first question is always the same: **why?**

The answer is currently split across two log files. The message log shows that
the flow stopped. The event log holds the reason. Nobody reads both at the same
time, which is why a routine disconnect still occupies several people for
hours — and why the client sometimes finds out before you do.

## What fixmon does

It reads both logs and merges them onto a single timeline:

```
14:29:10  event log     seq_num_too_high     MsgSeqNum too high, expecting 67 but received 73
14:29:14  message log   Reject               Required tag missing
14:29:17  message log   ExecutionReport      Instrument not tradable at this time
14:29:21  event log     heartbeat_timeout    Test Request timed out
14:29:23  event log     disconnected         Socket exception, connection reset by peer
```

Neither log produces this on its own.

And reading it does not require knowing FIX. Ask in plain language, and the
answer comes back with the raw log lines it was based on — so it can be
checked, not just trusted.

## What it touches

**Nothing in your order path.** No proxy, no network capture, no agent inside
the engine. It asks for **read access** to log files your engine already
writes, and nothing else.

That is also why the installation carries no risk: nothing in your existing
setup changes.

| | |
|---|---|
| Deployment | Reads your engine's own configuration; no session list to maintain |
| Platform | Windows and Linux |
| Dependencies | None — a single executable |
| Credentials | Passwords and usernames are discarded as the config is read; they never reach a variable, a metric or a log line |
| Database access | Opened read-only. Enforced by the database, not by convention |

## Where your data goes

For most firms this is the deciding question, so it is answered plainly.

**The collector has no outbound network access at all.** It reads files and
serves metrics locally. It cannot reach the internet.

The chat component is a separate process, which means:

- It can sit on its own network segment with its own firewall rules
- If it is stopped, monitoring continues — the collector does not know it exists
- **The language model can be one you host yourself.** One line of
  configuration points it at a model inside your network. Nothing leaves the
  building.

**Every question is recorded.** Who asked, what was asked, which data was
reached for, and when. Deliberately, the *answers* are not stored — that would
create a second copy of your session data in a new place.

```json
{"ts":"2026-09-17T14:22:31+03:00","kind":"question","user":"[user]","text":"what is wrong with VENUEX"}
{"ts":"2026-09-17T14:22:32+03:00","kind":"tool_call","user":"[user]","tool":"get_session_diagnosis","ok":true,"duration_ms":14}
```

## Who uses it

| | What they ask |
|---|---|
| Operations / IT | "Why did it drop, and when did it come back?" |
| Back office | "What was rejected last night, and why?" |
| Mid office | "Which error repeated most this week?" |
| Front office | "Which counterparties are we connected to right now?" |
| Risk / Compliance | "Who asked the system what?" |

Dashboards for operations. Plain language for everyone else.

## What it cannot do

Stated up front, because finding out later is worse.

It sees what your engine writes to its logs. It therefore **cannot** see
TCP-level health, wire latency, or anything that never reached the engine at
all — a TLS handshake failure, a firewall drop, a venue not answering.

Order-level detail (price, quantity, symbol, account) can be masked in storage.
Most firms will want it masked, which means session diagnosis stays complete
but "why was this specific order rejected" gets a narrower answer. This is a
per-deployment choice and it is reversible.

## Scope

### Standard

- Installation, connected to your existing engine configuration
- Dashboards and alert rules
- Chat interface
- Event store — every message and session event, with the original log line

### Adaptation

- Your alert thresholds
- Rules for your engine's specific log wording
- The reports and panels you ask for

### Enterprise

- Authorisation — everyone can ask, not everyone sees everything
- Self-hosted language model, so no data leaves your network
- Service level commitment

## Investment

| Item | Amount |
|---|---|
| Annual subscription — [N] sessions | **[AMOUNT] / year** |
| Setup and adaptation | [AMOUNT], one time |

Priced on **sessions monitored, not users**. How many people use it inside your
firm does not change the price.

## Early customer terms

[FIRM] would be one of our first customers. We do not discount the list price —
instead the terms below carry the value:

| | Value |
|---|---|
| **Setup fee waived** | [AMOUNT] |
| **Price locked for 3 years** — no increases | Inflation risk stays with us |
| **No session limit in year one** | Add counterparties at no extra cost |
| **Exit right within 3 months** | Not satisfied, remaining fees refunded |
| **Enterprise tier included in year one** | [AMOUNT] |

In return we ask for one thing:

**Two hours a month, for three months, of feedback.** The direction of the
product will largely be set in those sessions.

We are not asking to use your name. If it proves its value over the first three
months, we will raise that separately — and it will not be unpaid.

## Timeline

| Stage | Duration |
|---|---|
| Installation, first session connected | [X] days |
| Dashboards and alerts adapted | [X] days |
| User walkthrough | Half a day |
| Review | End of month 3 |

## Next step

This offer is valid until [DATE].

The next step is a [X]-day installation in your environment. Let's find a date.

[YOUR NAME]
[EMAIL] · [PHONE]

