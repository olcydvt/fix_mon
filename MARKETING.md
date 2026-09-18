# Marketing — fixmon

Positioning, messaging and content plan for **fixmon** (the collector) and
**fixmon-chat** (the LLM front end).

Internal document. Not for customers — for customers use `OFFER.md`.

---

## 1. What we are selling

Two products, one story.

| | fixmon | fixmon-chat |
|---|---|---|
| What it does | Reads the engine's logs, keeps the evidence | Answers questions from that evidence |
| Who uses it | Operations, IT | Everyone else |
| Interface | Grafana, `/metrics`, SQLite | Plain language chat |
| Sold as | The product | The reason non-technical people care |

**The one-line story:**

> fixmon keeps the evidence. fixmon-chat reads it back to you.

**The one-line promise (use this everywhere — profile, posts, first slide):**

> Find out what happened to a FIX session without knowing FIX.

## 2. The problem we lead with

A FIX session drops. The first question is always the same: **why?**

Today the answer is split across two files. The message log says the flow
stopped. The event log says why it stopped. Nobody looks at both at once, so a
routine disconnect occupies several people for hours.

This is the entire pitch. Lead with the problem, not the architecture.

**The artefact that sells it** — one merged timeline from two sources:

```
14:29:10  event_log    seq_num_too_high     MsgSeqNum too high, expecting 67 but received 73
14:29:14  message_log  Reject               Required tag missing
14:29:17  message_log  ExecutionReport      Instrument not tradable at this time
14:29:21  event_log    heartbeat_timeout    Test Request timed out
14:29:23  event_log    disconnected         Socket exception, connection reset by peer
```

Neither log produces this table on its own. Show it early, show it often.

## 3. Audience

The product reaches further than an ops tool, and that is the commercial
opportunity. But each audience asks a different question and needs different
words.

| Audience | Their question | What lands |
|---|---|---|
| Operations / IT | "Why did it drop, when did it come back?" | Merged timeline, alerts, no agent in the order path |
| Back office | "Which orders were rejected last night and why?" | Chat, answers with evidence attached |
| Mid office | "Which error repeated most this week?" | Chat, history over a window |
| Front office | "Which counterparties are we connected to right now?" | Chat, instant status |
| Risk / Compliance | "Who asked the system what?" | Audit log, self-hosted model option |
| CTO / Head of IT | "What does it touch, what does it cost me?" | Read-only, no order path, one binary |

**Consequence for content:** engineering depth goes last, not first. It is the
narrowest slice of the audience. It still matters — it is the answer to *how
can it be this certain* — but it is not the opening.

### One caveat, know it before a demo

With `mask_message_bodies = true`, stored message bodies carry `<masked>` in
place of price, quantity, symbol and account. Session-layer diagnosis is
unaffected, but **"why was my specific order rejected" gets a limited answer**.

So do not over-promise to front office. The honest framing:

> Front office gets connectivity and reject-reason visibility. Order-level
> detail is a deployment choice, and most firms will want it masked.

This is a feature in a compliance conversation and a limitation in a trader
conversation. Say it first in both, from the right angle.

## 4. Differentiators, in selling order

Ranked by how much they move a deal, not by how clever they are.

1. **Not in the order path.** No proxy, no sniffing, no agent, no root. It asks
   for read access to files the engine already writes. This kills the biggest
   objection before it is raised.
2. **The collector never reaches the internet.** Egress lives in the chat
   process, which can sit on its own network segment. For a bank this is the
   difference between a conversation and a refusal.
3. **The model can be yours.** One line of config points at a self-hosted,
   OpenAI-compatible model. Nothing leaves the network.
4. **Every question is audited.** Who asked what, which tool ran, with which
   arguments. Results are deliberately not stored — no second copy of session
   data appears anywhere new.
5. **Discovery from the engine's own config.** No second session list to drift
   out of sync. Add a session to the engine and the monitor knows.
6. **Credentials are dropped at parse time.** Passwords never reach a variable,
   a metric, the store or a log line — and the metric registry refuses a label
   by that name as a second layer.
