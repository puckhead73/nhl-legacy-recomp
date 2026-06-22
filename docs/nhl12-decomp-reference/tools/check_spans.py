import re

switches = {}
cur = None
for line in open("config/switch_tables.toml"):
    if line.startswith("[[switch]]"):
        cur = {"labels": []}
    else:
        m = re.match(r"base = (0x[0-9A-Fa-f]+)", line)
        if m:
            cur["base"] = int(m.group(1), 16)
            switches[cur["base"]] = cur
        m = re.match(r"\s+(0x[0-9A-Fa-f]+),", line)
        if m:
            cur["labels"].append(int(m.group(1), 16))

spans = []
for line in open("config/nhl12.toml", encoding="utf-8", errors="replace"):
    m = re.search(r"address = (0x[0-9A-F]+), size = (0x[0-9A-F]+)", line)
    if m:
        spans.append((int(m.group(1), 16), int(m.group(2), 16)))

sites = [0x828AEA44, 0x828AEB54, 0x828B24D4, 0x82BF9334, 0x82BFA670,
         0x82C7A5A8, 0x82CB867C, 0x82CB8DE8, 0x82D45F8C, 0x82D7ECEC,
         0x82DE9D54, 0x82E0DF60]
for s in sites:
    sw = switches.get(s)
    if not sw:
        print(f"{s:08X}: NOT IN SWITCH TOML")
        continue
    lo, hi = min(sw["labels"]), max(sw["labels"])
    span = [sp for sp in spans if sp[0] <= s < sp[0] + sp[1]]
    txt = f"{span[0][0]:08X}+{span[0][1]:X}" if span else "NONE"
    ok_lo = span and span[0][0] <= lo
    ok_hi = span and hi < span[0][0] + span[0][1]
    print(f"{s:08X}: labels {lo:08X}..{hi:08X} span={txt} "
          f"lo_in={ok_lo} hi_in={ok_hi}")
