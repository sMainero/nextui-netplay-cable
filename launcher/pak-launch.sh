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
SESSION="$DIR/state/session"
LOG="$LOGS_PATH/netplay.txt"

mkdir -p "$DIR/state" "$LOGS_PATH"

{
	echo "=== netplay shim test build - $(date 2>/dev/null) ==="
	echo "platform=$PLATFORM system=$SYSTEM_PATH"

	if [ -f "$FORCE" ] || [ -f "$SESSION" ]; then
		echo "--- disarming"
		rm -f "$FORCE" "$SESSION"
		./launcher/install-stubs.sh uninstall
		# Staged per-core copies of the shim, recreated on demand at launch.
		rm -rf "$DIR/cores"
		echo
		echo "off. Launches are stock again."
	else
		echo "--- arming"
		./launcher/install-stubs.sh install

		# session.conf is user-supplied: drop one in the pak dir to test link
		# play, leave it out to test passthrough. There is no UI yet, so this
		# is how a session gets described.
		if [ -f "$DIR/session.conf" ]; then
			cp "$DIR/session.conf" "$SESSION"
			echo
			echo "link session:"
			sed 's/^/    /' "$SESSION"
		else
			: > "$FORCE"
			echo
			echo "no session.conf - arming passthrough only"
		fi

		echo
		./launcher/install-stubs.sh status
		echo

		if [ -f "$SESSION" ]; then
			echo "armed for link. Start the same game on both devices."
			echo "Expect in the game's own log:"
			echo "    [netlink] connected as <role> (client_id N)"
			echo "    [netplay-shim] netpacket session started"
		else
			echo "armed. Launch a supported game; it should play exactly as before."
			echo "Expect in the game's own log:"
			echo "    [netplay-shim] wrapping <core> (session=none)"
		fi
		echo "Relaunch this pak to disarm."
	fi

	echo "=== done ==="
} >> "$LOG" 2>&1