7. **Answers carry their evidence.** The model synthesises, but the raw rows
   are there. This is what turns "a chatbot" into "an investigation tool".
8. **Windows and Linux. One binary, no dependencies.**

Note what is *not* on this list: the queue design, the WAL tuning, the adapter
abstraction. Real engineering, wrong audience. Save it for the technical post
and for the "how do I know it won't fall over" question.

## 5. Proof we already have

Do not invent claims. These are backed by the repos:

| Claim | Evidence |
|---|---|
| It works against a real engine | Live initiator + acceptor run, both directions monitored |
| It catches sequence problems | Seq number changed by hand, caught, shown on the Grafana panel |
| Failures are visible | Killing one end produced disconnect, then stale after heartbeat tolerance |
| The stack comes up | Four services up, `verify_stack.sh`, 16 checks passed |
| Credentials do not leak | `verify_no_secrets.sh` greps the real values out of the cfg and checks every endpoint |
| It is tested | 100+ checks in the collector, 93 in the chat host |
| We test the ugly cases | Scripted provider forces a looping model, an invented tool name, two tools at once |

**The strongest single asset:** the real-engine run where the sequence number
was broken by hand and caught. It is currently buried in a table in `DEMO.md`.
Write it up as a narrative — it is the best sales material that exists.

### Blocker to clear before the chat demo

`fixmon-chat` has **not been run against a live API** yet. Adapters are
verified by shape, not by round trip. Before recording any chat video, do one
real round trip against a provider (or the local model) and fix whatever comes
out — most likely a model name or a base URL.

Do not record a demo against a scripted provider. If it ever came out, it would
cost more than the video is worth.

## 6. Messaging rules

**Do not lead with "customisable".** It is true, and it is the wrong thing to
say in public. "Adapts to your needs" stops nobody — people do not recognise
themselves in a description that fits everyone. Worse, it signals *expensive,
slow, dependency*, and it prices you as a consultant on a day rate.

> **Marketing speaks narrowly. Sales speaks broadly.**

One sharp promise outside. Flexibility opens in the room, and "it already does
that" beats "we can build that" every time. Package the flexibility as a tier
(see `OFFER.md`) so it has a price instead of being given away.

**Keep the honesty.** Both READMEs say what has not been tested and what the
product structurally cannot see — TCP health, wire latency, anything that never
reached the engine. Do not sand this off. In this market a vendor who names
their limits is the one who gets believed about everything else.

**Never show a model answer without its sources.** This audience is sceptical
of LLM output by default. Showing the rows it relied on is the single detail
that moves the product from chatbot to investigation tool.

**Never use customer data.** Demos use `tools/gen_logs.py` only. If a real
engine recording is used, anonymise the comp IDs.

## 7. Content plan — six weeks, one post a week

More than one a week is not sustainable. Less is invisible.

| Week | Audience | Topic | Format |
|---|---|---|---|
| 1 | Everyone | The problem: one dropped session occupies five people | Text + timeline image |
| 2 | Everyone | **Chat demo** — ask a question, get an answer with evidence | 60–90s video |
| 3 | Ops / IT | Kill the session live: stale, alert, recover | 60–90s video |
| 4 | Management / Risk | Nothing leaves your network; every question is audited | Text + audit.log snippet |
| 5 | Ops / IT | Discovery from the engine config — no second session list | Text + startup output |
| 6 | Engineers | Architecture, and what it cannot see | Text |

Chat comes at week 2, not at the end. For the widest part of the audience the
chat *is* the product — delaying it delays them understanding what this is.

Each post: **one idea**, end with a question, at most three hashtags
(`#FIX #trading #observability`).

Closing question that reliably opens conversations:

> When a session drops at your firm, who finds out first — and how?

Comments are free market research and a legitimate reason to open a DM.

## 8. Video scripts

### Week 2 — chat (90 seconds)

```
0-10   Grafana: a session has gone STALE, alert red.
10-20  Chat window: "What happened to VENUEX?"
20-40  Answer streams in — and critically, the raw log lines it used
       are listed underneath.
40-55  Click one of the raw lines, the original event store record opens.
55-70  "How many times this month?" -> count + dates.
70-85  Show config.yaml: provider: local. "The model can be yours."
85-90  Close: "It returns evidence, not opinions."
```

