"""Checks the tool schemas against what the collector can actually emit.

A schema enum that offers a value the engine never produces is worse than one
that omits it. The model picks the plausible-looking name, the query matches
nothing, and the honest-sounding "there were no logon events in that window"
gets reported for a session that logged on without trouble. Nothing raises, so
nothing prompts anyone to look.

This imports the tool module rather than scraping it. An earlier version ran a
regex over the source, and when the literal list became a named constant the
regex matched nothing - and the check still printed ok, because "every declared
value is real" is trivially true of an empty list. A verifier that cannot tell
an empty result from a passing one is not a verifier, so the emptiness of both
sides is now checked first.
"""

from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from app.tools import SESSION_EVENT_TYPES  # noqa: E402

CPP = pathlib.Path(__file__).resolve().parents[1] / "fix_monitor" / "src" / "event.cpp"


def engine_session_events() -> set[str]:
    if not CPP.exists():
        raise SystemExit(f"collector source not found at {CPP}")

    src = CPP.read_text(encoding="utf-8", errors="replace")
    found = set(re.findall(r'case SessionEventType::\w+:\s*return "([a-z_]+)"', src))
    if not found:
        raise SystemExit(
            f"no SessionEventType cases found in {CPP}. Either the switch was "
            "rewritten or the pattern no longer matches - either way this check "
            "is not actually running and must not report success."
        )
    return found


def main() -> int:
    real = engine_session_events()
    declared = list(SESSION_EVENT_TYPES)

    if not declared:
        print("FAIL: the tool schema offers no event types at all.")
        return 1

    if len(declared) != len(set(declared)):
        dupes = sorted({v for v in declared if declared.count(v) > 1})
        print(f"FAIL: duplicate values in the schema: {dupes}")
        return 1

    bogus = sorted(set(declared) - real)
    missing = sorted(real - set(declared))

    print(f"engine emits   {len(real)} session event types")
    print(f"schema offers  {len(declared)}")
    print()

    if bogus:
        print("BOGUS - schema offers these, engine never emits them:")
        for v in bogus:
            print(f"    {v}")
        print()

    if missing:
        print("MISSING - engine emits these, schema does not offer them:")
        for v in missing:
            print(f"    {v}")
        print()

    if bogus:
        print("FAIL: a model choosing a bogus value gets an empty result and no error.")
        return 1

    print("ok: every offered value is one the engine can actually produce")
    if missing:
        print("note: some emitted types are not offered; that only narrows filtering")
    return 0


if __name__ == "__main__":
    sys.exit(main())

