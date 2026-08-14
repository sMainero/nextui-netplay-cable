#!/bin/sh
# The peer-cartridge lookup, against a real zipped library.
#
# This is the path Oracle of Seasons/Ages and Gold/Silver need: each device has
# to find the other's ROM among its own files, and a normal library is zipped.
# The version that shipped before could not - it filtered on file size and only
# considered bare .gb files - so both games silently fell back to the link cable
# with no indication that anything was wrong.
#
# The fixture is hostile in the way a real library is: every decoy has the same
# uncompressed size as the target, because Game Boy ROM sizes are powers of two
# and hundreds of cartridges collide on size alone. Only the CRC32 separates
# them, and only the digest confirms them.
set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "== peer cartridge lookup"
if ! command -v python3 >/dev/null 2>&1; then
	echo "  SKIP python3 is needed to build the zip fixture"
	exit 0
fi

mkdir -p "$OUT/Roms"
python3 - "$OUT" <<'PY'
import os, sys, zipfile
out = sys.argv[1]
roms = os.path.join(out, "Roms")

SIZE = 65536
def rom(seed):
    return bytes(((i * 7 + seed * 31) & 0xFF) for i in range(SIZE))

target = rom(1)
open(os.path.join(out, "target.bin"), "wb").write(target)
with zipfile.ZipFile(os.path.join(roms, "target.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Target Game (USA).gbc", target)

for i in range(2, 40):
    with zipfile.ZipFile(os.path.join(roms, "decoy%02d.zip" % i), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Decoy %d (USA).gbc" % i, rom(i))

# An archive holding nothing that looks like a cartridge, and a file that is not
# an archive at all: both must be skipped without upsetting the scan.
with zipfile.ZipFile(os.path.join(roms, "notes.zip"), "w") as z:
    z.writestr("readme.txt", "not a cartridge")
open(os.path.join(roms, "stray.gbc"), "wb").write(rom(99))
PY

$CC -O1 -g -Wall -o "$OUT/romscan_test" romscan_test.c ../romscan.c -I.. -lz
"$OUT/romscan_test" "$OUT/Roms" "$OUT/romscan.cache"
