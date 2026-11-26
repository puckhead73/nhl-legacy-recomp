#!/bin/bash
# Give the native Linux build a proper desktop / Steam library entry.
#
#   install-steam-entry.sh [options]
#
#   --install-dir DIR   Folder holding nhllegacy + run-native.sh (default: the
#                       current directory if it has run-native.sh, else the
#                       folder this script is in).
#   --name NAME         Display name (default: "NHL Legacy").
#   --art DIR           Artwork folder (default: the repo's packaging/steam, or
#                       a "steam-art" folder next to this script). Uses whichever
#                       of these exist:
#                         cover.png   600x900  library tile
#                         header.png  920x430  wide tile / recent games
#                         hero.png    1920x620 background on the game page
#                         logo.png    transparent title over the hero
#                         icon.png    256x256  icon
#   --steam             Also fix up Steam itself: name, icon and artwork of the
#                       non-Steam shortcut that launches run-native.sh, adding
#                       the shortcut if there is none. Steam must be closed
#                       (it rewrites its shortcut file on exit).
#   --uninstall         Remove the desktop entry and icon (Steam is left alone).
#
# Without --steam this only installs a desktop entry, so the game shows up with
# its name and icon in the application menu and in Steam's
# "Add a Non-Steam Game" list. The shipped artwork is plain original placeholder
# art; point --art at your own images to replace it.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
name="NHL Legacy"
install_dir=""
art=""
do_steam=0
uninstall=0
while [ $# -gt 0 ]; do
  case "$1" in
    --install-dir) install_dir=$2; shift 2 ;;
    --name) name=$2; shift 2 ;;
    --art) art=$2; shift 2 ;;
    --steam) do_steam=1; shift ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
desktop="$apps/nhl-legacy-recomp.desktop"
icon_dst="$icons/nhl-legacy-recomp.png"

if [ "$uninstall" = 1 ]; then
  rm -f "$desktop" "$icon_dst"
  command -v update-desktop-database >/dev/null && update-desktop-database "$apps" 2>/dev/null || true
  echo "removed $desktop"
  exit 0
fi

if [ -z "$install_dir" ]; then
  if [ -f "./run-native.sh" ]; then install_dir=$(pwd); else install_dir=$here; fi
fi
install_dir=$(cd "$install_dir" && pwd)
launcher="$install_dir/run-native.sh"
[ -f "$launcher" ] || { echo "no run-native.sh in $install_dir (use --install-dir)" >&2; exit 1; }
chmod +x "$launcher"

if [ -z "$art" ]; then
  for d in "$here/steam-art" "$here/../../packaging/steam" "$install_dir/steam-art"; do
    [ -d "$d" ] && { art=$(cd "$d" && pwd); break; }
  done
fi

# --- desktop entry + icon ------------------------------------------------------
mkdir -p "$apps"
icon_line=""
if [ -n "$art" ] && [ -f "$art/icon.png" ]; then
  mkdir -p "$icons"
  cp "$art/icon.png" "$icon_dst"
  icon_line="Icon=$icon_dst"
fi
{
  echo "[Desktop Entry]"
  echo "Type=Application"
  echo "Name=$name"
  echo "Comment=Native build of the NHL Legacy static recompilation"
  echo "Exec=\"$launcher\""
  echo "Path=$install_dir"
  [ -n "$icon_line" ] && echo "$icon_line"
  echo "Terminal=false"
  echo "Categories=Game;SportsGame;"
  echo "StartupWMClass=nhllegacy"
} > "$desktop"
chmod +x "$desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" 2>/dev/null || true
echo "desktop entry: $desktop"

[ "$do_steam" = 1 ] || { echo "done (run again with --steam to set the Steam shortcut's name, icon and artwork)"; exit 0; }

# --- Steam shortcut + artwork ---------------------------------------------------
if pgrep -x steam >/dev/null 2>&1; then
  echo "Steam is running. Close it first (in Gaming Mode: switch to Desktop and exit Steam)," >&2
  echo "then run this again - Steam overwrites its shortcut file when it exits." >&2
  exit 1
fi
command -v python3 >/dev/null || { echo "python3 is needed for --steam" >&2; exit 1; }

steam_root=""
for d in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
  [ -d "$d/userdata" ] && { steam_root=$d; break; }
done
[ -n "$steam_root" ] || { echo "no Steam userdata folder found" >&2; exit 1; }

NAME="$name" LAUNCHER="$launcher" START_DIR="$install_dir" ART="$art" ICON="${icon_line#Icon=}" \
STEAM_ROOT="$steam_root" python3 - <<'PY'
import os, shutil, struct, sys, time, zlib

name = os.environ["NAME"]; launcher = os.environ["LAUNCHER"]; start_dir = os.environ["START_DIR"]
art = os.environ.get("ART", ""); icon = os.environ.get("ICON", ""); root = os.environ["STEAM_ROOT"]

