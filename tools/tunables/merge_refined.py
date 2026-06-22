#!/usr/bin/env python3
"""Merge the agent-refined chunk files back into the tunables schema.

Base = docs/tunables_schema.auto.json (carries panel/section + a complete name
set). For each docs/tunables_chunks/chunk_NNN.refined.json that exists, overlay
the refined display fields (label/type/widget/min/max/step/unit/help) onto the
matching entry by name. Names absent from any refined file keep their auto values
(graceful fallback for failed/missing chunks). Writes docs/tunables_schema.json.
"""
import json
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
AUTO = REPO / "docs" / "tunables_schema.auto.json"
DIR = REPO / "docs" / "tunables_chunks"
OUT = REPO / "docs" / "tunables_schema.json"

REFINED_FIELDS = ("label", "type", "widget", "min", "max", "step", "unit", "help")
VALID_TYPE = {"bool", "int", "float"}
VALID_WIDGET = {"checkbox", "slider", "stepper"}


def main():
    base = {e["name"]: e for e in json.loads(AUTO.read_text(encoding="utf-8"))}
    order = [e["name"] for e in json.loads(AUTO.read_text(encoding="utf-8"))]

    refined_count = 0
    bad = 0
    files = sorted(DIR.glob("chunk_*.refined.json"))
    for f in files:
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except Exception as ex:
            print(f"  ! {f.name}: parse error ({ex})")
            continue
        for r in data.get("entries", []):
            nm = r.get("name")
            if nm not in base:
                bad += 1
                continue
            ent = base[nm]
            # Validate/sanitize before overlaying.
            if r.get("type") in VALID_TYPE:
                ent["type"] = r["type"]
            if r.get("widget") in VALID_WIDGET:
                ent["widget"] = r["widget"]
            for k in ("label", "unit", "help"):
                if isinstance(r.get(k), str) and r[k].strip():
                    ent[k] = r[k].strip()
            for k in ("min", "max", "step"):
                if k in r:
                    ent[k] = r[k]
            ent["source"] = "refined"
            refined_count += 1

    merged = [base[n] for n in order]
    OUT.write_text(json.dumps(merged, indent=0), encoding="utf-8")

    src = Counter(e.get("source", "auto") for e in merged)
    typ = Counter(e["type"] for e in merged)
    wid = Counter(e.get("widget", "?") for e in merged)
    print(f"wrote {OUT.relative_to(REPO)}  ({len(merged)} entries)")
    print(f"refined files: {len(files)}  entries refined: {refined_count}  "
          f"unknown-name skipped: {bad}")
    print(f"source: {dict(src)}")
    print(f"types:  {dict(typ)}")
    print(f"widgets:{dict(wid)}")
    # Spot-check: any entries still un-refined (fallback)?
    fb = [e["name"] for e in merged if e.get("source") != "refined"]
    if fb:
        print(f"\n{len(fb)} entries kept auto values (fallback). first few: {fb[:8]}")


if __name__ == "__main__":
    main()
