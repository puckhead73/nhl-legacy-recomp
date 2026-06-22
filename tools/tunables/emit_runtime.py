#!/usr/bin/env python3
"""Emit a compact runtime schema the overlay loads (fast tab-delimited parse).

Source of truth stays docs/tunables_schema.json (rich, refined). This writes
docs/tunables_schema.tsv — one tunable per line, tab-separated, for a trivial
C++ reader (no JSON lib in the overlay TU). Place next to the game exe.

Line: name \t panel \t section \t label \t type \t widget \t min \t max \t step \t unit \t help
  type   : f|i|b  (float/int/bool)
  widget : c|s|t  (checkbox/slider/stepper)
  min/max/step: number or "" (null)
Tabs/newlines in any field are stripped so the format stays line/field-safe.
"""
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "docs" / "tunables_schema.json"
OUT = REPO / "docs" / "tunables_schema.tsv"

TYPE = {"float": "f", "int": "i", "bool": "b"}
WIDGET = {"checkbox": "c", "slider": "s", "stepper": "t"}


def clean(s):
    return str(s).replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def num(x):
    return "" if x is None else (f"{x:g}")


def main():
    d = json.loads(SRC.read_text(encoding="utf-8"))
    lines = []
    for e in d:
        lines.append("\t".join([
            e["name"],
            clean(e.get("panel", "Misc")),
            clean(e.get("section", "")),
            clean(e.get("label", e["name"])),
            TYPE.get(e.get("type"), "f"),
            WIDGET.get(e.get("widget"), "t"),
            num(e.get("min")),
            num(e.get("max")),
            num(e.get("step")),
            clean(e.get("unit", "")),
            clean(e.get("help", "")),
        ]))
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)}  ({len(lines)} lines, {OUT.stat().st_size//1024} KB)")


if __name__ == "__main__":
    main()
