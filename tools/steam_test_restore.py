# Takes an Add to Steam test out of a real Steam (Steam closed; tools/run_game.ps1 test copies): shortcuts.vdf back
# to the backup when only our entry was appended; localconfig.vdf keeps Steam's own changes since the backup (a Steam
# that ran for hours saves them when it closes) and loses only our app's block and the library selection on it.
# Dry run by default; --write writes. Usage: python -I steam_test_restore.py <config dir> <backup dir> <appid> [--write]
import difflib
import json
import os
import re
import sys

config, backup, appid = sys.argv[1], sys.argv[2], sys.argv[3]
write = "--write" in sys.argv


def id_count(text):
    # The app id as a whole number, not inside a longer one.
    return len(re.findall(r"(?<!\d)%s(?!\d)" % re.escape(appid), text))


def read(p):
    with open(p, "rb") as f:
        return f.read()


# shortcuts.vdf: binary; the test only appended one entry before the two closing bytes.
sb, sc = read(os.path.join(backup, "shortcuts.vdf")), read(os.path.join(config, "shortcuts.vdf"))
prefix_same = sc[: len(sb) - 2] == sb[:-2] and sb.endswith(b"\x08\x08") and sc.endswith(b"\x08\x08")
added = sc[len(sb) - 2 : -2]
print("shortcuts.vdf: backup", len(sb), "now", len(sc), "| old entries unchanged:", prefix_same,
      "| added part names Battlefield 1943 Reborn:", b"Battlefield 1943 Reborn" in added)

# localconfig.vdf: text; drop our "<appid>" { ... } block under apps.
lb = read(os.path.join(backup, "localconfig.vdf")).decode("latin-1")
lc = read(os.path.join(config, "localconfig.vdf")).decode("latin-1")
lines = lc.split("\n")
start = next(i for i, l in enumerate(lines) if l.strip() == '"%s"' % appid)
assert lines[start + 1].strip() == "{", lines[start + 1]
depth, end = 0, None
for j in range(start + 1, len(lines)):
    s = lines[j].strip()
    depth += s == "{"
    depth -= s == "}"
    if depth == 0:
        end = j
        break
indent = len(lines[start]) - len(lines[start].lstrip("\t"))
parent = next(lines[k].strip() for k in range(start - 1, -1, -1)
              if len(lines[k]) - len(lines[k].lstrip("\t")) < indent and lines[k].strip() not in ("{", "}"))
print("our block: lines", start + 1, "to", end + 1, "under", parent, "|", " ".join(l.strip() for l in lines[start : end + 1]))
assert parent == '"apps"'
out = "\n".join(lines[:start] + lines[end + 1 :])


# The library's selection, if it is on our app: back to the backup's selection.
def ui_state(text):
    for l in text.split("\n"):
        if l.strip().startswith('"UIStoreLocalSteamUIState"'):
            return l, re.match(r'(\s*"UIStoreLocalSteamUIState"\s*")(.*)("\s*)$', l)
    return None, None


cur_line, cm = ui_state(out)
old_line, om = ui_state(lb)
if cm and om and id_count(cm.group(2)):
    cur = json.loads(cm.group(2).replace('\\"', '"'))
    old = json.loads(om.group(2).replace('\\"', '"'))
    print("selection now", cur.get("currentSelection"), "| before", old.get("currentSelection"))
    cur["currentSelection"] = old.get("currentSelection")
    out = out.replace(cur_line, cm.group(1) + json.dumps(cur, separators=(",", ":")).replace('"', '\\"') + cm.group(3))
print("our app id left in localconfig:", id_count(out))

# Every other difference against the backup, for review: Steam's own since the backup.
diff = [d for d in difflib.unified_diff(lb.split("\n"), out.split("\n"), lineterm="", n=0)
        if d[:1] in "+-" and d[:3] not in ("+++", "---")]
print("other differences against the backup:", len(diff), "lines")
for d in diff[:40]:
    print("   ", (d[:160] + "...") if len(d) > 160 else d)

if write:
    assert prefix_same and id_count(out) == id_count(lb)
    with open(os.path.join(config, "shortcuts.vdf"), "wb") as f:
        f.write(sb)
    tmp = os.path.join(config, "localconfig.vdf.bfrestore")
    with open(tmp, "wb") as f:
        f.write(out.encode("latin-1"))
    os.replace(tmp, os.path.join(config, "localconfig.vdf"))
    print("written")