### Week 3 — kill the session (90 seconds)

```
0-10   Grafana "FIX Sessions", LOGGED ON, messages flowing.
10-25  Terminal: docker compose stop loggen        "The far side went away."
25-45  Split screen: last_message_age climbing, state -> STALE,
       Prometheus alert FixSessionStale -> Pending
45-65  SQLite timeline query, two sources merging into one table.
       "The message log says the flow stopped. The event log says why."
65-85  docker compose start loggen -> back to green
85-90  Close card: repo link + "read-only, no proxy, no sniffing"
```

**Silent with captions, not voiceover.** Most LinkedIn video is watched muted,
and this content reads well visually. If you do want audio, your own voice is
fine — "a real engineer explaining it" outperforms polished narration with this
audience.

Always run `tools/verify_stack.sh` before recording. `DEMO.md` puts it best:
better than finding "No data" on stage.

## 9. Demo questions that land

Use these in the chat demo instead of technical ones. All are answerable from
the event store today.

- "What happened to VENUEX last night?"
- "Which counterparties are we connected to right now?"
- "How many times did we disconnect this week?"
- "Which reject reason came up most this month?"
- "Was anything unusual between 14:00 and 15:00 yesterday?"

Then one that shows integrity — ask something the logs cannot answer and let
the model say so:

- "Was there any network latency on that link?"

Being told "the logs cannot show that" is more persuasive than any correct
answer. It is also the honest demonstration of the one real risk in this
layer: a model constructing a plausible story without noticing the gap.

## 10. Repository work before the first post

The first post sends traffic to the repos. Ungated, unlicensed, image-free
repos waste that traffic.

- [ ] **Screenshots** — `docs/images/`, Grafana panels and the chat window.
      Neither README has a single image; LinkedIn link previews come up empty.
- [ ] **LICENSE** — the first question a visitor has is "can I use this?"
- [ ] **ONE-PAGER.md** — one page: problem, solution, one screenshot, three
      metrics, install command
- [ ] **Case narrative** — the real-engine sequence-number catch, written as a
      story rather than a table row
- [ ] **Fix the MSVC line** — `README.md` says MSVC "has not been attempted"
      and `DEMO.md` says "Not attempted". Both are now false. Public wording is
      just: *Windows and Linux.*
- [ ] **One live API round trip** for fixmon-chat, before any chat demo

## 11. Sales motion

The goal right now is not orders. It is **ten discovery calls.** Posts exist to
open those calls, not to close deals.

### Discovery questions

Ask these to learn the price, not to pitch:

1. How many session-related incidents did you have last month?
2. How many people get pulled in, and for how long?
3. Who watches this today — do you find out from an alert, or from a client
   calling?
4. Who needs this information but cannot get to it today?
5. What have you tried, and why did you stop?

Q3 answered with "the client calls us" means the sale is half made.
Q2 gives you the price: people × hours × frequency, plus reputational cost.

**Do not describe the product until you have heard the answer to Q4.** Then
open the demo with that person's question.

### Pricing shape

- **Annual subscription + one-time setup.** Customisation is billed to setup,
  ongoing service to the subscription. Bundle them and the customisation is
  free work.
- **Price on sessions or counterparties, never on users.** Per-user pricing
  makes the customer ration seats, the tool stays with three people and gets
  dropped at renewal. Per-session pricing means they roll it out to forty
  people and the price grows on its own as they add venues.
- **Never discount the list price.** Give fixed-price-bearing value instead —
  waived setup, a price lock, an exit right. Details and the reasoning are in
  `OFFER.md`.

### Target list

Twenty names: ops and IT managers at brokers you already have some connection
to. Warm before cold.

### The objection that will come

*"What happens if you get hit by a bus?"* — a one-person vendor is a real risk
and pretending otherwise fails. Prepare the answer now: source code escrow,
handover documentation, or the quality of the documentation itself.

Your advantage is unusual here: the READMEs are better than most companies'
product docs. **Show them in the meeting.** That extinguishes the fear more
effectively than any contractual clause.

