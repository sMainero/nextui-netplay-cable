#!/bin/sh
# Exercises the launcher against a fake SD tree: stub install/uninstall, and
# the minarch.elf shadow's routing decisions. Host-only, no device needed.
#
#   ./test.sh

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

export SDCARD_PATH="$ROOT"
export PLATFORM=tg5040
export SYSTEM_PATH="$ROOT/.system/$PLATFORM"

NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

# --- fake SD card ---------------------------------------------------------
mkdir -p "$SYSTEM_PATH/bin" "$SYSTEM_PATH/cores" "$NP/launcher" "$NP/bin/$PLATFORM" "$NP/state"
cp "$HERE/launch-stub.sh" "$HERE/install-stubs.sh" "$HERE/minarch.elf" "$HERE/wrap-pak.sh" "$NP/launcher/"
chmod 755 "$NP/launcher"/*
: > "$NP/bin/$PLATFORM/netplay_shim.so"

# A stand-in minarch that just reports the arguments and env it was handed.
cat > "$SYSTEM_PATH/bin/minarch.elf" <<'EOF'
#!/bin/sh
echo "minarch core=$1 rom=$2 real=$NETPLAY_REAL_CORE session=$NETPLAY_SESSION"
EOF
chmod 755 "$SYSTEM_PATH/bin/minarch.elf"

# System emu paks: two netplay-capable, one not.
for spec in "GBA:gpsp" "GB:gambatte" "VB:mednafen_vb"; do
	tag=${spec%%:*}; exe=${spec##*:}
	mkdir -p "$SYSTEM_PATH/paks/Emus/$tag.pak"
	cat > "$SYSTEM_PATH/paks/Emus/$tag.pak/launch.sh" <<EOF
#!/bin/sh
EMU_EXE=$exe
EMU_TAG=\$(basename "\$(dirname "\$0")" .pak)
minarch.elf "\$CORES_PATH/\${EMU_EXE}_libretro.so" "\$1"
EOF
	chmod 755 "$SYSTEM_PATH/paks/Emus/$tag.pak/launch.sh"
done

# An EXTRAS pak already sitting on the SD path - must be left alone. Bundles
# its own core and locates it via CORES_PATH=$(dirname "$0"), like FBN/SUPA.
mkdir -p "$ROOT/Emus/$PLATFORM/FBN.pak"
cat > "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_EXE=fbneo
CORES_PATH=$(dirname "$0")
EMU_TAG=$(basename "$(dirname "$0")" .pak)
echo "tag=$EMU_TAG"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh"
echo "fake fbneo core" > "$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so"
FBN_BEFORE=$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")

export CORES_PATH="$SYSTEM_PATH/cores"

echo "== install"
"$NP/launcher/install-stubs.sh" install > "$ROOT/install.log" 2>&1 || bad "installer exited non-zero"
[ -f "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" ] && ok "GBA stub installed"  || bad "GBA stub missing"
[ -f "$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" ]  && ok "GB stub installed"   || bad "GB stub missing"
[ -f "$ROOT/Emus/$PLATFORM/VB.pak/launch.sh" ]  && bad "VB should be skipped (unsupported core)" || ok "VB skipped"
check "EXTRAS pak untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"
grep -q "skip FBN (fbneo)" "$ROOT/install.log" && ok "EXTRAS pak reported as uncovered" || bad "no report for FBN"

echo
echo "== launch with no session (must be byte-identical to stock)"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "routes to real core" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session="

echo
echo "== launch with a session armed"
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "routes through shim" "$OUT" \
	"minarch core=$NP/cores/gpsp_libretro.so rom=/roms/game.gba real=$SYSTEM_PATH/cores/gpsp_libretro.so session=/tmp/session"
[ -f "$NP/cores/gpsp_libretro.so" ] && ok "shim staged under the real core's filename" \
	|| bad "shim not staged as gpsp_libretro.so"

echo
echo "== shim staging preserves core.name"
# minarch: basename truncated at the last underscore -> feeds config_dir/states_dir
NAME=$(basename "$NP/cores/gpsp_libretro.so" | sed 's/_[^_]*$//')
check "core.name unchanged" "$NAME" "gpsp"

echo
echo "== missing shim falls back to stock"
mv "$NP/bin/$PLATFORM/netplay_shim.so" "$NP/bin/$PLATFORM/hidden.so"
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1 | tail -n 1)
check "falls back" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session=/tmp/session"
mv "$NP/bin/$PLATFORM/hidden.so" "$NP/bin/$PLATFORM/netplay_shim.so"

echo
echo "== force-shim file routes without an env var"
# Game-list launches are started by NextUI, so the force flag has to be a file.
mkdir -p "$NP/state"
: > "$NP/state/force-shim"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "force file routes through shim" || bad "force file ignored: $OUT"
rm -f "$NP/state/force-shim"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "clearing it restores stock" "$OUT" "minarch core=$SYSTEM_PATH/cores/gpsp_libretro.so rom=/roms/game.gba real= session="

echo
echo "== session file is discovered without an env var"
mkdir -p "$NP/state"
printf 'role=host\nport=55437\n' > "$NP/state/session"
OUT=$("$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "session file routes through shim" || bad "session file ignored: $OUT"
echo "$OUT" | grep -q "session=$NP/state/session" \
	&& ok "NETPLAY_SESSION exported to minarch" || bad "session not exported: $OUT"
rm -f "$NP/state/session"

echo
echo "== uninstall"
"$NP/launcher/install-stubs.sh" uninstall > /dev/null 2>&1
[ -e "$ROOT/Emus/$PLATFORM/GBA.pak" ] && bad "GBA stub dir left behind" || ok "GBA stub removed"
check "EXTRAS pak still untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"

echo
echo "== uninstall leaves a pak that became someone else's alone"
"$NP/launcher/install-stubs.sh" install > /dev/null 2>&1
echo "#!/bin/sh
# replaced by another pak" > "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh"
"$NP/launcher/install-stubs.sh" uninstall > "$ROOT/uninstall2.log" 2>&1
[ -f "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" ] && ok "foreign launch.sh preserved" || bad "removed a file that was not ours"

"$NP/launcher/install-stubs.sh" uninstall > /dev/null 2>&1

echo
echo "== wrap-pak: stage an EXTRAS pak without touching it"
cp "$HERE/wrap-pak.sh" "$NP/launcher/" && chmod 755 "$NP/launcher/wrap-pak.sh"
"$NP/launcher/wrap-pak.sh" sync > "$ROOT/sync.log" 2>&1 || bad "sync exited non-zero"
check "original still untouched" "$(cat "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh")" "$FBN_BEFORE"
check "launch.sh.old is the original, verbatim" "$(cat "$NP/wrapped/FBN.pak/launch.sh.old")" "$FBN_BEFORE"
grep -q "Installed by Netplay.pak" "$NP/wrapped/FBN.pak/launch.sh" && ok "staged launch.sh is ours" || bad "staged launch.sh not marked"
[ -f "$NP/wrapped/FBN.pak/fbneo_libretro.so" ] && ok "bundled core copied into staging" || bad "bundled core missing from staging"

echo
echo "== wrap-pak: what the bind mount would present"
# cp -a of the staged dir over the original path presents exactly the contents
# the bind mount does, so $0-derived EMU_TAG and CORES_PATH resolve identically.
rm -rf "$ROOT/Emus/$PLATFORM/FBN.pak"
cp -a "$NP/wrapped/FBN.pak" "$ROOT/Emus/$PLATFORM/FBN.pak"

OUT=$("$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" /roms/game.zip 2>&1)
echo "$OUT" | grep -qx "tag=FBN" && ok "EMU_TAG still resolves to FBN" || bad "EMU_TAG wrong: $OUT"
echo "$OUT" | grep -q "core=$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so" \
	&& ok "CORES_PATH finds the bundled core" || bad "CORES_PATH wrong: $OUT"

OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/FBN.pak/launch.sh" /roms/game.zip 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/fbneo_libretro.so" \
	&& ok "wrapped pak routes through the shim" || bad "no shim routing: $OUT"
echo "$OUT" | grep -q "real=$ROOT/Emus/$PLATFORM/FBN.pak/fbneo_libretro.so" \
	&& ok "real core passed to the shim" || bad "wrong real core: $OUT"

echo
[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit $fail
