#!/bin/sh
set -e

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
PAK="$ROOT/Emus/tg5040/MGBA.pak"
BIN="$ROOT/bin"
mkdir -p "$PAK" "$BIN" "$ROOT/bios" "$ROOT/saves" "$ROOT/cheats" \
	"$ROOT/logs" "$ROOT/userdata" "$ROOT/Tools/tg5040/Netplay.pak/state"
cp testing/MGBA.pak/launch.sh "$PAK/launch.sh"
: > "$PAK/mgba_libretro.so"
: > "$PAK/mgba_dual_libretro.so"
: > "$PAK/netplay_shim.tg5040.so"
printf '%s\n' '#!/bin/sh' \
	'printf "arg=%s real=%s session=%s\n" "$1" "$NETPLAY_REAL_CORE" "$NETPLAY_SESSION"' \
	> "$BIN/minarch.elf"
chmod +x "$BIN/minarch.elf" "$PAK/launch.sh"

run_launcher() {
	: > "$ROOT/logs/MGBA.txt"
	PATH="$BIN:$PATH" SDCARD_PATH="$ROOT" PLATFORM=tg5040 \
		BIOS_PATH="$ROOT/bios" SAVES_PATH="$ROOT/saves" \
		CHEATS_PATH="$ROOT/cheats" LOGS_PATH="$ROOT/logs" \
		USERDATA_PATH="$ROOT/userdata" "$PAK/launch.sh" "$ROOT/game.gba"
}

echo "== mGBA paired-core launcher"
run_launcher
grep -q "real=$PAK/mgba_libretro.so" "$ROOT/logs/MGBA.txt"
grep -q 'session=$' "$ROOT/logs/MGBA.txt"
echo "  ok   ordinary sessions select stock mGBA"

printf 'instanced_mgba=1\n' > "$ROOT/Tools/tg5040/Netplay.pak/state/session"
run_launcher
grep -q "real=$PAK/mgba_dual_libretro.so" "$ROOT/logs/MGBA.txt"
grep -q "session=$ROOT/Tools/tg5040/Netplay.pak/state/session" "$ROOT/logs/MGBA.txt"
echo "  ok   negotiated paired sessions select mGBA Dual"

NETPLAY_DUAL_DISABLE=1; export NETPLAY_DUAL_DISABLE
run_launcher
grep -q "real=$PAK/mgba_libretro.so" "$ROOT/logs/MGBA.txt"
echo "  ok   demotion marker selects the network fallback core"
