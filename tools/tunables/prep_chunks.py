#!/usr/bin/env python3
"""Prepare per-chunk input files for the schema-refinement workflow.

Joins the auto-generated schema (label/type/widget guesses) with the live values
from the runtime catalog (so refiner agents can judge float-vs-int and ranges),
sorts by panel/section so each chunk is coherent, and writes fixed-size chunks to
docs/tunables_chunks/chunk_NNN.json. Each refiner agent reads one chunk file.
"""
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "docs" / "tunables_schema.json"
CATALOG = REPO / "docs" / "tunable_values_runtime.json"
OUTDIR = REPO / "docs" / "tunables_chunks"
CHUNK = 130

def main():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    cat = {e["name"]: e for e in json.loads(CATALOG.read_text(encoding="utf-8"))}
    rows = []
    for s in schema:
        c = cat.get(s["name"], {})
        rows.append({
            "name": s["name"],
            "category": c.get("category", ""),
            "panel": s["panel"],
            "section": s["section"],
            "current_label": s["label"],
            "guess_type": s["type"],
            "i32": c.get("value_i32"),
            "f32": c.get("value_f32"),
            "bits": c.get("value_bits"),
            "located": c.get("located"),
        })
    rows.sort(key=lambda r: (r["panel"], r["section"], r["name"]))

    OUTDIR.mkdir(parents=True, exist_ok=True)
    for f in OUTDIR.glob("chunk_*.json"):
        f.unlink()
    n = 0
    for i in range(0, len(rows), CHUNK):
        part = rows[i:i + CHUNK]
        panels = sorted({r["panel"] for r in part})
        (OUTDIR / f"chunk_{n:03d}.json").write_text(
            json.dumps({"chunk_id": n, "panels": panels, "entries": part}, indent=0),
            encoding="utf-8")
        n += 1
    print(f"{len(rows)} tunables -> {n} chunks of {CHUNK} in {OUTDIR.relative_to(REPO)}")

if __name__ == "__main__":
    main()
