#!/bin/sh
#
# Entry point for the shim test build - staged as launch.sh by `make dist`.
#
# This is deliberately NOT the shipping pak's launch.sh, which runs netplay.elf:
# that app installs patched system binaries, which is the thing the shim
# architecture exists to avoid. A test build must never do it by accident.
#
# Launching the pak toggles between:
#
#   armed     stubs installed + force flag set -> every launch of a supported
#             system routes through the shim, with no session, so it should be
#             indistinguishable from stock
#   off       stubs removed, force flag cleared, nothing left behind
#
# Everything it does is written to $LOGS_PATH/netplay.txt.

DIR="$(dirname "$0")"
cd "$DIR" || exit 1

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"
: "${LOGS_PATH:=$SDCARD_PATH/.userdata/$PLATFORM/logs}"

export SDCARD_PATH PLATFORM SYSTEM_PATH

FORCE="$DIR/state/force-shim"
LOG="$LOGS_PATH/netplay.txt"

mkdir -p "$DIR/state" "$LOGS_PATH"

{
	echo "=== netplay shim test build - $(date 2>/dev/null) ==="
	echo "platform=$PLATFORM system=$SYSTEM_PATH"

	if [ -f "$FORCE" ]; then
		echo "--- disarming"
		rm -f "$FORCE"
		./launcher/install-stubs.sh uninstall
		# Staged per-core copies of the shim, recreated on demand at launch.
		rm -rf "$DIR/cores"
		echo
		echo "off. Launches are stock again."
	else
		echo "--- arming"
		./launcher/install-stubs.sh install
		: > "$FORCE"
		echo
		./launcher/install-stubs.sh status
		echo
		echo "armed. Launch a supported game; it should play exactly as before."
		echo "Check its own log for a line reading:"
		echo "    [netplay-shim] wrapping <core> (session=none)"
		echo "Relaunch this pak to disarm."
	fi

	echo "=== done ==="
} >> "$LOG" 2>&1
