#!/bin/bash
# Builds the decomp ELF that rogue_squadron.toml expects from the current decomp, without modifying the decomp repo.
# Usage: tools/vita/make_compat_elf.sh [decomp_dir] [venv_dir]
set -euo pipefail

RECOMP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DECOMP_DIR="$(cd "${1:-$RECOMP_DIR/../rogue_squadron64}" && pwd)"
VENV_DIR="$(cd "${2:-$RECOMP_DIR/../.venv}" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

[ -f "$DECOMP_DIR/rogue_squadron.z64" ] || { echo "Missing $DECOMP_DIR/rogue_squadron.z64"; exit 1; }

# Work on a copy so splat output and symbol fixes never touch the decomp checkout.
rsync -a --exclude .git --exclude asm --exclude build --exclude assets "$DECOMP_DIR/" "$WORK/"

"$VENV_DIR/bin/python" - "$WORK" <<'PY'
import os, re, sys, glob, yaml

work = sys.argv[1]
os.chdir(work)
import splat.util.symbols as splat_symbols
known = set(re.findall(r'attr_name == "(\w+)"', open(splat_symbols.__file__).read()))

y = yaml.safe_load(open("roguesquadron.yaml"))
opts = y["options"]
paths = opts["symbol_addrs_path"]
overlay_files = {p for p in paths if re.search(r"(mission|menu|cinematic)_overlay\.txt$", p)}
SYM_RE = re.compile(r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;")
SEG_PREFIX_RE = re.compile(r"^func_(?:main|libultra|zlib|mission_overlay|menu_overlay|cinematic_overlay)_([0-9A-F]{8})\b")

# Names rogue_squadron.toml hooks use but the public decomp names differently (address in mission_overlay.txt).
MISSION_RENAMES = {
    0x800B4588: "updateXwingFlightControls",
    0x800B5B4C: "updateAwingFlightControls",
    0x800B77D0: "updateYwingFlightControls",
    0x800B8C30: "updateVwingFlightControls",
    0x800BA620: "updateFalconFlightControls",
    0x800BBF6C: "updateTieInterceptorFlightControls",
    0x800BCE74: "updateT16FlightControls",
    0x800BDC9C: "updateNabooStarfighterFlightControls",
    0x800AADD4: "updateSnowSpeederFlightControls",
}

# Symbols declared in an overlay file win over their copies in the other files.
overlay_syms = set()
for p in overlay_files:
    for line in open(p, encoding="utf-8"):
        m = SYM_RE.match(line.strip())
        if m:
            overlay_syms.add((m[1], int(m[2], 16)))

seen = set()
stats = {"comments": 0, "dup": 0, "overlay_dup": 0, "seg_prefix": 0, "renamed": 0}
for p in sorted(paths, key=lambda p: p not in overlay_files):
    out = []
    for line in open(p, encoding="utf-8"):
        s = line.strip()
        if SEG_PREFIX_RE.match(s):
            s = SEG_PREFIX_RE.sub(r"func_\1", s)
            stats["seg_prefix"] += 1
        m = SYM_RE.match(s)
        if m and p.endswith("mission_overlay.txt") and int(m[2], 16) in MISSION_RENAMES:
            s = MISSION_RENAMES[int(m[2], 16)] + s[len(m[1]):]
            stats["renamed"] += 1
            m = SYM_RE.match(s)
        if not m:
            out.append(line)
            continue
        key = (m[1], int(m[2], 16))
        if p not in overlay_files and key in overlay_syms:
            stats["overlay_dup"] += 1
            continue
        if key in seen:
            stats["dup"] += 1
            continue
        seen.add(key)
        # Keep only attributes splat understands; free-text comments break its parser.
        c = s.find("//")
        main = s if c < 0 else s[:c].strip()
        tokens = [] if c < 0 else [t for t in s[c + 2:].split()
                                   if re.fullmatch(r"(\w+):([^:\s]+)", t) and t.split(":")[0] in known]
        new = main + (" // " + " ".join(tokens) if tokens else "")
        if new != s:
            stats["comments"] += 1
        out.append(new + "\n")
    open(p, "w", encoding="utf-8").write("".join(out))

# rogue_squadron.toml uses func_XXXXXXXX names, not the newer $SEG_$VRAM ones.
opts["symbol_name_format"] = "$VRAM"
yaml.safe_dump(y, open("roguesquadron.yaml", "w"), sort_keys=False)
print("symbol cleanup:", stats)
PY

(cd "$WORK" && "$VENV_DIR/bin/splat" split roguesquadron.yaml > "$WORK/split.log" 2>&1) || {
    tail -20 "$WORK/split.log"; echo "splat failed"; exit 1; }
# Function boundaries come from the reference table of the ELF the recomp was built with; the decomp only supplies names.
"$VENV_DIR/bin/python" - "$WORK/tools/make_elf.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "    resolved = resolved_sized\n"
assert anchor in src, "make_elf.py layout changed"
block = r'''
    ref_path = os.environ.get("RS64_REF_SYMS")
    if ref_path:
        shndx_of = {s[3]: i + 1 for i, s in enumerate(SECTIONS)}
        ref, sec = {}, None
        for line in open(ref_path):
            m = re.match(r'name = "(.*)"', line)
            if m:
                sec = shndx_of.get(m.group(1))
                continue
            m = re.search(r'name = "(\w+)", vram = (0x\w+), size = (0x\w+)', line)
            if m and sec:
                ref[(sec, int(m.group(2), 16))] = (int(m.group(3), 16), m.group(1))
        ours = {(r[4], r[1]): r[0] for r in resolved if r[2] and r[4] not in (SHN_UNDEF, SHN_ABS)}
        ref_keys = {(n, vram) for (_, vram), (_, n) in ref.items()}
        data = [r for r in resolved if not (r[2] and r[4] not in (SHN_UNDEF, SHN_ABS)) and (r[0], r[1]) not in ref_keys]
        used = {r[0] for r in data}
        funcs, from_decomp = [], 0
        for (shndx, vram), (size, ref_name) in sorted(ref.items(), key=lambda kv: (kv[0][1], kv[0][0])):
            name = ours.get((shndx, vram))
            if name and name not in used:
                from_decomp += 1
            else:
                name = ref_name if ref_name not in used else f"func_{vram:08X}_s{shndx}"
            used.add(name)
            funcs.append((name, vram, True, size, shndx))
        resolved = sorted(data + funcs, key=lambda r: r[1])
        print(f"Reference boundaries: {len(funcs)} functions, {from_decomp} named from decomp, {len(ours) - from_decomp} decomp-only dropped")
'''
open(path, "w").write(src.replace(anchor, anchor + block, 1))
PY
(cd "$WORK" && RS64_REF_SYMS="$RECOMP_DIR/syms/rogue_squadron.syms.toml" python3 tools/make_elf.py | tail -3)

mkdir -p "$DECOMP_DIR/build"
cp "$WORK/build/roguesquadron.elf" "$DECOMP_DIR/build/roguesquadron.elf"
echo "ELF written to $DECOMP_DIR/build/roguesquadron.elf"