# Binary VDF: 0x00 = nested map, 0x01 = string, 0x02 = int32, 0x08 = end of map.
def parse(buf, i=0):
    out = []
    while True:
        t = buf[i]; i += 1
        if t == 0x08:
            return out, i
        j = buf.index(b"\x00", i); key = buf[i:j].decode("utf-8", "replace"); i = j + 1
        if t == 0x00:
            val, i = parse(buf, i)
        elif t == 0x01:
            j = buf.index(b"\x00", i); val = buf[i:j].decode("utf-8", "replace"); i = j + 1
        elif t == 0x02:
            val = struct.unpack_from("<i", buf, i)[0]; i += 4
        else:
            raise ValueError("unsupported VDF type %#x at %d" % (t, i - 1))
        out.append((t, key, val))

def dump(items):
    b = bytearray()
    for t, key, val in items:
        b.append(t); b += key.encode("utf-8") + b"\x00"
        if t == 0x00:
            b += dump(val)
        elif t == 0x01:
            b += val.encode("utf-8") + b"\x00"
        else:
            b += struct.pack("<i", val)
    b.append(0x08)
    return bytes(b)

def get(entry, key):
    for t, k, v in entry:
        if k.lower() == key.lower():
            return v
    return None

def put(entry, t, key, val):
    for n, (_, k, _v) in enumerate(entry):
        if k.lower() == key.lower():
            entry[n] = (t, k, val); return
    entry.append((t, key, val))

users = [u for u in sorted(os.listdir(os.path.join(root, "userdata")))
         if u.isdigit() and u != "0" and os.path.isdir(os.path.join(root, "userdata", u, "config"))]
if not users:
    sys.exit("no Steam user folder under userdata")

quoted = '"%s"' % launcher
for user in users:
    cfg = os.path.join(root, "userdata", user, "config")
    path = os.path.join(cfg, "shortcuts.vdf")
    if os.path.exists(path):
        data = open(path, "rb").read()
        top, _ = parse(data)
        shutil.copy2(path, path + ".bak-" + time.strftime("%Y%m%d-%H%M%S"))
    else:
        top = [(0x00, "shortcuts", [])]
    shortcuts = top[0][2]

    entry = None
    for _, _, e in shortcuts:
        exe = (get(e, "Exe") or "").strip('"')
        if exe == launcher:
            entry = e; break
    if entry is None:
        entry = []
        # Same scheme Steam uses when it has to make one up: a CRC of exe + name
        # with the top bit set, stored as a signed 32-bit value.
        crc = zlib.crc32((quoted + name).encode("utf-8")) | 0x80000000
        put(entry, 0x02, "appid", struct.unpack("<i", struct.pack("<I", crc))[0])
        put(entry, 0x01, "AppName", name)
        put(entry, 0x01, "Exe", quoted)
        put(entry, 0x01, "StartDir", '"%s"' % start_dir)
        put(entry, 0x01, "icon", icon)
        put(entry, 0x01, "ShortcutPath", "")
        put(entry, 0x01, "LaunchOptions", "")
        for k, v in (("IsHidden", 0), ("AllowDesktopConfig", 1), ("AllowOverlay", 1), ("OpenVR", 0),
                     ("Devkit", 0)):
            put(entry, 0x02, k, v)
        put(entry, 0x01, "DevkitGameID", "")
        put(entry, 0x02, "DevkitOverrideAppID", 0)
        put(entry, 0x02, "LastPlayTime", 0)
        put(entry, 0x01, "FlatpakAppID", "")
        put(entry, 0x00, "tags", [])
        shortcuts.append((0x00, str(len(shortcuts)), entry))
        action = "added"
    else:
        put(entry, 0x01, "AppName", name)
        if icon:
            put(entry, 0x01, "icon", icon)
        action = "updated"

    appid = get(entry, "appid")
    if appid is None:
        crc = zlib.crc32((quoted + name).encode("utf-8")) | 0x80000000
        appid = struct.unpack("<i", struct.pack("<I", crc))[0]
        put(entry, 0x02, "appid", appid)
    uid = appid & 0xFFFFFFFF

    with open(path, "wb") as f:
        f.write(dump(top))
    print("Steam user %s: shortcut %s (app id %d)" % (user, action, uid))

    if art:
        grid = os.path.join(cfg, "grid"); os.makedirs(grid, exist_ok=True)
        for src, dst in (("cover.png", "%dp.png"), ("header.png", "%d.png"),
                         ("hero.png", "%d_hero.png"), ("logo.png", "%d_logo.png")):
            s = os.path.join(art, src)
            if os.path.isfile(s):
                shutil.copyfile(s, os.path.join(grid, dst % uid))
                print("  artwork: " + src)
PY
echo "done - start Steam; \"$name\" is in your library under Non-Steam."
