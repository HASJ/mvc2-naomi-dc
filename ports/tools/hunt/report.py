"""Summarise a roster sweep: python -I tools/hunt/report.py OUTDIR [--full]"""
import json, os, sys

rows = [json.loads(l) for l in open(os.path.join(sys.argv[1], "results.jsonl"))]
full = "--full" in sys.argv
for r in rows:
    p, s = r.get("port"), r.get("stub")
    tag = "%-16s side %d %-22s" % (r["char"], r["side"], r["moves"][0] + "..")
    m = r.get("port_minor")
    if m:
        print(tag, "port command readers differ at t%d in %s: %s" % (m["t"], m["move"], m["first"][0][:100]))
    if not p and not s:
        print(tag, "same in all %d moves" % len(r["moves"]))
        continue
    if p:
        print(tag, "PORT differs at t%d in %s" % (p["t"], p["move"]))
        for l in p["first"][:25 if full else 6]:
            print("      ", l[:160])
    if s and (not p or s["t"] < p["t"] or s["move"] != p["move"]):
        print(tag, "stub (no nlAtan256) differs at t%d in %s" % (s["t"], s["move"]))
        for l in s["first"][:25 if full else 4]:
            print("      ", l[:160])
