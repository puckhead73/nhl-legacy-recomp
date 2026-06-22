#!/usr/bin/env python3
"""Build a curation SCHEMA for the engine tunables the overlay renders.

Input : docs/tunable_values_runtime.json  (the 12k-tunable catalog: name,
        category, label, value_bits/i32/f32, name_va, value_va, record_va).
Output: docs/tunables_schema.json          (per-tunable display metadata).

The overlay can't auto-organize 12k raw gXxx names cleanly, and the game ships
no static min/max table (the registration records materialize at boot, BSS in
the image). So we synthesize a schema from what IS reliable — the category
hierarchy (97% clean) — plus heuristics for label/type/widget/range. High-value
panels (Injuries) are then hand-refined on top; this file is the scalable base.

Each schema entry:
  { name, panel, section, label, type, widget, min, max, step, source }
    panel   : friendly top-level group (Goalie, Shooting, Injuries, Physics, ...)
    section : the remaining category path (for sub-grouping inside a panel)
    type    : bool | int | float
    widget  : checkbox | slider | stepper
    min/max : slider bounds (heuristic; null for stepper)
    source  : "pool-label" | "humanized" (where the label came from)
"""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "docs" / "tunable_values_runtime.json"
OUT = REPO / "docs" / "tunables_schema.json"

BOOL_PREFIXES = ("gIs", "gHas", "gEnable", "gEnabled", "gUse", "gAllow", "gCan",
                 "gShould", "gDoes", "gForce", "gShow", "gDisable", "gWants")
INT_WORDS = ("Num", "Count", "Frames", "Ticks", "Index", "Iterations", "Steps",
             "Level", "Frame")


def humanize(name: str) -> str:
    """gInjuryExistingInjMinIncrease -> 'Injury Existing Inj Min Increase'."""
    s = name[1:] if name.startswith("g") else name
    s = s.replace("_", " ")
    # split camelCase and letter/number boundaries
    s = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", s)
    s = re.sub(r"(?<=[A-Za-z])(?=[0-9])", " ", s)
    s = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", s)
    return re.sub(r"\s+", " ", s).strip()


def good_pool_label(label: str, name: str) -> bool:
    """Accept a pool label only when it reads like a human display string and
    isn't just the de-prefixed name echoed back."""
    if not label:
        return False
    if len(label) < 3 or len(label) > 60:
        return False
    # Reject single-token labels that just echo the name (e.g. 'InjuryRateSlider'
    # for gInjuryRateSlider) — humanize() does better.
    if " " not in label:
        compact = re.sub(r"[^a-z0-9]", "", label.lower())
        nm = re.sub(r"[^a-z0-9]", "", name.lower())
        if compact in nm or nm in compact:
            return False
    return True


def category_ok(cat: str) -> bool:
    if not cat or "/" not in cat:
        return False
    # Reject sentence-like / asset-path categories (mis-parsed labels).
    if "(" in cat or cat.count(" ") >= 2:
        return False
    if re.search(r"\.(fsh|big|dds|fxo|swf|lua|rw4)(/|$)", cat):
        return False
    return True


def panel_and_section(cat: str, name: str):
    """Friendly panel + sub-section path from the category."""
    if not category_ok(cat):
        return "Misc", ""
    segs = cat.split("/")
    # AI dominates (10k); the meaningful group is the subsystem (segs[1]).
    if segs[0] == "AI" and len(segs) > 1:
        return segs[1], "/".join(segs[2:])
    return segs[0], "/".join(segs[1:])


def infer_type(name: str, i32: int, bits: int, f32) -> str:
    for p in BOOL_PREFIXES:
        if name.startswith(p) and (len(name) == len(p) or name[len(p)].isupper()):
            return "bool"
    # value {0,1} with a bool-ish name handled above; otherwise check int words.
    for w in INT_WORDS:
        if re.search(rf"(?<![A-Za-z]){w}(?=[A-Z0-9]|$)", name):
            return "int"
    # value-bit disambiguation: tiny/denormal float but clean small int -> int.
    if bits not in (None, 0):
        plausible_f = f32 is not None and (1e-4 <= abs(f32) < 1e7)
        small_int = i32 is not None and -1_000_000 < i32 < 1_000_000
        if not plausible_f and small_int:
            return "int"
    return "float"


def infer_widget(name: str, typ: str, i32):
    """Return (widget, min, max, step). No static ranges exist, so these are
    name-pattern heuristics; floats default to a stepper unless a clear 0..1."""
    if typ == "bool":
        return "checkbox", None, None, None
    n = name
    if re.search(r"(Chance|Pct|Percent|Probability)(?![a-z])", n):
        return ("slider", 0, 100, 1) if typ == "int" else ("slider", 0.0, 1.0, 0.01)
    if re.search(r"Level(?![a-z])", n):
        return "slider", 0, 20, 1
    if typ == "int":
        return "stepper", None, None, 1
    # float: a value already in [0,1] suggests a 0..1 normalized knob.
    return "stepper", None, None, None


def main():
    cat = json.loads(CATALOG.read_text(encoding="utf-8"))
    schema = []
    panels = Counter()
    src = Counter()
    for e in cat:
        name = e["name"]
        i32 = e.get("value_i32")
        f32 = e.get("value_f32")
        bits = e.get("value_bits")
        typ = infer_type(name, i32, bits, f32)
        widget, lo, hi, step = infer_widget(name, typ, i32)
        pool = e.get("label", "")
        if good_pool_label(pool, name):
            label, source = pool, "pool-label"
        else:
            label, source = humanize(name), "humanized"
        panel, section = panel_and_section(e.get("category", ""), name)
        panels[panel] += 1
        src[source] += 1
        schema.append({
            "name": name, "panel": panel, "section": section, "label": label,
            "type": typ, "widget": widget, "min": lo, "max": hi, "step": step,
            "source": source,
        })

    OUT.write_text(json.dumps(schema, indent=0), encoding="utf-8")
    # Summary
    print(f"wrote {OUT.relative_to(REPO)}  ({len(schema)} entries)")
    print(f"label source: {dict(src)}")
    tcount = Counter(s["type"] for s in schema)
    wcount = Counter(s["widget"] for s in schema)
    print(f"types:  {dict(tcount)}")
    print(f"widgets:{dict(wcount)}")
    print(f"\n{len(panels)} panels:")
    for p, c in panels.most_common():
        print(f"  {c:5d}  {p}")

    # Show a sample panel so quality is visible.
    sample = sys.argv[1] if len(sys.argv) > 1 else "Goalie"
    print(f"\n=== sample: panel '{sample}' (first 25) ===")
    shown = 0
    for s in schema:
        if s["panel"] != sample:
            continue
        rng = f" [{s['min']}..{s['max']}]" if s["widget"] == "slider" else ""
        sec = f"  <{s['section']}>" if s["section"] else ""
        print(f"  {s['label']:38s} {s['type']:5} {s['widget']:7}{rng}{sec}  ({s['name']})")
        shown += 1
        if shown >= 25:
            break


if __name__ == "__main__":
    main()
