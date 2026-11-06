#!/usr/bin/env python3
"""Compares Ruche's --title-log against the test server's schedule.

For every title change the player showed, works out the content time at which that
title really starts in the audio, and reports how far off the display was.
"""
import json
import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
from stream_server import SCHEDULE, SEG, SR, FRAME  # noqa: E402

TOLERANCE = 0.75


def main(kind, server_log, app_log):
    events = [json.loads(l) for l in open(server_log, encoding="utf-8")]
    shown = []
    for line in open(app_log, encoding="utf-8"):
        ms, pt, title = line.rstrip("\n").split("\t", 2)
        shown.append((int(ms), float(pt), title))
    if not shown:
        print(f"[{kind}] FAIL: no titles shown")
        return 1

    starts = {s: t for s, t, _ in SCHEDULE}
    by_title = {t: s for s, t, _ in SCHEDULE}
    if kind == "icy":
        conn = next(e["conn"] for e in events if e.get("req") == "connect" and e.get("meta"))
        first_at = {}
        for e in events:
            if e.get("req") == "title" and e.get("conn") == conn and e["title"] not in first_at:
                first_at[e["title"]] = e["at"]
        base = 0.0
        expected = lambda title: first_at.get(title)
    else:
        segs = [e for e in events if e.get("req") == "segment" and e["kind"] == kind]
        s0 = segs[0]
        base = s0["start"]
        seg_starts = sorted({e["start"] for e in segs})
        fps = SR / FRAME

        def expected(title):
            s = by_title.get(title)
            if s is None:
                return None
            if kind == "extinf":
                # titles only exist per segment: the first segment starting at or after s
                return next((x for x in seg_starts if x >= s - 1e-6), None)
            return round(s * fps) / fps  # changes sit on the nearest audio frame

    fails = 0
    print(f"[{kind}] joined at content {base:.2f}s; {len(shown)} titles shown")
    first = shown[0]
    for i, (ms, pt, title) in enumerate(shown):
        exp = expected(title)
        if exp is None:
            print(f"   ?? unknown title {title!r}")
            fails += 1
            continue
        content = base + pt
        if i == 0:
            # the title on air when playback began
            ok = exp <= content + TOLERANCE
            print(f"   {'ok ' if ok else 'BAD'} first title {title!r} (starts {exp:.2f}s, playback began at {content:.2f}s)")
            fails += not ok
            continue
        err = content - exp
        ok = abs(err) <= TOLERANCE
        fails += not ok
        print(f"   {'ok ' if ok else 'BAD'} {title!r}: shown at {content:7.2f}s, starts at {exp:7.2f}s, error {err:+.2f}s")
    print(f"[{kind}] {'PASS' if fails == 0 else 'FAIL'} ({fails} problem(s))")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
