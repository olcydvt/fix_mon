#!/usr/bin/env python3
"""
Writes QuickFIX-shaped message and event logs, appending live so the collector
can be exercised the way it will run in production (tailing a growing file).

Injects the failure modes that actually matter on a FIX desk: a sequence gap
followed by a resend, a session-level reject, a rejected ExecutionReport, a
heartbeat timeout, a disconnect with a reason, and a reconnect.

  python3 tools/gen_logs.py --dir ./logs --duration 30
"""
import argparse
import os
import random
import time
from datetime import datetime, timezone

SENDER = "BROKER1"
TARGET = "VENUEX"
BEGIN = "FIX.4.4"


def ts():
    return datetime.now(timezone.utc).strftime("%Y%m%d-%H:%M:%S.") + \
        f"{datetime.now(timezone.utc).microsecond // 1000:03d}"


def fix(seq, msg_type, outgoing=True, **fields):
    snd, tgt = (SENDER, TARGET) if outgoing else (TARGET, SENDER)
    parts = [f"8={BEGIN}", "9=0", f"35={msg_type}", f"34={seq}",
             f"49={snd}", f"56={tgt}", f"52={ts()}"]
    parts += [f"{k}={v}" for k, v in fields.items()]
    parts.append("10=000")
    return "|".join(parts)


class Writer:
    def __init__(self, path):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self.f = open(path, "a", buffering=1)  # line buffered, like a real engine

    def line(self, payload):
        self.f.write(f"{ts()} : {payload}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="./logs")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--rate", type=float, default=8.0, help="messages per second")
    args = ap.parse_args()

    base = os.path.join(args.dir, f"{SENDER}-{TARGET}")
    msgs = Writer(base + ".messages.log")
    evts = Writer(base + ".event.log")

    out_seq = 1
    in_seq = 1

    # --- session bring-up ---
    evts.line("Created session")
    evts.line(f"Connecting to venue-gw.example.com:9823")
    evts.line("Initiated logon request")
    msgs.line(fix(out_seq, "A", True, **{"98": 0, "108": 30})); out_seq += 1
    time.sleep(0.2)
    msgs.line(fix(in_seq, "A", False, **{"98": 0, "108": 30})); in_seq += 1
    evts.line("Received logon response")
    print(f"[gen] session up, writing to {base}.*.log")

    deadline = time.time() + args.duration
    injected = set()
    start = time.time()

    while time.time() < deadline:
        elapsed = time.time() - start

        # --- normal order flow ---
        msgs.line(fix(out_seq, "D", True, **{
            "11": f"ORD{out_seq}", "55": random.choice(["EURUSD", "XAUUSD", "GBPUSD"]),
            "54": random.choice([1, 2]), "38": random.choice([100000, 250000]),
            "40": 2, "44": round(random.uniform(1.05, 1.10), 5)}))
        out_seq += 1

        msgs.line(fix(in_seq, "8", False, **{
            "11": f"ORD{out_seq-1}", "39": 0, "150": 0, "17": f"EX{in_seq}"}))
        in_seq += 1

        # --- injected failure modes ---
        if elapsed > 5 and "gap" not in injected:
            injected.add("gap")
            in_seq += 6  # six messages vanish
            msgs.line(fix(in_seq, "8", False, **{"11": "ORDX", "39": 0, "150": 0}))
            evts.line(f"MsgSeqNum too high, expecting {in_seq-6} but received {in_seq}")
            evts.line(f"Sent ResendRequest FROM: {in_seq-6} TO: {in_seq-1}")
            msgs.line(fix(out_seq, "2", True, **{"7": in_seq-6, "16": in_seq-1}))
            out_seq += 1
            in_seq += 1
            print("[gen] injected sequence gap + resend")

        if elapsed > 9 and "reject" not in injected:
            injected.add("reject")
            msgs.line(fix(in_seq, "3", False, **{
                "45": out_seq - 1, "371": 40, "373": 1, "58": "Required tag missing"}))
            in_seq += 1
            print("[gen] injected session-level reject")

        if elapsed > 12 and "ordrej" not in injected:
            injected.add("ordrej")
            msgs.line(fix(in_seq, "8", False, **{
                "11": f"ORD{out_seq-1}", "39": 8, "150": 8, "103": 3,
                "58": "Instrument not tradable at this time"}))
            in_seq += 1
            msgs.line(fix(in_seq, "j", False, **{
                "45": out_seq - 1, "380": 2, "58": "Unknown security"}))
            in_seq += 1
            print("[gen] injected order reject + business reject")

        if elapsed > 16 and "hbtimeout" not in injected:
            injected.add("hbtimeout")
            evts.line("Sending test request TEST-1")
            evts.line("Test Request timed out. Session will be disconnected.")
            print("[gen] injected heartbeat timeout")

        if elapsed > 18 and "disconnect" not in injected:
            injected.add("disconnect")
            evts.line("Disconnecting: Socket exception, connection reset by peer")
            msgs.line(fix(out_seq, "5", True, **{"58": "Session ended"})); out_seq += 1
            time.sleep(1)
            evts.line("Attempting to reconnect in 5 seconds")
            evts.line("Connecting to venue-gw.example.com:9823")
            evts.line("Initiated logon request")
            msgs.line(fix(out_seq, "A", True, **{"98": 0, "108": 30})); out_seq += 1
            msgs.line(fix(in_seq, "A", False, **{"98": 0, "108": 30})); in_seq += 1
            evts.line("Received logon response")
            print("[gen] injected disconnect + reconnect")

        if elapsed > 22 and "toolow" not in injected:
            injected.add("toolow")
            evts.line(f"MsgSeqNum too low, expecting {in_seq} but received 4")
            print("[gen] injected seq-too-low")

        if elapsed > 25 and "unknown" not in injected:
            injected.add("unknown")
            evts.line("Vendor-specific condition XYZ-4471 raised on channel 2")
            print("[gen] injected unknown wording (should land as 'unparsed')")

        # heartbeats
        if random.random() < 0.3:
            msgs.line(fix(out_seq, "0", True)); out_seq += 1
            msgs.line(fix(in_seq, "0", False)); in_seq += 1

        time.sleep(1.0 / args.rate)

    print(f"[gen] done. out_seq={out_seq} in_seq={in_seq}")


if __name__ == "__main__":
    main()
