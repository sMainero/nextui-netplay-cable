#!/bin/sh
set -eu

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
SD="$ROOT/card"
NP="$SD/Tools/tg5040/Netplay.pak"
STATE="$SD/.userdata/shared/Netplay"
mkdir -p "$NP/launcher"
cp "$(dirname "$0")/../launcher/state-path.sh" "$NP/launcher/"
for p in tg5040 h700 my282; do
	mkdir -p "$SD/Emus/$p" "$NP/cores/mgba/$p/MGBA.pak"
	printf 'new-%s\n' "$p" > "$NP/cores/mgba/$p/MGBA.pak/mgba_libretro.so"
	printf '#!/bin/sh\n' > "$NP/cores/mgba/$p/MGBA.pak/launch.sh"
done
mkdir -p "$SD/Emus/tg5040/MGBA.pak" "$SD/Emus/tg5040/MGBA.pak.bak" \
	"$SD/Saves/MGBA/game" "$SD/.userdata/shared/MGBA-mgba/game"
printf old > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
printf occupied > "$SD/Emus/tg5040/MGBA.pak.bak/file"
printf save-old > "$SD/Saves/MGBA/game/save.sav"
printf state-old > "$SD/.userdata/shared/MGBA-mgba/game/state.st0"

run() {
	SDCARD_PATH="$SD" PLATFORM=tg5040 NETPLAY_PAK="$NP" NETPLAY_STATE="$STATE" \
		sh "$PWD/launcher/mgba-manage.sh" "$@"
}

echo '== mGBA managed installation'
run install | grep -q '^ok=1$'
test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = new-tg5040
test "$(cat "$SD/Emus/tg5040/MGBA.pak.bak.1/mgba_libretro.so")" = old
test "$(cat "$STATE/mgba-save-backup"/*/Saves/MGBA/game/save.sav)" = save-old
test -f "$SD/Emus/h700/MGBA.pak/.netplay-installed"
echo '  ok   installs all represented platforms and avoids .bak collision'

printf changed > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=mismatch$'
run ignore >/dev/null
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=ignored$'
printf changed-again > "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so"
OUT=$(run status); echo "$OUT" | grep -q '^platform.tg5040=mismatch$'
echo '  ok   ignores only one exact installed/expected hash pair'

run install >/dev/null
test "$(cat "$SD/Emus/tg5040/MGBA.pak.bak.1/mgba_libretro.so")" = old
echo '  ok   reinstall preserves the original backup'

printf save-current > "$SD/Saves/MGBA/game/save.sav"
printf state-current > "$SD/.userdata/shared/MGBA-mgba/game/state.st0"
run restore 1 1 1 >/dev/null
test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = old
test "$(cat "$SD/Saves/MGBA/game/save.sav")" = save-old
test "$(cat "$SD/.userdata/shared/MGBA-mgba/game/state.st0")" = state-old
test "$(cat "$STATE/mgba-restore-collisions"/*/saves/game/save.sav)" = save-current
test "$(cat "$STATE/mgba-restore-collisions"/*/states/game/state.st0)" = state-current
test ! -e "$SD/Emus/h700/MGBA.pak"
echo '  ok   restores originals and preserves overwritten progress'

echo
echo '== arm-time ensure (additive only)'
# Start from a device with no mGBA pak at all, as a fresh card would be. The
# .bak siblings from the phases above have to go too, or the "ensure did not
# create a backup" assertion below would be reading their leftovers.
rm -rf "$SD/Emus"/*/MGBA.pak "$SD/Emus"/*/MGBA.pak.bak* "$STATE/mgba"
for p in tg5040 h700 my282; do mkdir -p "$SD/Emus/$p"; done
OUT=$(run ensure)
echo "$OUT" | grep -q '^ok=1$' \
	&& test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = new-tg5040 \
	&& echo '  ok   ensure installs when the device has no mGBA pak'

# Second call must be a no-op: an existing pak is never moved aside, which is the
# whole reason this verb exists rather than calling install().
ls "$SD/Emus/tg5040" | grep -q '^MGBA.pak.bak' && { echo '  FAIL backup created'; exit 1; }
OUT=$(run ensure)
echo "$OUT" | grep -q '^skipped=1$' \
	&& echo "$OUT" | grep -q '^reason=.*present' \
	&& test "$(cat "$SD/Emus/tg5040/MGBA.pak/mgba_libretro.so")" = new-tg5040 \
	&& ! ls "$SD/Emus/tg5040" | grep -q '^MGBA.pak.bak' \
	&& echo '  ok   ensure leaves an existing pak exactly where it is'

# A build that ships no mGBA pak must not invent one.
rm -rf "$SD/Emus"/*/MGBA.pak "$SD/Emus"/*/MGBA.pak.bak* "$NP"/cores/mgba/*/MGBA.pak "$STATE/mgba"
OUT=$(run ensure)
echo "$OUT" | grep -q '^skipped=1$' \
	&& echo "$OUT" | grep -q '^reason=.*ships no mGBA pak' \
	&& test ! -e "$SD/Emus/tg5040/MGBA.pak" \
	&& echo '  ok   ensure does nothing when this build has no mGBA pak'
