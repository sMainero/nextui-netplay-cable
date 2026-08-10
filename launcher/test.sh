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
export USERDATA_PATH="$ROOT/.userdata/$PLATFORM"

NP="$ROOT/Tools/$PLATFORM/Netplay.pak"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

# --- fake SD card ---------------------------------------------------------
mkdir -p "$USERDATA_PATH" "$SYSTEM_PATH/bin" "$SYSTEM_PATH/cores" "$NP/launcher" "$NP/bin/$PLATFORM" "$NP/state"
cp "$HERE/launch-stub.sh" "$HERE/install-stubs.sh" "$HERE/minarch.elf" "$HERE/wrap-pak.sh" \
   "$HERE/bind-mount.sh" "$HERE/mount-common.sh" "$HERE/pre-launch.sh" "$NP/launcher/"
chmod 755 "$NP/launcher"/*
: > "$NP/bin/$PLATFORM/netplay_shim.so"

# A stand-in minarch that just reports the arguments and env it was handed.
cat > "$SYSTEM_PATH/bin/minarch.elf" <<'EOF'
#!/bin/sh
[ "$NETPLAY_COMPAT_CORE" = "1" ] && echo "compat=1"
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

# Reports its $0-derived EMU_TAG, so the bind mount can be checked for keeping
# $0 at the pak's real path. Kept separate from the paks above because three
# tests compare their launch output byte for byte.
mkdir -p "$SYSTEM_PATH/paks/Emus/MD.pak"
cat > "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_EXE=picodrive
EMU_TAG=$(basename "$(dirname "$0")" .pak)
echo "tag=$EMU_TAG"
minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh"

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

# my282 selects the core inside a case statement rather than assigning EMU_EXE
# at the top level. An anchored match finds nothing there and silently installs
# no stubs at all, so keep a pak in that shape in the fixture.
mkdir -p "$SYSTEM_PATH/paks/Emus/GBA282.pak"
cat > "$SYSTEM_PATH/paks/Emus/GBA282.pak/launch.sh" <<'EOF'
#!/bin/sh
EMU_TAG=$(basename "$(dirname "$0")" .pak)
case "$EMU_TAG" in
	FC) EMU_EXE=fceumm ;;
	GB|GBC) EMU_EXE=gambatte ;;
	GBA282) EMU_EXE=gpsp ;;
	*) exit 1 ;;
esac
exec minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$1"
EOF
chmod 755 "$SYSTEM_PATH/paks/Emus/GBA282.pak/launch.sh"

echo "== install"
"$NP/launcher/install-stubs.sh" install > "$ROOT/install.log" 2>&1 || bad "installer exited non-zero"
[ -f "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" ] && ok "GBA stub installed"  || bad "GBA stub missing"
[ -f "$ROOT/Emus/$PLATFORM/GB.pak/launch.sh" ]  && ok "GB stub installed"   || bad "GB stub missing"
[ -f "$ROOT/Emus/$PLATFORM/VB.pak/launch.sh" ]  && bad "VB should be skipped (unsupported core)" || ok "VB skipped"
[ -f "$ROOT/Emus/$PLATFORM/GBA282.pak/launch.sh" ] && ok "my282 case-statement pak detected" \
	|| bad "my282 case-statement pak missed (anchored EMU_EXE match?)"
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

# A normal launch must not rewrite the same shared object to the SD card.
SHIM_INODE=$(ls -i "$NP/cores/gpsp_libretro.so" | awk '{print $1}')
OUT=$(NETPLAY_SESSION=/tmp/session "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
check "unchanged shim is not copied again" \
	"$(ls -i "$NP/cores/gpsp_libretro.so" | awk '{print $1}')" "$SHIM_INODE"

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
echo "== mandatory link cores and compatibility fallback"
mkdir -p "$NP/cores/override/$PLATFORM" "$NP/cores/compatibility/aarch64"
echo "pretend network-enabled core" > "$NP/cores/override/$PLATFORM/gpsp_libretro.so"
echo "pretend compatibility core" > "$NP/cores/compatibility/aarch64/fceumm_libretro.so"
printf 'role=host\nport=55437\n' > "$ROOT/compat.session"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$ROOT/Emus/$PLATFORM/GBA.pak/launch.sh" /roms/game.gba 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/override/$PLATFORM/gpsp_libretro.so" \
	&& ok "packaged gpSP used for link implementation" || bad "packaged gpSP ignored: $OUT"
echo "$OUT" | grep -q "compat=1" \
	&& ok "packaged-core launch is marked" || bad "compatibility environment missing: $OUT"
echo "$OUT" | grep -q "core=$NP/cores/gpsp_libretro.so" \
	&& ok "shim still staged under the real core name" || bad "shim name wrong: $OUT"

# Other cores stay on the installed build until the setup negotiation selects
# the matching compatibility build on both devices.
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/fceumm_libretro.so" \
	&& ok "installed core remains first choice" || bad "compatibility core used eagerly: $OUT"
printf 'compat_core.fceumm=1\n' >> "$ROOT/compat.session"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$NP/cores/compatibility/aarch64/fceumm_libretro.so" \
	&& ok "selected compatibility core used" || bad "compatibility fallback ignored: $OUT"
rm -rf "$NP/cores/compatibility"
OUT=$(NETPLAY_SESSION="$ROOT/compat.session" "$NP/launcher/minarch.elf" \
	"$SYSTEM_PATH/cores/fceumm_libretro.so" /roms/game.nes 2>&1)
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/fceumm_libretro.so" \
	&& ok "falls back to the system core" || bad "no fallback: $OUT"

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
echo "== bind-mount: stage system paks, writing nothing to the Emus tree"
GBA_BEFORE=$(cat "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh")
# Snapshot rather than assert emptiness: earlier install-stubs tests deliberately
# leave a foreign pak behind, so what matters is that we add nothing of our own.
EMUS_BEFORE=$(ls -R "$ROOT/Emus" 2>/dev/null)
"$NP/launcher/bind-mount.sh" sync > "$ROOT/bmsync.log" 2>&1 || bad "sync exited non-zero"
check "system pak untouched by staging" "$(cat "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh")" "$GBA_BEFORE"
check "launch.sh.old is the original, verbatim" "$(cat "$NP/mounts/GBA.pak/launch.sh.old")" "$GBA_BEFORE"
grep -q "Installed by Netplay.pak" "$NP/mounts/GBA.pak/launch.sh" \
	&& ok "staged launch.sh is ours" || bad "staged launch.sh not marked"
[ -d "$NP/mounts/VB.pak" ] && bad "staged a pak whose core we cannot drive" || ok "unsupported core not staged"

# The entire point of the rewrite: the Emus tree gains nothing.
check "Emus tree untouched by the mount route" "$(ls -R "$ROOT/Emus" 2>/dev/null)" "$EMUS_BEFORE"

echo
echo "== bind-mount: what the mount would present"
# cp -a of the staged dir over the pak's own path presents exactly what the bind
# mount does, so $0 - and therefore EMU_TAG - resolves identically.
cp -a "$NP/mounts/MD.pak/." "$SYSTEM_PATH/paks/Emus/MD.pak/"

OUT=$("$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" /roms/g.md 2>&1)
echo "$OUT" | grep -qx "tag=MD" && ok "EMU_TAG still resolves to MD under the mount" \
	|| bad "EMU_TAG wrong: $OUT"

OUT=$(NETPLAY_SESSION=/tmp/session "$SYSTEM_PATH/paks/Emus/MD.pak/launch.sh" /roms/g.md 2>&1)
echo "$OUT" | grep -q "core=$NP/cores/picodrive_libretro.so" \
	&& ok "mounted pak routes through the shim" || bad "no shim routing: $OUT"
echo "$OUT" | grep -q "real=$SYSTEM_PATH/cores/picodrive_libretro.so" \
	&& ok "real core passed to the shim" || bad "wrong real core: $OUT"

echo
echo "== bind-mount: staleness is detected"
"$NP/launcher/bind-mount.sh" status 2>&1 | grep -q "stale" && bad "clean staging reported stale" || ok "fresh staging not stale"
rm -rf "$SYSTEM_PATH/paks/Emus/GBA.pak"; mkdir -p "$SYSTEM_PATH/paks/Emus/GBA.pak"
printf '#!/bin/sh\nEMU_EXE=gpsp\necho updated\n' > "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh"
chmod 755 "$SYSTEM_PATH/paks/Emus/GBA.pak/launch.sh"
"$NP/launcher/bind-mount.sh" status 2>&1 | grep -q "stale    GBA" \
	&& ok "pak updated underneath is reported stale" || bad "stale staging not detected"

echo
echo "== bind-mount: mount ownership"
# Teardown must distinguish our exact stage source from a foreign bind mount.
OWN_TARGET="$SYSTEM_PATH/paks/Emus/GB.pak"
OWN_SOURCE="$NP/mounts/GB.pak"
FOREIGN_TARGET="$SYSTEM_PATH/paks/Emus/GBA.pak"
MOUNTS_FIXTURE="$ROOT/proc-mounts"
printf '%s %s none rw 0 0\n' "$OWN_SOURCE" "$OWN_TARGET" > "$MOUNTS_FIXTURE"
printf '%s %s none rw 0 0\n' "$ROOT/other/GBA.pak" "$FOREIGN_TARGET" >> "$MOUNTS_FIXTURE"
NETPLAY_MOUNTS_FILE="$MOUNTS_FIXTURE"
export NETPLAY_MOUNTS_FILE
MOUNT_STAGE="$NP/mounts"
MARKER="Installed by Netplay.pak"
. "$NP/launcher/mount-common.sh"
mount_is_ours "$OWN_TARGET" && ok "recognises Netplay-owned mount" || bad "owned mount not recognised"
mount_is_ours "$FOREIGN_TARGET" && bad "foreign mount claimed as ours" || ok "foreign mount left unowned"
unset NETPLAY_MOUNTS_FILE

echo
echo "== bind-mount: boot hook"
"$NP/launcher/bind-mount.sh" hook-install > /dev/null 2>&1
grep -q "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh" && ok "hook added to auto.sh" || bad "hook not added"
"$NP/launcher/bind-mount.sh" hook-install > /dev/null 2>&1
[ "$(grep -c "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh")" = "1" ] \
	&& ok "hook install is idempotent" || bad "hook added twice"

# Must not disturb hooks other paks registered the same way.
echo 'test -f /other/on-boot && /other/on-boot # Other.pak-on-boot' >> "$USERDATA_PATH/auto.sh"
"$NP/launcher/bind-mount.sh" hook-remove > /dev/null 2>&1
grep -q "Netplay.pak-on-boot" "$USERDATA_PATH/auto.sh" && bad "our hook survived removal" || ok "hook removed"
grep -q "Other.pak-on-boot" "$USERDATA_PATH/auto.sh" && ok "another pak's hook left alone" || bad "removed someone else's hook"

echo
echo "== bind-mount: boot never fails, however broken the staging"
rm -rf "$NP/mounts"
"$NP/launcher/bind-mount.sh" boot; rc=$?
[ "$rc" = "0" ] && ok "boot exits 0 with no staging at all" || bad "boot exited $rc - would break a boot"

echo
[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit $fail
