#!/usr/bin/env python3
"""Convert ea_jumptables.py output (config/switch_tables.toml) into RexGlue's
codegen schema.

  ea_jumptables:   [[switch]]   base = <bctr PC>   r = <reg>   default=..  labels=[..]
  RexGlue codegen: [[switch_tables]]  address = <bctr PC>  register = <reg>  labels=[..]

RexGlue parses address/register/labels (config.cpp); `default` is unused, dropped.
Both express `base`/`address` as the address of the bctr instruction, so the
mapping is 1:1 (verified against bctr @ 0x82C1F5DC).

Usage: python tools/convert_switch_tables.py config/switch_tables.toml app/nhl12_switch_tables.toml
"""
import re
import sys


def convert(src_text: str) -> str:
    out = []
    n_tables = 0
    for line in src_text.splitlines():
        s = line.strip()
        if s.startswith("[[switch]]"):
            out.append("[[switch_tables]]")
            n_tables += 1
            continue
        m = re.match(r"\s*base\s*=\s*(.+)$", line)
        if m:
            out.append(f"address = {m.group(1).strip()}")
            continue
        m = re.match(r"\s*r\s*=\s*(.+)$", line)
        if m:
            out.append(f"register = {m.group(1).strip()}")
            continue
        if re.match(r"\s*default\s*=", line):
            continue  # RexGlue ignores it; drop for cleanliness
        out.append(line)
    sys.stderr.write(f"converted {n_tables} switch tables\n")
    header = ("# Auto-generated from config/switch_tables.toml by "
              "tools/convert_switch_tables.py.\n"
              "# RexGlue [[switch_tables]] schema: address=<bctr PC>, register, labels.\n\n")
    return header + "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__ + "\n")
        return 2
    with open(sys.argv[1], "r", encoding="utf-8-sig") as f:
        text = f.read()
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(convert(text))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
